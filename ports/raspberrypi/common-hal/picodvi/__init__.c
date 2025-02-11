// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2025 Jeff Epler for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "common-hal/picodvi/__init__.h"
#include "common-hal/picodvi/Framebuffer.h"
#include "bindings/picodvi/Framebuffer.h"
#include "shared-bindings/busio/I2C.h"
#include "shared-bindings/board/__init__.h"
#include "shared-module/displayio/__init__.h"
#include "shared-module/os/__init__.h"
#include "supervisor/shared/safe_mode.h"
#include "py/gc.h"
#include "py/runtime.h"
#include "supervisor/port_heap.h"

void picodvi_autoconstruct(void) {
    if (get_safe_mode() != SAFE_MODE_NONE) {
        return;
    }
    #if defined(DEFAULT_DVI_BUS_CLK_DP)
    // check if address 0x50 is live on the I2C bus -- return if not
    busio_i2c_obj_t *i2c = common_hal_board_create_i2c(0);
    if (!i2c) {
        return;
    }
    if (!common_hal_busio_i2c_try_lock(i2c)) {
        return;
    }
    bool probed = common_hal_busio_i2c_probe(i2c, 0x50);
    common_hal_busio_i2c_unlock(i2c);
    if (!probed) {
        return;
    }

    mp_int_t width = 320;
    mp_int_t height = 240;
    mp_int_t color_depth = 16;

    // TODO: User configuration can cause safe mode errors without a self-explanatory message.
    common_hal_os_getenv_int("CIRCUITPY_DISPLAY_WIDTH", &width);
    common_hal_os_getenv_int("CIRCUITPY_DISPLAY_HEIGHT", &height);
    common_hal_os_getenv_int("CIRCUITPY_DISPLAY_COLOR_DEPTH", &color_depth);

    // construct framebuffer and display

    picodvi_framebuffer_obj_t *fb = &allocate_display_bus_or_raise()->picodvi;
    fb->base.type = &picodvi_framebuffer_type;
    common_hal_picodvi_framebuffer_construct(fb,
        width, height,
        DEFAULT_DVI_BUS_CLK_DP,
        DEFAULT_DVI_BUS_CLK_DN,
        DEFAULT_DVI_BUS_RED_DP,
        DEFAULT_DVI_BUS_RED_DN,
        DEFAULT_DVI_BUS_GREEN_DP,
        DEFAULT_DVI_BUS_GREEN_DN,
        DEFAULT_DVI_BUS_BLUE_DP,
        DEFAULT_DVI_BUS_BLUE_DN,
        color_depth);

    framebufferio_framebufferdisplay_obj_t *display = &allocate_display()->framebuffer_display;
    display->base.type = &framebufferio_framebufferdisplay_type;
    common_hal_framebufferio_framebufferdisplay_construct(
        display,
        MP_OBJ_FROM_PTR(fb),
        0,
        true);
    #endif
}
