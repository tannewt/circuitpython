// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2019 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

#include "shared-bindings/digitalio/DigitalInOut.h"
#include "shared-bindings/displayio/Group.h"
#if CIRCUITPY_PWMIO
#include "shared-bindings/pwmio/PWMOut.h"
#endif

#if CIRCUITPY_DISPLAYIO_LVGL
#include "lvgl.h"
// LVGL fork: no display_core/bus_core, own lv_display + bus handles.
typedef struct {
    mp_obj_base_t base;
    // Bus the display is attached to (fourwire, etc.) - stored as mp_obj_t for GC
    mp_obj_t bus;
    // LVGL display handle - owns draw buffers and flush CB
    lv_display_t *lv_display;
    lv_color_format_t lv_color_format;
    void *lv_buf1;
    void *lv_buf2;
    size_t lv_buf_size;
    displayio_group_t *root_group;
    // Display geometry / commands retained for flush
    uint16_t width;
    uint16_t height;
    uint16_t colstart;
    uint16_t rowstart;
    uint16_t rotation;
    uint16_t color_depth;
    bool grayscale;
    bool pixels_in_byte_share_row;
    uint8_t bytes_per_cell;
    bool reverse_pixels_in_byte;
    bool reverse_bytes_in_word;
    bool single_byte_bounds;
    bool data_as_commands;
    bool sh1107_addressing;
    uint8_t set_column_command;
    uint8_t set_row_command;
    uint8_t write_ram_command;
    union {
        digitalio_digitalinout_obj_t backlight_inout;
        #if CIRCUITPY_PWMIO
        pwmio_pwmout_obj_t backlight_pwm;
        #endif
    };
    uint64_t last_refresh_call;
    mp_float_t current_brightness;
    uint16_t brightness_command;
    uint16_t native_frames_per_second;
    uint16_t native_ms_per_frame;
    bool auto_refresh;
    bool first_manual_refresh;
    bool backlight_on_high;
    bool deinited;
} busdisplay_busdisplay_obj_t;
#else
#include "shared-module/displayio/legacy/area.h"
#include "shared-module/displayio/bus_core.h"
#include "shared-module/displayio/display_core.h"

typedef struct {
    mp_obj_base_t base;
    displayio_display_core_t core;
    displayio_display_bus_t bus;
    union {
        digitalio_digitalinout_obj_t backlight_inout;
        #if CIRCUITPY_PWMIO
        pwmio_pwmout_obj_t backlight_pwm;
        #endif
    };
    uint64_t last_refresh_call;
    mp_float_t current_brightness;
    uint16_t brightness_command;
    uint16_t native_frames_per_second;
    uint16_t native_ms_per_frame;
    uint8_t write_ram_command;
    bool auto_refresh;
    bool first_manual_refresh;
    bool backlight_on_high;
} busdisplay_busdisplay_obj_t;
#endif

void busdisplay_busdisplay_background(busdisplay_busdisplay_obj_t *self);
void release_busdisplay(busdisplay_busdisplay_obj_t *self);
void reset_busdisplay(busdisplay_busdisplay_obj_t *self);
void busdisplay_busdisplay_collect_ptrs(busdisplay_busdisplay_obj_t *self);
