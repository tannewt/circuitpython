// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2018 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// LVGL-based displayio Group implementation. Each Group is mirrored into a plain lv_obj
// container; members are appended as children so the lv object tree's child order matches
// the displayio z-order (later members draw on top). Group scale is applied by scaling
// child positions and images directly. That keeps displayio's integer nearest-neighbor
// scaling and per-child invalidation; LVGL transform styles use an ARGB8888 transform
// layer and bilinear filtering instead.
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
#include "shared-bindings/displayio/__init__.h"
#include "shared-module/displayio/__init__.h"
#include "shared-module/displayio/lvgl/lvgl_internal.h"
#include "shared-module/displayio/lvgl/lvgl_protocol.h"

#if CIRCUITPY_DISPLAYIO_LVGL

// Resolve a member's protocol and, in place, the object it implements the protocol on. Layers
// mirror themselves into the LVGL tree through protocol_lvgl, so nothing here has to know a
// member's type. See lvgl_protocol.h.
static const lvgl_protocol_t *_layer_protocol(mp_obj_t *layer) {
    const lvgl_protocol_t *protocol = mp_proto_get(MP_QSTR_protocol_lvgl, *layer);
    if (protocol != NULL && protocol->protocol_self != NULL) {
        *layer = protocol->protocol_self(*layer);
        protocol = mp_proto_get(MP_QSTR_protocol_lvgl, *layer);
    }
    return protocol;
}

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

// Groups are mirrored as transparent, screen sized LVGL objects at the screen origin and
// their members are positioned in screen coordinates (lv_abs_x/y is the group's origin on
// the screen). A group's LVGL object never moves or resizes, so moving a member only
// redraws the member's old and new area. (A group sized to its content would be redrawn
// from its origin to its farthest member whenever a member moves.)
//
// Recompute members' inherited scale and screen positions after this group's scale or
// position changed. Returns false when a member rejects the transform; some members may
// already have taken it then, so callers restore the group's own transform and push again.
static bool _update_children(displayio_group_t *self) {
    uint16_t child_scale = self->lv_scale * self->scale;
    for (size_t i = 0; i < self->members->len; i++) {
        mp_obj_t layer = self->members->items[i];
        const lvgl_protocol_t *protocol = _layer_protocol(&layer);
        if (protocol == NULL || protocol->set_transform == NULL) {
            continue;
        }
        if (!protocol->set_transform(layer, self->lv_abs_x, self->lv_abs_y, child_scale)) {
            return false;
        }
    }
    return true;
}

static void check_readonly(displayio_group_t *self) {
    if (self->readonly) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("Read-only"));
    }
}

// Bitmap writers mark the dirty area before writing, and TileGrid writes only set bits, so the
// composed buffers are brought up to date here: at the start of a refresh, after the writes and
// before anything is drawn. The lock is held, and LVGL doesn't render while this runs.
static void refresh_start_cb(lv_event_t *e) {
    (void)e;
    displayio_lvgl_tilegrid_refresh_all();
}

void displayio_lvgl_register_display(lv_display_t *display) {
    if (display == NULL) {
        return;
    }
    lv_display_add_event_cb(display, refresh_start_cb, LV_EVENT_REFR_START, NULL);
}

void displayio_lvgl_init(void) {
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
        // The OnDiskBitmap decoder has to be in place before any TileGrid attaches.
        displayio_lvgl_ondiskbitmap_decoder_init();
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
        mp_obj_t layer = self->members->items[i];
        const lvgl_protocol_t *protocol = _layer_protocol(&layer);
        if (protocol != NULL && protocol->set_hidden_by_parent != NULL) {
            protocol->set_hidden_by_parent(layer, hidden);
        }
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
    if (self->lv_obj == NULL) {
        self->scale = scale;
        return;
    }
    uint16_t previous_scale = self->scale;
    self->scale = scale;
    displayio_lvgl_lock();
    bool accepted = _update_children(self);
    if (!accepted) {
        // A member can't be shown at this scale. Put the group back the way it was, push that
        // to the members that already took the new scale, and report it.
        self->scale = previous_scale;
        _update_children(self);
    }
    displayio_lvgl_unlock();
    if (!accepted) {
        mp_raise_ValueError(MP_ERROR_TEXT("Layer does not support scaling"));
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
    // The origin is in screen pixels, so the move is scaled by the parent chain.
    self->lv_abs_x += (x - self->x) * self->lv_scale;
    self->x = x;
    if (self->lv_obj != NULL) {
        displayio_lvgl_lock();
        // Every layer can be translated; only set_scale() can be rejected.
        (void)_update_children(self);
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
    // The origin is in screen pixels, so the move is scaled by the parent chain.
    self->lv_abs_y += (y - self->y) * self->lv_scale;
    self->y = y;
    if (self->lv_obj != NULL) {
        displayio_lvgl_lock();
        // Every layer can be translated; only set_scale() can be rejected.
        (void)_update_children(self);
        displayio_lvgl_unlock();
    }
}

// Resolve a group member to its mirror object, or NULL if it has none yet.
static lv_obj_t *_member_mirror(mp_obj_t layer) {
    const lvgl_protocol_t *protocol = _layer_protocol(&layer);
    return protocol == NULL ? NULL : protocol->root_obj(layer);
}

// Create the mirror for a member that doesn't have one yet. Unsupported layers stay NULL.
static lv_obj_t *_attach_member(displayio_group_t *self, mp_obj_t layer) {
    const lvgl_protocol_t *protocol = _layer_protocol(&layer);
    if (protocol == NULL) {
        return NULL;
    }
    protocol->attach(layer, self->lv_obj);
    return protocol->root_obj(layer);
}

// Adopt a member: validate that it can join, update Python-visible state, and push it into
// the mirror if the group is mirrored.
static void _adopt_member(displayio_group_t *self, mp_obj_t layer) {
    const lvgl_protocol_t *protocol = _layer_protocol(&layer);
    if (protocol == NULL || protocol->set_in_group == NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("Layer must be a Group, TileGrid or vector shape"));
    }
    if (protocol->set_in_group(layer, true)) {
        mp_raise_ValueError(MP_ERROR_TEXT("Layer already in a group"));
    }
    // Check the inherited transform before the layer joins: a layer that can't be shown at
    // the group's current scale (a VideoBuffer, for example) is refused here instead of
    // leaving the group with a member it can't position.
    if (protocol->set_transform != NULL &&
        !protocol->set_transform(layer, self->lv_abs_x, self->lv_abs_y,
            self->lv_scale * self->scale)) {
        protocol->set_in_group(layer, false);
        mp_raise_ValueError(MP_ERROR_TEXT("Layer does not support scaling"));
    }
    if (protocol->set_hidden_by_parent != NULL) {
        protocol->set_hidden_by_parent(layer, self->hidden || self->hidden_by_parent);
    }
}

// Release a member and remove its mirror. Must be called under the displayio lock.
static void _release_member(displayio_group_t *self, mp_obj_t layer) {
    const lvgl_protocol_t *protocol = _layer_protocol(&layer);
    if (protocol == NULL) {
        return;
    }
    if (protocol->set_in_group != NULL) {
        protocol->set_in_group(layer, false);
    }
    protocol->detach(layer);
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
    // Newly attached members need their position, scale and absolute offset pushed in. Every
    // member already accepted this transform in _adopt_member().
    (void)_update_children(self);
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

// protocol_lvgl implementation. A Group is a member of another Group like any other layer;
// its own position and scale are inherited, so it recomputes its origin from the parent's.
static lv_obj_t *_group_protocol_root_obj(mp_obj_t layer) {
    return ((displayio_group_t *)MP_OBJ_TO_PTR(layer))->lv_obj;
}

static void _group_protocol_attach(mp_obj_t layer, lv_obj_t *parent) {
    displayio_lvgl_group_attach(MP_OBJ_TO_PTR(layer), parent);
}

static void _group_protocol_detach(mp_obj_t layer) {
    displayio_lvgl_group_detach(MP_OBJ_TO_PTR(layer));
}

static bool _group_protocol_set_transform(mp_obj_t layer, int32_t abs_x, int32_t abs_y,
    uint16_t scale) {
    displayio_group_t *self = MP_OBJ_TO_PTR(layer);
    self->lv_scale = scale;
    // abs_x/y is where this group's parent origin is on the screen; shift by this group's
    // own position, scaled the same way as its members are.
    self->lv_abs_x = abs_x + self->x * scale;
    self->lv_abs_y = abs_y + self->y * scale;
    if (self->lv_obj == NULL) {
        return true;
    }
    return _update_children(self);
}

static void _group_protocol_set_hidden_by_parent(mp_obj_t layer, bool hidden) {
    displayio_group_set_hidden_by_parent(MP_OBJ_TO_PTR(layer), hidden);
}

static bool _group_protocol_set_in_group(mp_obj_t layer, bool in_group) {
    displayio_group_t *self = MP_OBJ_TO_PTR(layer);
    bool was_in_group = self->in_group;
    self->in_group = in_group;
    return was_in_group;
}

const lvgl_protocol_t displayio_lvgl_group_protocol = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_lvgl)
    .root_obj = _group_protocol_root_obj,
    .attach = _group_protocol_attach,
    .detach = _group_protocol_detach,
    .set_transform = _group_protocol_set_transform,
    .set_hidden_by_parent = _group_protocol_set_hidden_by_parent,
    .set_in_group = _group_protocol_set_in_group,
};

void displayio_lvgl_group_attach(displayio_group_t *self, lv_obj_t *parent) {
    if (self->lv_obj == NULL) {
        self->lv_obj = lv_obj_create(parent);
        if (self->lv_obj == NULL) {
            return;
        }
        // Plain, transparent, screen sized container at the screen origin. See
        // _update_children() for why it doesn't follow the group's position.
        lv_obj_remove_style_all(self->lv_obj);
        lv_obj_clear_flag(self->lv_obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(self->lv_obj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(self->lv_obj, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        lv_obj_set_size(self->lv_obj, lv_pct(100), lv_pct(100));
        lv_obj_set_pos(self->lv_obj, 0, 0);
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
        const lvgl_protocol_t *protocol = _layer_protocol(&layer);
        if (protocol != NULL) {
            protocol->detach(layer);
        }
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
        // The root group's origin is its own position.
        group->lv_scale = 1;
        group->lv_abs_x = group->x;
        group->lv_abs_y = group->y;
        displayio_lvgl_group_attach(cp_lvgl_root_group, lv_screen_active());
    }
    displayio_lvgl_unlock();
}

void displayio_gc_collect(void) {
    void *root_ptrs[1] = { cp_lvgl_root_group };
    if (root_ptrs[0] != NULL) {
        gc_collect_root(root_ptrs, 1);
    }
    // The shader registry holds vectorio pointers in port memory; root them explicitly.
    displayio_lvgl_mirror_gc();
    // So does the compositor's list of attached TileGrids.
    displayio_lvgl_tilegrid_gc_collect();
    // Displays live in the static slot registry, which the GC can't see.
    displayio_display_slots_gc_collect();
}

#endif  // CIRCUITPY_DISPLAYIO_LVGL
