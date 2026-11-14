// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "bindings/zephyr_display/Display.h"

#include <string.h>

#include "bindings/zephyr_kernel/__init__.h"
#include "py/gc.h"
#include "py/runtime.h"
#include "shared-bindings/time/__init__.h"
#include "shared-module/displayio/__init__.h"
#include "supervisor/shared/display.h"
#include "supervisor/shared/tick.h"


#include <lvgl.h>
#include <lvgl_display.h>
#include <lvgl_zephyr.h>

#include "shared-bindings/displayio/Group.h"
#include "shared-module/displayio/lvgl/lvgl_internal.h"
#include "shared-bindings/displayio/__init__.h"

// Display objects live in displayio's static slot registry. Each slot owns its LVGL
// display and buffers until release_displays(); soft reload keeps the primary slot.

#if defined(CONFIG_LV_Z_RUN_LVGL_ON_WORKQUEUE)
// The Zephyr LVGL glue creates its display during SYS_INIT from the driver's current
// pixel format. If that format is monochrome, the glue's flush callback expects a
// conversion buffer that it only allocates when LVGL itself is built at 1bpp (which it
// isn't when the panel format is chosen at runtime). Pause rendering until CircuitPython
// has recreated the display with the matching buffers; running the workqueue before that
// flushes through a NULL conversion buffer.
static int zephyr_display_lvgl_pause_rendering(void) {
    lvgl_timer_handler_pause();
    return 0;
}
SYS_INIT(zephyr_display_lvgl_pause_rendering, APPLICATION, 0);
#endif

// LVGL is initialized by the Zephyr glue at boot.
void displayio_lvgl_port_init(void) {
}

// The LVGL compositor's port hooks: the RTOS renders LVGL on its workqueue thread, and
// CircuitPython-side mirror mutations take the glue's LVGL lock.
void displayio_lvgl_port_lock(void) {
    lvgl_lock();
}

void displayio_lvgl_port_unlock(void) {
    lvgl_unlock();
}

void displayio_lvgl_port_install_busdisplay_refresh(lv_display_t *display) {
    // The Zephyr glue flushes every display on the LVGL workqueue, including BusDisplay.
    LV_UNUSED(display);
}

void displayio_lvgl_port_frame_boundary(void) {
    // No Zephyr framebuffer exposes a frame callback yet. If one does, this must be made ISR
    // safe (for example by scheduling the refresh on the LVGL workqueue).
    displayio_lvgl_refresh_paced_displays();
}

static void zephyr_display_lvgl_flush_argb_alpha0(lv_display_t *display, const lv_area_t *area,
    uint8_t *px_map) {
    // The legacy compositor's transfer buffer for 32-bit displays carried no alpha
    // channel (0x00RRGGBB), which the SDL capture surfaces as its transparency grid.
    // Match that: clear the alpha byte of every pixel before handing the buffer to the
    // glue's 32-bit flush.
    uint32_t size = (uint32_t)(area->x2 - area->x1 + 1) * (uint32_t)(area->y2 - area->y1 + 1);
    for (uint32_t i = 0; i < size; i++) {
        px_map[i * 4 + 3] = 0x00;
    }
    lvgl_flush_cb_32bit(display, area, px_map);
}

static void zephyr_display_lvgl_flush_565x(lv_display_t *display, const lv_area_t *area,
    uint8_t *px_map) {
    // The driver for RGB_565X panels reads big-endian pixels. Keep LVGL's buffer in
    // plain RGB565 and swap on the way out (mirrors the legacy colorspace-level swap).
    uint32_t size = (uint32_t)(area->x2 - area->x1 + 1) * (uint32_t)(area->y2 - area->y1 + 1);
    uint16_t *px = (uint16_t *)px_map;
    for (uint32_t i = 0; i < size; i++) {
        px[i] = (uint16_t)((px[i] << 8) | (px[i] >> 8));
    }
    lvgl_flush_cb_16bit(display, area, px_map);
}

// LVGL borrows user_data and render buffers; lv_display_delete() does not free them.
// Keep our allocations together and release them on every delete, including failed setup.
typedef struct {
    struct lvgl_disp_data driver;
    void *buf;
    void *mono_buf;
} circuitpython_lvgl_display_data_t;

static void zephyr_display_lvgl_delete_cb(lv_event_t *event) {
    circuitpython_lvgl_display_data_t *data = lv_event_get_user_data(event);
    if (data->mono_buf != NULL) {
        lvgl_set_mono_conversion_buffer(NULL, 0);
    }
    lv_free(data->mono_buf);
    lv_free(data->buf);
    lv_free(data);
}

// Recreate the default LVGL display for the (possibly newly selected) driver pixel format,
// mirroring the glue's lvgl_init() display setup. The caller holds the LVGL lock.
static int zephyr_display_lvgl_recreate_display(zephyr_display_display_obj_t *self) {
    lv_display_t *old = lv_display_get_default();
    if (old != NULL) {
        lv_display_delete(old);
    }

    lv_display_t *display = lv_display_create(self->capabilities.x_resolution,
        self->capabilities.y_resolution);
    if (display == NULL) {
        return -ENOMEM;
    }

    circuitpython_lvgl_display_data_t *data = lv_malloc_zeroed(sizeof(*data));
    if (data == NULL) {
        lv_display_delete(display);
        return -ENOMEM;
    }
    lv_display_add_event_cb(display, zephyr_display_lvgl_delete_cb, LV_EVENT_DELETE, data);
    data->driver.display_dev = self->device;
    display_get_capabilities(self->device, &data->driver.cap);
    lv_display_set_user_data(display, &data->driver);

    int result = set_lvgl_rendering_cb(display);
    if (result != 0) {
        lv_display_delete(display);
        return result;
    }

    // Override the glue's flush choice where the legacy-compatible transfer differs.
    if (self->pixel_format == PIXEL_FORMAT_RGB_565X) {
        lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
        lv_display_set_flush_cb(display, zephyr_display_lvgl_flush_565x);
    } else if (self->pixel_format == PIXEL_FORMAT_ABGR_8888 ||
               self->pixel_format == PIXEL_FORMAT_BGRA_8888) {
        // Legacy treated ABGR/BGRA as 32-bit passthrough without alpha as well.
        lv_display_set_flush_cb(display, zephyr_display_lvgl_flush_argb_alpha0);
    } else if (self->pixel_format == PIXEL_FORMAT_ARGB_8888) {
        lv_display_set_flush_cb(display, zephyr_display_lvgl_flush_argb_alpha0);
    }

    // Partial-render-mode buffer, sized like the glue does.
    uint16_t hor_res = lv_display_get_horizontal_resolution(display);
    uint16_t ver_res = lv_display_get_vertical_resolution(display);
    uint32_t buf_nbr_pixels = (CONFIG_LV_Z_VDB_SIZE * hor_res * ver_res) / 100;
    if (buf_nbr_pixels < hor_res) {
        buf_nbr_pixels = hor_res;
    }
    uint32_t buf_size;
    bool mono = self->pixel_format == PIXEL_FORMAT_MONO01 || self->pixel_format == PIXEL_FORMAT_MONO10;
    bool al88 = self->pixel_format == PIXEL_FORMAT_AL_88;
    switch (self->pixel_format) {
        case PIXEL_FORMAT_ARGB_8888:
        case PIXEL_FORMAT_ABGR_8888:
            buf_size = 4 * buf_nbr_pixels;
            break;
        case PIXEL_FORMAT_RGB_888:
        case PIXEL_FORMAT_BGR_888:
            buf_size = 3 * buf_nbr_pixels;
            break;
        case PIXEL_FORMAT_RGB_565:
        case PIXEL_FORMAT_RGB_565X:
            buf_size = 2 * buf_nbr_pixels;
            break;
        case PIXEL_FORMAT_L_8:
            buf_size = buf_nbr_pixels;
            break;
        case PIXEL_FORMAT_MONO01:
        case PIXEL_FORMAT_MONO10:
            buf_size = buf_nbr_pixels / 8 + 8;
            buf_size += (buf_nbr_pixels % 8) == 0 ? 0 : 1;
            break;
        default:
            buf_size = 2 * buf_nbr_pixels;
            break;
    }
    if (al88) {
        buf_size = 2 * buf_nbr_pixels;
    }

    data->buf = lv_malloc(buf_size);
    if (data->buf == NULL) {
        lv_display_delete(display);
        return -ENOMEM;
    }
    lv_display_set_buffers(display, data->buf, NULL, buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);

    if (mono) {
        // The glue's mono flush converts the row-packed LVGL 1bpp buffer into the
        // display's (possibly vertically tiled) layout through this buffer.
        data->mono_buf = lv_malloc(buf_size);
        if (data->mono_buf == NULL) {
            lv_display_delete(display);
            return -ENOMEM;
        }
        lvgl_set_mono_conversion_buffer(data->mono_buf, buf_size);
    }
    self->lv_display = display;
    return 0;
}



static enum display_pixel_format zephyr_display_select_pixel_format(const struct display_capabilities *caps) {
    uint32_t formats = caps->supported_pixel_formats;

    if (formats & PIXEL_FORMAT_RGB_565) {
        return PIXEL_FORMAT_RGB_565;
    }
    if (formats & PIXEL_FORMAT_RGB_888) {
        return PIXEL_FORMAT_RGB_888;
    }
    if (formats & PIXEL_FORMAT_ARGB_8888) {
        return PIXEL_FORMAT_ARGB_8888;
    }
    if (formats & PIXEL_FORMAT_RGB_565X) {
        return PIXEL_FORMAT_RGB_565X;
    }
    if (formats & PIXEL_FORMAT_L_8) {
        return PIXEL_FORMAT_L_8;
    }
    if (formats & PIXEL_FORMAT_AL_88) {
        return PIXEL_FORMAT_AL_88;
    }
    if (formats & PIXEL_FORMAT_MONO01) {
        return PIXEL_FORMAT_MONO01;
    }
    if (formats & PIXEL_FORMAT_MONO10) {
        return PIXEL_FORMAT_MONO10;
    }
    return caps->current_pixel_format;
}

static void zephyr_display_select_colorspace(zephyr_display_display_obj_t *self,
    uint16_t *color_depth,
    uint8_t *bytes_per_cell,
    bool *grayscale,
    bool *pixels_in_byte_share_row,
    bool *reverse_pixels_in_byte,
    bool *reverse_bytes_in_word) {
    *color_depth = 16;
    *bytes_per_cell = 2;
    *grayscale = false;
    *pixels_in_byte_share_row = false;
    *reverse_pixels_in_byte = false;
    *reverse_bytes_in_word = false;

    if (self->pixel_format == PIXEL_FORMAT_RGB_565X) {
        // RGB_565X is big-endian RGB_565, so byte-swap from native LE.
        *reverse_bytes_in_word = true;
    } else if (self->pixel_format == PIXEL_FORMAT_RGB_888) {
        *color_depth = 24;
        *bytes_per_cell = 3;
        *reverse_bytes_in_word = false;
    } else if (self->pixel_format == PIXEL_FORMAT_ARGB_8888) {
        *color_depth = 32;
        *bytes_per_cell = 4;
        *reverse_bytes_in_word = false;
    } else if (self->pixel_format == PIXEL_FORMAT_L_8 ||
               self->pixel_format == PIXEL_FORMAT_AL_88) {
        *color_depth = 8;
        *bytes_per_cell = 1;
        *grayscale = true;
        *reverse_bytes_in_word = false;
    } else if (self->pixel_format == PIXEL_FORMAT_MONO01 ||
               self->pixel_format == PIXEL_FORMAT_MONO10) {
        bool vtiled = self->capabilities.screen_info & SCREEN_INFO_MONO_VTILED;
        bool msb_first = self->capabilities.screen_info & SCREEN_INFO_MONO_MSB_FIRST;
        *color_depth = 1;
        *bytes_per_cell = 1;
        *grayscale = true;
        *pixels_in_byte_share_row = !vtiled;
        *reverse_pixels_in_byte = msb_first;
        *reverse_bytes_in_word = false;
    }
}

void common_hal_zephyr_display_display_construct_from_device(zephyr_display_display_obj_t *self,
    const struct device *device,
    uint16_t rotation,
    bool auto_refresh) {
    self->auto_refresh = false;
    self->lv_display = NULL;

    if (device == NULL || !device_is_ready(device)) {
        raise_zephyr_error(-ENODEV);
    }

    self->device = device;
    display_get_capabilities(self->device, &self->capabilities);

    // Lock for the whole LVGL setup: the workqueue may already be running timers.
    nlr_jump_callback_node_t cleanup;
    displayio_lvgl_lock_with_cleanup(&cleanup);
    // Pick the pixel format exactly like the legacy compositor does (RGB565 preferred).
    self->pixel_format = zephyr_display_select_pixel_format(&self->capabilities);
    if (self->pixel_format != self->capabilities.current_pixel_format) {
        (void)display_set_pixel_format(self->device, self->pixel_format);
        display_get_capabilities(self->device, &self->capabilities);
        self->pixel_format = self->capabilities.current_pixel_format;
    }
    // The LVGL display glue configured itself from the driver's original current format at
    // boot. Recreate the LVGL display so its buffers and flush callbacks match the format
    // actually selected above.
    int result = zephyr_display_lvgl_recreate_display(self);
    if (result != 0) {
        self->device = NULL;
        self->base.type = &mp_type_NoneType;
        raise_zephyr_error(result);
    }

    uint16_t color_depth;
    uint8_t bytes_per_cell;
    bool grayscale;
    bool pixels_in_byte_share_row;
    bool reverse_pixels_in_byte;
    bool reverse_bytes_in_word;
    zephyr_display_select_colorspace(self, &color_depth, &bytes_per_cell,
        &grayscale, &pixels_in_byte_share_row, &reverse_pixels_in_byte,
        &reverse_bytes_in_word);

    displayio_display_core_construct(
        &self->core,
        self->capabilities.x_resolution,
        self->capabilities.y_resolution,
        0,
        color_depth,
        grayscale,
        pixels_in_byte_share_row,
        bytes_per_cell,
        reverse_pixels_in_byte,
        reverse_bytes_in_word);

    self->native_frames_per_second = 60;
    self->native_ms_per_frame = 1000 / self->native_frames_per_second;
    self->first_manual_refresh = !auto_refresh;

    if (rotation != 0) {
        common_hal_zephyr_display_display_set_rotation(self, rotation);
    }

    (void)display_blanking_off(self->device);

    displayio_lvgl_init();
    displayio_lvgl_register_display(lv_display_get_default());

    displayio_display_core_set_root_group(&self->core, &circuitpython_splash);
    displayio_lvgl_set_root_group(&circuitpython_splash);
    displayio_lvgl_unlock_with_cleanup();
    #if defined(CONFIG_LV_Z_RUN_LVGL_ON_WORKQUEUE)
    // Buffers and flush callbacks now match the panel; let the workqueue render.
    lvgl_timer_handler_resume();
    #endif
    common_hal_zephyr_display_display_set_auto_refresh(self, auto_refresh);
}

uint16_t common_hal_zephyr_display_display_get_width(zephyr_display_display_obj_t *self) {
    return displayio_display_core_get_width(&self->core);
}

uint16_t common_hal_zephyr_display_display_get_height(zephyr_display_display_obj_t *self) {
    return displayio_display_core_get_height(&self->core);
}

mp_float_t common_hal_zephyr_display_display_get_brightness(zephyr_display_display_obj_t *self) {
    (void)self;
    return -1;
}

bool common_hal_zephyr_display_display_set_brightness(zephyr_display_display_obj_t *self, mp_float_t brightness) {
    (void)self;
    (void)brightness;
    return false;
}


void common_hal_zephyr_display_display_set_rotation(zephyr_display_display_obj_t *self, int rotation) {
    bool transposed = (self->core.rotation == 90 || self->core.rotation == 270);
    bool will_transposed = (rotation == 90 || rotation == 270);
    if (transposed != will_transposed) {
        int tmp = self->core.width;
        self->core.width = self->core.height;
        self->core.height = tmp;
    }

    displayio_display_core_set_rotation(&self->core, rotation);

    if (self == &displays[0].zephyr_display) {
        supervisor_stop_terminal();
        supervisor_start_terminal(self->core.width, self->core.height);
    }

    if (self->core.current_group != NULL) {
        displayio_lvgl_set_root_group(self->core.current_group);
    }
}

uint16_t common_hal_zephyr_display_display_get_rotation(zephyr_display_display_obj_t *self) {
    return self->core.rotation;
}

bool common_hal_zephyr_display_display_refresh(zephyr_display_display_obj_t *self,
    uint32_t target_ms_per_frame,
    uint32_t maximum_ms_per_real_frame) {
    if (!self->auto_refresh && !self->first_manual_refresh && (target_ms_per_frame != NO_FPS_LIMIT)) {
        uint64_t current_time = supervisor_ticks_ms64();
        uint32_t current_ms_since_real_refresh = current_time - self->core.last_refresh;
        if (current_ms_since_real_refresh > maximum_ms_per_real_frame) {
            mp_raise_RuntimeError(MP_ERROR_TEXT("Below minimum frame rate"));
        }
        uint32_t current_ms_since_last_call = current_time - self->last_refresh_call;
        self->last_refresh_call = current_time;
        if (current_ms_since_last_call > target_ms_per_frame) {
            return false;
        }
        uint32_t remaining_time = target_ms_per_frame - (current_ms_since_real_refresh % target_ms_per_frame);
        while (supervisor_ticks_ms64() - self->last_refresh_call < remaining_time) {
            RUN_BACKGROUND_TASKS;
        }
    }
    self->first_manual_refresh = false;
    self->core.last_refresh = supervisor_ticks_ms64();
    // Manual refresh must render immediately, not on LVGL's timer cadence. The
    // workqueue's own handler already holds the lock, so no re-entrancy here.
    lvgl_lock();
    lv_refr_now(lv_display_get_default());
    lvgl_unlock();
    return true;
}

bool common_hal_zephyr_display_display_get_auto_refresh(zephyr_display_display_obj_t *self) {
    return self->auto_refresh;
}

void common_hal_zephyr_display_display_set_auto_refresh(zephyr_display_display_obj_t *self, bool auto_refresh) {
    self->first_manual_refresh = !auto_refresh;
    if (auto_refresh != self->auto_refresh) {
        if (auto_refresh) {
            supervisor_enable_tick();
        } else {
            supervisor_disable_tick();
        }
    }
    self->auto_refresh = auto_refresh;
}

void zephyr_display_display_background(zephyr_display_display_obj_t *self) {
    // LVGL renders on its own RTOS thread (the LVGL workqueue), so the CircuitPython
    // background task has nothing to pump.
    (void)self;
}

void release_zephyr_display(zephyr_display_display_obj_t *self) {
    common_hal_zephyr_display_display_set_auto_refresh(self, false);
    nlr_jump_callback_node_t cleanup;
    displayio_lvgl_lock_with_cleanup(&cleanup);
    displayio_lvgl_set_root_group(NULL);
    if (self->lv_display != NULL) {
        lv_display_delete(self->lv_display);
        self->lv_display = NULL;
    }
    release_display_core(&self->core);
    self->device = NULL;
    self->base.type = &mp_type_NoneType;
    displayio_lvgl_unlock_with_cleanup();
}

void zephyr_display_display_collect_ptrs(zephyr_display_display_obj_t *self) {
    (void)self;
    displayio_display_core_collect_ptrs(&self->core);
}

void zephyr_display_display_reset(zephyr_display_display_obj_t *self) {
    if (self->device != NULL && device_is_ready(self->device)) {
        common_hal_zephyr_display_display_set_auto_refresh(self, true);
        displayio_display_core_set_root_group(&self->core, &circuitpython_splash);
        displayio_lvgl_set_root_group(&circuitpython_splash);
        displayio_lvgl_lock();
        lv_refr_now(lv_display_get_default());
        displayio_lvgl_unlock();
    } else {
        release_zephyr_display(self);
    }
}

mp_obj_t common_hal_zephyr_display_display_get_root_group(zephyr_display_display_obj_t *self) {
    if (self->core.current_group == NULL) {
        return mp_const_none;
    }
    return self->core.current_group;
}

bool common_hal_zephyr_display_display_set_root_group(zephyr_display_display_obj_t *self, displayio_group_t *root_group) {
    bool ok = displayio_display_core_set_root_group(&self->core, root_group);
    displayio_lvgl_set_root_group(ok ? root_group : NULL);
    return ok;
}
