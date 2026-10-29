// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2018 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// LVGL-based displayio Group implementation. Each Group is mirrored into a plain lv_obj
// container; members are appended as children so the lv object tree's child order matches
// the displayio z-order (later members draw on top). Group scale is applied by scaling
// child positions and images directly, avoiding LVGL's transform layers (which route
// through a premultiplied intermediate buffer and lose sub-255 precision).
//
// The compositor pushes every state change into the LVGL mirror synchronously, under the
// port's LVGL lock: LVGL itself renders on an RTOS thread (e.g. a Zephyr workqueue), and
// this keeps tree mutation off the render thread.

#include "shared-bindings/displayio/Group.h"

#include <stdio.h>
#include <string.h>

#include "py/gc.h"
#include "py/runtime.h"
#include "py/objlist.h"
#include "shared-bindings/displayio/TileGrid.h"
#include "shared-module/displayio/__init__.h"
#include "shared-module/displayio/lvgl/lvgl_internal.h"

#if CIRCUITPY_VECTORIO
#include "shared-bindings/vectorio/VectorShape.h"
#endif

#if CIRCUITPY_DISPLAYIO_LVGL

static displayio_group_t *cp_lvgl_root_group = NULL;
static bool cp_lvgl_inited = false;
static uint8_t lvgl_lock_depth = 0;

void displayio_lvgl_lock(void) {
    if (lvgl_lock_depth == 0) {
        displayio_lvgl_port_lock();
    }
    lvgl_lock_depth++;
}

void displayio_lvgl_unlock(void) {
    if (lvgl_lock_depth == 0) {
        return;
    }
    lvgl_lock_depth--;
    if (lvgl_lock_depth == 0) {
        displayio_lvgl_port_unlock();
    }
}

// Recompute children's inherited scale and positions after this group's scale or
// position changed.
static void _update_children(displayio_group_t *self) {
    uint16_t child_scale = self->lv_scale * self->scale;
    for (size_t i = 0; i < self->members->len; i++) {
        mp_obj_t layer = self->members->items[i];
        mp_obj_t native = mp_obj_cast_to_native_base(layer, &displayio_tilegrid_type);
        if (native != MP_OBJ_NULL) {
            displayio_tilegrid_t *tilegrid = native;
            tilegrid->lv_scale = child_scale;
            if (tilegrid->lv_obj != NULL) {
                lv_obj_set_pos(tilegrid->lv_obj, tilegrid->x * child_scale,
                    tilegrid->y * child_scale);
                displayio_lvgl_tilegrid_update_scale(tilegrid);
            }
            continue;
        }
        native = mp_obj_cast_to_native_base(layer, &displayio_group_type);
        if (native != MP_OBJ_NULL) {
            displayio_group_t *group = native;
            group->lv_scale = child_scale;
            group->lv_abs_x = self->lv_abs_x + group->x * child_scale;
            group->lv_abs_y = self->lv_abs_y + group->y * child_scale;
            if (group->lv_obj != NULL) {
                lv_obj_set_pos(group->lv_obj, group->x * child_scale,
                    group->y * child_scale);
                _update_children(group);
            }
            continue;
        }
        #if CIRCUITPY_VECTORIO
        native = mp_obj_cast_to_native_base(layer, &vectorio_vector_shape_type);
        if (native != MP_OBJ_NULL) {
            vectorio_vector_shape_t *shape = native;
            displayio_lvgl_vector_shape_set_transform(shape,
                self->lv_abs_x + shape->x * child_scale,
                self->lv_abs_y + shape->y * child_scale,
                child_scale);
        }
        #endif
    }
}

static void check_readonly(displayio_group_t *self) {
    if (self->readonly) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("Read-only"));
    }
}

void displayio_lvgl_init(void) {
    printf("[CP-LVGL] init called\n");
    if (cp_lvgl_inited) {
        return;
    }
    displayio_lvgl_lock();
    lv_obj_t *screen = lv_screen_active();
    if (screen != NULL) {
        // Match the legacy compositor, where undrawn pixels are zero (black).
        lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
        displayio_lvgl_register_image_decoder();
        cp_lvgl_inited = true;
    }
    displayio_lvgl_unlock();
}

void displayio_group_construct(displayio_group_t *self, mp_obj_list_t *members, uint32_t scale, mp_int_t x, mp_int_t y) {
    self->x = x;
    self->y = y;
    self->members = members;
    self->item_removed = false;
    self->scale = scale;
    self->in_group = false;
    self->readonly = false;
    self->lv_obj = NULL;
    self->lv_scale = 1;
    self->lv_abs_x = x;
    self->lv_abs_y = y;
}

void common_hal_displayio_group_construct(displayio_group_t *self, uint32_t scale, mp_int_t x, mp_int_t y) {
    mp_obj_list_t *members = mp_obj_new_list(0, NULL);
    displayio_group_construct(self, members, scale, x, y);
}

bool common_hal_displayio_group_get_hidden(displayio_group_t *self) {
    return self->hidden;
}

// Update the mirror object for a group's effective visibility.
static void _update_lvgl_visible(displayio_group_t *self) {
    if (self->lv_obj == NULL) {
        return;
    }
    if (self->hidden || self->hidden_by_parent) {
        lv_obj_add_flag(self->lv_obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(self->lv_obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void _propagate_hidden(displayio_group_t *self, bool hidden) {
    for (size_t i = 0; i < self->members->len; i++) {
        mp_obj_t layer;
        layer = mp_obj_cast_to_native_base(self->members->items[i], &displayio_tilegrid_type);
        if (layer != MP_OBJ_NULL) {
            displayio_tilegrid_set_hidden_by_parent(layer, hidden);
            continue;
        }
        layer = mp_obj_cast_to_native_base(self->members->items[i], &displayio_group_type);
        if (layer != MP_OBJ_NULL) {
            displayio_group_set_hidden_by_parent(layer, hidden);
            continue;
        }
        #if CIRCUITPY_VECTORIO
        layer = mp_obj_cast_to_native_base(self->members->items[i], &vectorio_vector_shape_type);
        if (layer != MP_OBJ_NULL) {
            displayio_lvgl_vector_shape_set_hidden_by_parent(layer, hidden);
        }
        #endif
    }
}

void common_hal_displayio_group_set_hidden(displayio_group_t *self, bool hidden) {
    if (self->hidden == hidden) {
        return;
    }
    check_readonly(self);
    self->hidden = hidden;
    if (self->hidden_by_parent) {
        return;
    }
    _propagate_hidden(self, hidden);
    displayio_lvgl_lock();
    _update_lvgl_visible(self);
    displayio_lvgl_unlock();
}

void displayio_group_set_hidden_by_parent(displayio_group_t *self, bool hidden) {
    if (self->hidden_by_parent == hidden) {
        return;
    }
    check_readonly(self);
    self->hidden_by_parent = hidden;
    if (self->hidden) {
        return;
    }
    _propagate_hidden(self, hidden);
    displayio_lvgl_lock();
    _update_lvgl_visible(self);
    displayio_lvgl_unlock();
}

uint32_t common_hal_displayio_group_get_scale(displayio_group_t *self) {
    return self->scale;
}

void common_hal_displayio_group_set_scale(displayio_group_t *self, uint32_t scale) {
    if (self->scale == scale) {
        return;
    }
    check_readonly(self);
    self->scale = scale;
    if (self->lv_obj != NULL) {
        _update_children(self);
    }
}

mp_int_t common_hal_displayio_group_get_x(displayio_group_t *self) {
    return self->x;
}

void common_hal_displayio_group_set_x(displayio_group_t *self, mp_int_t x) {
    if (self->x == x) {
        return;
    }
    check_readonly(self);
    self->x = x;
    if (self->lv_obj != NULL) {
        displayio_lvgl_lock();
        lv_obj_set_pos(self->lv_obj, self->x, self->y);
        // Children's absolute positions moved with us; vectorio's contains() needs them.
        _update_children(self);
        displayio_lvgl_unlock();
    }
}

mp_int_t common_hal_displayio_group_get_y(displayio_group_t *self) {
    return self->y;
}

void common_hal_displayio_group_set_y(displayio_group_t *self, mp_int_t y) {
    if (self->y == y) {
        return;
    }
    check_readonly(self);
    self->y = y;
    if (self->lv_obj != NULL) {
        displayio_lvgl_lock();
        lv_obj_set_pos(self->lv_obj, self->x, self->y);
        _update_children(self);
        displayio_lvgl_unlock();
    }
}

// Resolve a group member to its mirror object, or NULL if it has none yet.
static lv_obj_t *_member_mirror(mp_obj_t layer) {
    mp_obj_t native = mp_obj_cast_to_native_base(layer, &displayio_tilegrid_type);
    if (native != MP_OBJ_NULL) {
        return ((displayio_tilegrid_t *)native)->lv_obj;
    }
    native = mp_obj_cast_to_native_base(layer, &displayio_group_type);
    if (native != MP_OBJ_NULL) {
        return ((displayio_group_t *)native)->lv_obj;
    }
    #if CIRCUITPY_VECTORIO
    native = mp_obj_cast_to_native_base(layer, &vectorio_vector_shape_type);
    if (native != MP_OBJ_NULL) {
        return ((vectorio_vector_shape_t *)native)->lv_obj;
    }
    #endif
    return NULL;
}

// Create the mirror for a member that doesn't have one yet. Unsupported layers stay NULL.
static lv_obj_t *_attach_member(displayio_group_t *self, mp_obj_t layer) {
    mp_obj_t native = mp_obj_cast_to_native_base(layer, &displayio_tilegrid_type);
    if (native != MP_OBJ_NULL) {
        displayio_lvgl_tilegrid_attach((displayio_tilegrid_t *)native, self->lv_obj);
        return ((displayio_tilegrid_t *)native)->lv_obj;
    }
    native = mp_obj_cast_to_native_base(layer, &displayio_group_type);
    if (native != MP_OBJ_NULL) {
        displayio_lvgl_group_attach((displayio_group_t *)native, self->lv_obj);
        return ((displayio_group_t *)native)->lv_obj;
    }
    #if CIRCUITPY_VECTORIO
    native = mp_obj_cast_to_native_base(layer, &vectorio_vector_shape_type);
    if (native != MP_OBJ_NULL) {
        displayio_lvgl_vector_shape_attach((vectorio_vector_shape_t *)native, self->lv_obj);
        return ((vectorio_vector_shape_t *)native)->lv_obj;
    }
    #endif
    return NULL;
}

// Adopt a member: validate that it can join, update Python-visible state, and push it
// into the mirror if the group is mirrored.
static void _adopt_member(displayio_group_t *self, mp_obj_t layer) {
    mp_obj_t native_layer = mp_obj_cast_to_native_base(layer, &displayio_tilegrid_type);
    if (native_layer != MP_OBJ_NULL) {
        displayio_tilegrid_t *tilegrid = native_layer;
        if (tilegrid->in_group) {
            mp_raise_ValueError(MP_ERROR_TEXT("Layer already in a group"));
        }
        tilegrid->in_group = true;
        displayio_tilegrid_set_hidden_by_parent(tilegrid, self->hidden || self->hidden_by_parent);
        return;
    }
    native_layer = mp_obj_cast_to_native_base(layer, &displayio_group_type);
    if (native_layer != MP_OBJ_NULL) {
        displayio_group_t *group = native_layer;
        if (group->in_group) {
            mp_raise_ValueError(MP_ERROR_TEXT("Layer already in a group"));
        }
        group->in_group = true;
        displayio_group_set_hidden_by_parent(group, self->hidden || self->hidden_by_parent);
        return;
    }
    #if CIRCUITPY_VECTORIO
    native_layer = mp_obj_cast_to_native_base(layer, &vectorio_vector_shape_type);
    if (native_layer != MP_OBJ_NULL) {
        vectorio_vector_shape_t *shape = native_layer;
        if (vectorio_vector_shape_set_in_group(shape, true)) {
            mp_raise_ValueError(MP_ERROR_TEXT("Layer already in a group"));
        }
        displayio_lvgl_vector_shape_set_hidden_by_parent(shape,
            self->hidden || self->hidden_by_parent);
        return;
    }
    #endif
    mp_raise_ValueError(MP_ERROR_TEXT("Layer must be a Group, TileGrid or vector shape"));
}

// Release a member and remove its mirror. Must be called under the displayio lock.
static void _release_member(displayio_group_t *self, mp_obj_t layer) {
    mp_obj_t native_layer = mp_obj_cast_to_native_base(layer, &displayio_tilegrid_type);
    if (native_layer != MP_OBJ_NULL) {
        displayio_tilegrid_t *tilegrid = native_layer;
        tilegrid->in_group = false;
        displayio_lvgl_tilegrid_detach(tilegrid);
        return;
    }
    native_layer = mp_obj_cast_to_native_base(layer, &displayio_group_type);
    if (native_layer != MP_OBJ_NULL) {
        displayio_group_t *group = native_layer;
        group->in_group = false;
        displayio_lvgl_group_detach(group);
        return;
    }
    #if CIRCUITPY_VECTORIO
    native_layer = mp_obj_cast_to_native_base(layer, &vectorio_vector_shape_type);
    if (native_layer != MP_OBJ_NULL) {
        vectorio_vector_shape_t *shape = native_layer;
        vectorio_vector_shape_set_in_group(shape, false);
        displayio_lvgl_vector_shape_detach(shape);
    }
    #endif
}

// Push the mirror children into the same order as the members list. Newly added members
// get mirrors here; children that no longer belong to a member are deleted. Must be
// called under the displayio lock.
static void _sync_members(displayio_group_t *self) {
    if (self->lv_obj == NULL) {
        return;
    }
    size_t child_index = 0;
    for (size_t i = 0; i < self->members->len; i++) {
        mp_obj_t layer = self->members->items[i];
        lv_obj_t *obj = _member_mirror(layer);
        if (obj == NULL) {
            // Not mirrored yet: try to attach it now (unsupported layers stay NULL).
            obj = _attach_member(self, layer);
        }
        if (obj == NULL) {
            continue;
        }
        if (lv_obj_get_parent(obj) != self->lv_obj) {
            lv_obj_set_parent(obj, self->lv_obj);
        }
        if (lv_obj_get_index(obj) != (int32_t)child_index) {
            lv_obj_move_to_index(obj, child_index);
        }
        child_index++;
    }
    // Delete mirrors of members that were removed from the group.
    while ((uint32_t)lv_obj_get_child_count(self->lv_obj) > child_index) {
        lv_obj_t *child = lv_obj_get_child(self->lv_obj, (int32_t)(lv_obj_get_child_count(self->lv_obj) - 1));
        lv_obj_delete(child);
    }
    // Newly attached members need their position, scale and absolute offset pushed in.
    _update_children(self);
}

void common_hal_displayio_group_insert(displayio_group_t *self, size_t index, mp_obj_t layer) {
    check_readonly(self);
    _adopt_member(self, layer);
    mp_obj_list_insert(self->members, index, layer);

    if (self->lv_obj != NULL) {
        displayio_lvgl_lock();
        _sync_members(self);
        displayio_lvgl_unlock();
    }
}

mp_obj_t common_hal_displayio_group_pop(displayio_group_t *self, size_t index) {
    check_readonly(self);
    mp_obj_t layer = self->members->items[index];
    displayio_lvgl_lock();
    _release_member(self, layer);
    mp_obj_list_pop(self->members, index);
    if (self->lv_obj != NULL) {
        _sync_members(self);
    }
    displayio_lvgl_unlock();
    return layer;
}

mp_int_t common_hal_displayio_group_index(displayio_group_t *self, mp_obj_t layer) {
    mp_obj_t args[] = {self->members, layer};
    mp_obj_t *index = mp_seq_index_obj(self->members->items, self->members->len, 2, args);
    return MP_OBJ_SMALL_INT_VALUE(index);
}

size_t common_hal_displayio_group_get_len(displayio_group_t *self) {
    return self->members->len;
}

mp_obj_t common_hal_displayio_group_get(displayio_group_t *self, size_t index) {
    return self->members->items[index];
}

void common_hal_displayio_group_set(displayio_group_t *self, size_t index, mp_obj_t layer) {
    check_readonly(self);
    displayio_lvgl_lock();
    _release_member(self, self->members->items[index]);
    _adopt_member(self, layer);
    mp_obj_list_store(self->members, MP_OBJ_NEW_SMALL_INT(index), layer);
    if (self->lv_obj != NULL) {
        _sync_members(self);
    }
    displayio_lvgl_unlock();
}

void displayio_lvgl_group_attach(displayio_group_t *self, lv_obj_t *parent) {
    if (self->lv_obj == NULL) {
        self->lv_obj = lv_obj_create(parent);
        if (self->lv_obj == NULL) {
            return;
        }
        // Plain container: no theme chrome, sized to the children, positioned like
        // displayio would place it.
        lv_obj_set_style_bg_opa(self->lv_obj, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(self->lv_obj, 0, 0);
        lv_obj_set_style_pad_all(self->lv_obj, 0, 0);
        lv_obj_clear_flag(self->lv_obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(self->lv_obj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(self->lv_obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_add_flag(self->lv_obj, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        lv_obj_set_pos(self->lv_obj, self->x, self->y);
        _update_lvgl_visible(self);
    } else if (lv_obj_get_parent(self->lv_obj) != parent) {
        lv_obj_set_parent(self->lv_obj, parent);
    }
    _sync_members(self);
}

void displayio_lvgl_group_detach(displayio_group_t *self) {
    if (self->lv_obj == NULL) {
        return;
    }
    // Detach the members first so their object pointers don't dangle when the lv tree
    // is deleted below us.
    for (size_t i = 0; i < self->members->len; i++) {
        mp_obj_t layer = self->members->items[i];
        mp_obj_t native = mp_obj_cast_to_native_base(layer, &displayio_tilegrid_type);
        if (native != MP_OBJ_NULL) {
            displayio_lvgl_tilegrid_detach(native);
            continue;
        }
        native = mp_obj_cast_to_native_base(layer, &displayio_group_type);
        if (native != MP_OBJ_NULL) {
            displayio_lvgl_group_detach(native);
            continue;
        }
        #if CIRCUITPY_VECTORIO
        native = mp_obj_cast_to_native_base(layer, &vectorio_vector_shape_type);
        if (native != MP_OBJ_NULL) {
            displayio_lvgl_vector_shape_detach(native);
        }
        #endif
    }
    lv_obj_delete(self->lv_obj);
    self->lv_obj = NULL;
}

void displayio_lvgl_set_root_group(displayio_group_t *group) {
    displayio_lvgl_init();
    if (!cp_lvgl_inited) {
        // The port never created the LVGL screen (no display); nothing to mirror.
        return;
    }
    displayio_lvgl_lock();
    if (cp_lvgl_root_group == group) {
        displayio_lvgl_unlock();
        return;
    }
    if (cp_lvgl_root_group != NULL) {
        displayio_lvgl_group_detach(cp_lvgl_root_group);
    }
    cp_lvgl_root_group = group;
    if (cp_lvgl_root_group != NULL) {
        displayio_lvgl_group_attach(cp_lvgl_root_group, lv_screen_active());
    }
    displayio_lvgl_unlock();
}

void displayio_gc_collect(void) {
    void *root_ptrs[1] = { cp_lvgl_root_group };
    if (root_ptrs[0] != NULL) {
        gc_collect_root(root_ptrs, 1);
    }
    // The mirror registry holds TileGrid pointers in port memory; root them explicitly.
    displayio_lvgl_mirror_gc();
}

#endif  // CIRCUITPY_DISPLAYIO_LVGL
