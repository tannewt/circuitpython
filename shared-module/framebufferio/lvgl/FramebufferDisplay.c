// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2018 Scott Shawcroft for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2020 Jeff Epler for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "shared-bindings/framebufferio/FramebufferDisplay.h"

#include "py/gc.h"
#include "py/runtime.h"
#include "shared-bindings/microcontroller/Pin.h"
#include "shared-bindings/time/__init__.h"
#include "shared-module/displayio/__init__.h"
#include "shared-module/displayio/display_core.h"
#include "supervisor/shared/display.h"
#include "supervisor/shared/tick.h"

#include "shared-module/displayio/lvgl/lvgl_internal.h"
#include "supervisor/port_heap.h"

#if CIRCUITPY_ESPIDF
#include "supervisor/lvgl_port.h"
#include <esp_log.h>
#endif

#include "lvgl.h"

// Port allocations must be aligned enough for LVGL draw buffers and the PPA.
MP_STATIC_ASSERT(MICROPY_BYTES_PER_GC_BLOCK % LV_DRAW_BUF_ALIGN == 0);
// LVGL renamed lv_draw_sw_rotate() to lv_draw_rotate(); accept either header set.
#if __has_include("lvgl/draw/lv_draw_utils.h")
#include "lvgl/draw/lv_draw_utils.h"
#define CP_LV_DRAW_ROTATE lv_draw_rotate
#else
#include "lvgl/draw/sw/lv_draw_sw_utils.h"
#undef lv_draw_sw_rotate
#define CP_LV_DRAW_ROTATE lv_draw_sw_rotate
#endif

#if CIRCUITPY_TINYUSB
#include "supervisor/usb.h"
#endif

#include <stdint.h>
#include <string.h>

#define fb_getter_default(method, default_value) \
    (self->framebuffer_protocol->method \
        ? self->framebuffer_protocol->method(self->framebuffer) \
        : (default_value))


// Rendering directly into a display's scanout framebuffer while its DMA is active can
// cause tearing or corruption on some controllers. Use the aligned partial buffer path
// by default; enable this only after validating direct rendering on a given port.
#define FRAMEBUFFERIO_LVGL_DIRECT 0

// Height of the aligned draw buffer used when the provided framebuffer can't be
// rendered into directly (for example, because it isn't aligned for LVGL's draw units).
#define FRAMEBUFFERIO_LVGL_PARTIAL_LINES 20

#define MARK_ROW_DIRTY(r) (self->lv_dirty_row_bitmask[(r) / 8] |= (1 << ((r) & 7)))

static lv_color_format_t framebufferio_lvgl_color_format(framebufferio_framebufferdisplay_obj_t *self) {
    switch (self->core.colorspace.depth) {
        case 16:
            return self->core.colorspace.reverse_bytes_in_word ? LV_COLOR_FORMAT_RGB565_SWAPPED : LV_COLOR_FORMAT_RGB565;
        case 24:
            return LV_COLOR_FORMAT_RGB888;
        case 32:
            return LV_COLOR_FORMAT_ARGB8888;
        case 8:
            return LV_COLOR_FORMAT_L8;
        case 4:
            return LV_COLOR_FORMAT_I4;
        case 2:
            return LV_COLOR_FORMAT_I2;
        case 1:
            return LV_COLOR_FORMAT_I1;
        default:
            return LV_COLOR_FORMAT_RGB565;
    }
}

static bool framebufferio_lvgl_can_render_direct(framebufferio_framebufferdisplay_obj_t *self) {
    // Rotation is handled while flushing, which requires an intermediate buffer.
    if (self->core.rotation != 0) {
        return false;
    }
    uint8_t depth = self->core.colorspace.depth;
    if (depth < 8 || depth % 8 != 0) {
        return false;
    }
    size_t bytes_per_pixel = depth / 8;
    if (self->row_stride != self->core.width * bytes_per_pixel ||
        self->row_stride != lv_draw_buf_width_to_stride(self->core.width, self->lv_color_format)) {
        return false;
    }
    if (self->bufinfo.buf == NULL) {
        return false;
    }
    // PPA also requires its output buffer size to be cache-line aligned.
    if ((self->row_stride * self->core.height) % LV_DRAW_BUF_ALIGN != 0) {
        return false;
    }
    uint8_t *framebuffer = (uint8_t *)self->bufinfo.buf + self->first_pixel_offset;
    return lv_draw_buf_align(framebuffer, self->lv_color_format) == framebuffer;
}

static void framebufferio_lvgl_free_partial_buf(framebufferio_framebufferdisplay_obj_t *self) {
    if (self->lv_buf != NULL) {
        port_free(self->lv_buf);
        self->lv_buf = NULL;
        self->lv_buf_size = 0;
    }
}

static bool framebufferio_lvgl_ensure_partial_buf(framebufferio_framebufferdisplay_obj_t *self) {
    if (self->lv_buf != NULL) {
        return true;
    }
    uint16_t max_width = self->core.width > self->core.height ? self->core.width : self->core.height;
    size_t lines = FRAMEBUFFERIO_LVGL_PARTIAL_LINES;
    if (lines > max_width) {
        lines = max_width;
    }
    size_t buf_size = lv_draw_buf_width_to_stride(max_width, self->lv_color_format) * lines;
    // The PPA draw unit requires its output buffer size to be cache-line aligned too.
    buf_size = (buf_size + LV_DRAW_BUF_ALIGN - 1) & ~(LV_DRAW_BUF_ALIGN - 1);
    self->lv_buf = port_malloc(buf_size, false);
    if (self->lv_buf == NULL) {
        return false;
    }
    self->lv_buf_size = buf_size;
    return true;
}

static bool framebufferio_lvgl_configure_buffers(framebufferio_framebufferdisplay_obj_t *self) {
    if (FRAMEBUFFERIO_LVGL_DIRECT && framebufferio_lvgl_can_render_direct(self)) {
        framebufferio_lvgl_free_partial_buf(self);
        uint8_t *framebuffer = (uint8_t *)self->bufinfo.buf + self->first_pixel_offset;
        lv_display_set_buffers(self->lv_display, framebuffer, NULL,
            self->row_stride * self->core.height, LV_DISPLAY_RENDER_MODE_DIRECT);
        self->lv_direct = true;
        return true;
    }

    if (!framebufferio_lvgl_ensure_partial_buf(self)) {
        return false;
    }
    lv_display_set_buffers(self->lv_display, self->lv_buf, NULL, self->lv_buf_size,
        LV_DISPLAY_RENDER_MODE_PARTIAL);
    self->lv_direct = false;
    return true;
}

static void framebufferio_lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    framebufferio_framebufferdisplay_obj_t *self = lv_display_get_user_data(disp);
    lv_color_format_t color_format = lv_display_get_color_format(disp);
    lv_display_rotation_t rotation = lv_display_get_rotation(disp);
    uint16_t width = lv_area_get_width(area);
    uint16_t height = lv_area_get_height(area);
    size_t bytes_per_pixel = (self->core.colorspace.depth + 7) / 8;
    uint8_t *framebuffer = (uint8_t *)self->bufinfo.buf + self->first_pixel_offset;

    if (rotation == LV_DISPLAY_ROTATION_0) {
        if (!self->lv_direct) {
            size_t source_stride = lv_draw_buf_width_to_stride(width, color_format);
            size_t row_bytes = (width * self->core.colorspace.depth + 7) / 8;
            for (uint16_t y = 0; y < height; y++) {
                uint8_t *dest = framebuffer + (area->y1 + y) * self->row_stride + area->x1 * bytes_per_pixel;
                memcpy(dest, px_map + y * source_stride, row_bytes);
            }
        }
        for (uint16_t y = area->y1; y <= area->y2; y++) {
            MARK_ROW_DIRTY(y);
        }
    } else {
        lv_area_t rotated_area = *area;
        lv_display_rotate_area(disp, &rotated_area);
        size_t source_stride = lv_draw_buf_width_to_stride(width, color_format);
        uint8_t *dest = framebuffer + rotated_area.y1 * self->row_stride + rotated_area.x1 * bytes_per_pixel;
        CP_LV_DRAW_ROTATE(px_map, dest, width, height, source_stride, self->row_stride, rotation, color_format);
        for (uint16_t y = rotated_area.y1; y <= rotated_area.y2; y++) {
            MARK_ROW_DIRTY(y);
        }
    }

    // Publish each flushed area now. LVGL doesn't reliably report the last flush part
    // in all port/display combinations, and relying on it can leave new pixels in the
    // CPU cache forever.
    bool last = lv_display_flush_is_last(disp);
    (void)last;
    #if CIRCUITPY_ESPIDF
    static uint32_t flush_count;
    if ((flush_count++ & 0x3FF) == 0) {
        ESP_LOGI("LVGL", "FramebufferDisplay flush %lu %ux%u at %d,%d last=%d disp=%p",
            (unsigned long)flush_count, width, height, area->x1, area->y1, last, disp);
    }
    #endif
    // Some LVGL/port combinations don't set the "last part" flag, so publish every
    // flushed area instead of relying on lv_display_flush_is_last() alone.
    self->framebuffer_protocol->swapbuffers(self->framebuffer, self->lv_dirty_row_bitmask);
    memset(self->lv_dirty_row_bitmask, 0, self->lv_dirty_row_bitmask_size);
    lv_display_flush_ready(disp);
}


void common_hal_framebufferio_framebufferdisplay_construct(framebufferio_framebufferdisplay_obj_t *self,
    mp_obj_t framebuffer,
    uint16_t rotation,
    bool auto_refresh) {
    // Turn off auto-refresh as we init.
    self->auto_refresh = false;
    self->framebuffer = framebuffer;
    self->framebuffer_protocol = mp_proto_get_or_throw(MP_QSTR_protocol_framebuffer, framebuffer);
    self->lv_display = NULL;
    self->lv_color_format = LV_COLOR_FORMAT_UNKNOWN;
    self->lv_buf = NULL;
    self->lv_buf_size = 0;
    self->lv_dirty_row_bitmask = NULL;
    self->lv_dirty_row_bitmask_size = 0;
    self->lv_direct = false;

    uint16_t depth = fb_getter_default(get_color_depth, 16);
    displayio_display_core_construct(
        &self->core,
        self->framebuffer_protocol->get_width(self->framebuffer),
        self->framebuffer_protocol->get_height(self->framebuffer),
        0, // rotation
        depth,
        fb_getter_default(get_grayscale, (depth < 8)),
        fb_getter_default(get_pixels_in_byte_share_row, false),
        fb_getter_default(get_bytes_per_cell, 2),
        fb_getter_default(get_reverse_pixels_in_byte, false),
        fb_getter_default(get_reverse_pixels_in_word, false)
        );

    self->first_pixel_offset = fb_getter_default(get_first_pixel_offset, 0);
    self->row_stride = fb_getter_default(get_row_stride, 0);
    if (self->row_stride == 0) {
        self->row_stride = self->core.width * self->core.colorspace.depth / 8;
    }

    self->framebuffer_protocol->get_bufinfo(self->framebuffer, &self->bufinfo);
    size_t framebuffer_size = self->first_pixel_offset + self->row_stride * (self->core.height - 1) + self->core.width * self->core.colorspace.depth / 8;

    mp_arg_validate_length_min(self->bufinfo.len, framebuffer_size, MP_QSTR_framebuffer);

    self->first_manual_refresh = !auto_refresh;

    self->native_frames_per_second = fb_getter_default(get_native_frames_per_second, 60);
    self->native_ms_per_frame = 1000 / self->native_frames_per_second;

    displayio_lvgl_port_init();

    self->lv_display = lv_display_create(self->core.width, self->core.height);
    if (self->lv_display == NULL) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("LVGL display create failed"));
    }
    lv_display_set_user_data(self->lv_display, self);
    lv_display_set_default(self->lv_display);
    displayio_lvgl_register_display(self->lv_display);
    self->lv_color_format = framebufferio_lvgl_color_format(self);
    lv_display_set_color_format(self->lv_display, self->lv_color_format);
    lv_display_set_flush_cb(self->lv_display, framebufferio_lvgl_flush_cb);
    uint16_t max_dimension = self->core.width > self->core.height ? self->core.width : self->core.height;
    self->lv_dirty_row_bitmask_size = (max_dimension + 7) / 8;
    self->lv_dirty_row_bitmask = port_malloc(self->lv_dirty_row_bitmask_size, false);
    if (self->lv_dirty_row_bitmask == NULL) {
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("LVGL dirty row alloc failed"));
    }
    memset(self->lv_dirty_row_bitmask, 0, self->lv_dirty_row_bitmask_size);
    displayio_lvgl_init();
    if (!framebufferio_lvgl_configure_buffers(self)) {
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("LVGL buffer alloc failed"));
    }
    #if CIRCUITPY_ESPIDF
    ESP_LOGI("LVGL", "FramebufferDisplay %ux%u depth %u (%s)",
        self->core.width, self->core.height, self->core.colorspace.depth,
        self->lv_direct ? "direct" : "partial");
    #endif

    if (rotation != 0) {
        common_hal_framebufferio_framebufferdisplay_set_rotation(self, rotation);
    }

    // Set the group after initialization otherwise we may send pixels while we delay in
    // initialization.
    displayio_display_core_set_root_group(&self->core, &circuitpython_splash);
    displayio_lvgl_set_root_group(&circuitpython_splash);
    common_hal_framebufferio_framebufferdisplay_set_auto_refresh(self, auto_refresh);
}

uint16_t common_hal_framebufferio_framebufferdisplay_get_width(framebufferio_framebufferdisplay_obj_t *self) {
    return displayio_display_core_get_width(&self->core);
}

uint16_t common_hal_framebufferio_framebufferdisplay_get_height(framebufferio_framebufferdisplay_obj_t *self) {
    return displayio_display_core_get_height(&self->core);
}

mp_float_t common_hal_framebufferio_framebufferdisplay_get_brightness(framebufferio_framebufferdisplay_obj_t *self) {
    if (self->framebuffer_protocol->get_brightness) {
        return self->framebuffer_protocol->get_brightness(self->framebuffer);
    }
    return -1;
}

bool common_hal_framebufferio_framebufferdisplay_set_brightness(framebufferio_framebufferdisplay_obj_t *self, mp_float_t brightness) {
    bool ok = false;
    if (self->framebuffer_protocol->set_brightness) {
        self->framebuffer_protocol->set_brightness(self->framebuffer, brightness);
        ok = true;
    }
    return ok;
}

mp_obj_t common_hal_framebufferio_framebufferdisplay_get_framebuffer(framebufferio_framebufferdisplay_obj_t *self) {
    return self->framebuffer;
}


void common_hal_framebufferio_framebufferdisplay_set_rotation(framebufferio_framebufferdisplay_obj_t *self, int rotation) {
    bool transposed = (self->core.rotation == 90 || self->core.rotation == 270);
    bool will_transposed = (rotation == 90 || rotation == 270);
    if (transposed != will_transposed) {
        int tmp = self->core.width;
        self->core.width = self->core.height;
        self->core.height = tmp;
    }
    displayio_display_core_set_rotation(&self->core, rotation);
    if (self->lv_display != NULL) {
        lv_display_set_rotation(self->lv_display, (lv_display_rotation_t)(rotation / 90));
        if (!framebufferio_lvgl_configure_buffers(self)) {
            mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("LVGL buffer alloc failed"));
        }
        lv_display_set_default(self->lv_display);
        if (self->core.current_group != NULL) {
            displayio_lvgl_set_root_group(self->core.current_group);
        }
    }
}

uint16_t common_hal_framebufferio_framebufferdisplay_get_rotation(framebufferio_framebufferdisplay_obj_t *self) {
    return self->core.rotation;
}


bool common_hal_framebufferio_framebufferdisplay_refresh(framebufferio_framebufferdisplay_obj_t *self, uint32_t target_ms_per_frame, uint32_t maximum_ms_per_real_frame) {
    if (!self->auto_refresh && !self->first_manual_refresh && (target_ms_per_frame != NO_FPS_LIMIT)) {
        uint64_t current_time = supervisor_ticks_ms64();
        uint32_t current_ms_since_real_refresh = current_time - self->core.last_refresh;
        // Test to see if the real frame time is below our minimum.
        if (current_ms_since_real_refresh > maximum_ms_per_real_frame) {
            mp_raise_RuntimeError(MP_ERROR_TEXT("Below minimum frame rate"));
        }
        uint32_t current_ms_since_last_call = current_time - self->last_refresh_call;
        self->last_refresh_call = current_time;
        // Skip the actual refresh to help catch up.
        if (current_ms_since_last_call > target_ms_per_frame) {
            return false;
        }
        uint32_t remaining_time = target_ms_per_frame - (current_ms_since_real_refresh % target_ms_per_frame);
        // We're ahead of the game so wait until we align with the frame rate.
        while (supervisor_ticks_ms64() - self->last_refresh_call < remaining_time) {
            RUN_BACKGROUND_TASKS;
        }
    }
    self->first_manual_refresh = false;
    if (self->lv_display != NULL) {
        // The renderer refreshes on its own thread too, so this has to exclude it.
        displayio_lvgl_lock();
        lv_refr_now(self->lv_display);
        displayio_lvgl_unlock();
    }
    self->core.last_refresh = supervisor_ticks_ms64();
    return true;
}

bool common_hal_framebufferio_framebufferdisplay_get_auto_refresh(framebufferio_framebufferdisplay_obj_t *self) {
    return self->auto_refresh;
}

void common_hal_framebufferio_framebufferdisplay_set_auto_refresh(framebufferio_framebufferdisplay_obj_t *self,
    bool auto_refresh) {
    self->first_manual_refresh = !auto_refresh;
    // No supervisor_enable/disable_tick() here: under LVGL that would stop the whole supervisor
    // background tick (filesystem, USB, BLE, port work). The port's LVGL pump asks
    // displayio_lvgl_auto_refresh() instead, so this flag only decides whether the compositor
    // repaints on its own.
    self->auto_refresh = auto_refresh;
}

static void _update_backlight(framebufferio_framebufferdisplay_obj_t *self) {
    // TODO(tannewt): Fade the backlight based on it's existing value and a target value. The target
    // should account for ambient light when possible.
}

void framebufferio_framebufferdisplay_background(framebufferio_framebufferdisplay_obj_t *self) {
    _update_backlight(self);

    if (self->auto_refresh) {
        lv_timer_handler();
    }
}

void release_framebufferdisplay(framebufferio_framebufferdisplay_obj_t *self) {
    common_hal_framebufferio_framebufferdisplay_set_auto_refresh(self, false);
    displayio_lvgl_set_root_group(NULL);
    if (self->lv_display != NULL) {
        lv_display_delete(self->lv_display);
        self->lv_display = NULL;
    }
    framebufferio_lvgl_free_partial_buf(self);
    if (self->lv_dirty_row_bitmask != NULL) {
        port_free(self->lv_dirty_row_bitmask);
        self->lv_dirty_row_bitmask = NULL;
        self->lv_dirty_row_bitmask_size = 0;
    }
    release_display_core(&self->core);
    self->framebuffer_protocol->deinit(self->framebuffer);
    self->base.type = &mp_type_NoneType;
}

void framebufferio_framebufferdisplay_collect_ptrs(framebufferio_framebufferdisplay_obj_t *self) {
    gc_collect_ptr(self->framebuffer);
    displayio_display_core_collect_ptrs(&self->core);
}

void framebufferio_framebufferdisplay_reset(framebufferio_framebufferdisplay_obj_t *self) {
    const mp_obj_type_t *fb_type = mp_obj_get_type(self->framebuffer);
    if (fb_type != NULL && fb_type != &mp_type_NoneType) {
        common_hal_framebufferio_framebufferdisplay_set_auto_refresh(self, true);
        displayio_display_core_set_root_group(&self->core, &circuitpython_splash);
        if (self->lv_display != NULL) {
            lv_display_set_default(self->lv_display);
        }
        displayio_lvgl_set_root_group(&circuitpython_splash);
    } else {
        release_framebufferdisplay(self);
    }
}

mp_obj_t common_hal_framebufferio_framebufferdisplay_get_root_group(framebufferio_framebufferdisplay_obj_t *self) {
    if (self->core.current_group == NULL) {
        return mp_const_none;
    }
    return self->core.current_group;
}

mp_obj_t common_hal_framebufferio_framebufferdisplay_set_root_group(framebufferio_framebufferdisplay_obj_t *self, displayio_group_t *root_group) {
    bool ok = displayio_display_core_set_root_group(&self->core, root_group);
    if (!ok) {
        mp_raise_ValueError(MP_ERROR_TEXT("Group already used"));
    }
    lv_display_set_default(self->lv_display);
    displayio_lvgl_set_root_group(root_group);
    return mp_const_none;
}
