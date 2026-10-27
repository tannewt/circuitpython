// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "py/obj.h"

// LVGL-based version of displayio Bitmap. Same pixel storage as the legacy version (rows
// packed into uint32_t words, sub-byte values MSB-first) so that LVGL image descriptors can
// share the data without copying. Dirty tracking is LVGL's own instead of the legacy dirty
// area: the compositor invalidates whole tile images when anything changes.

typedef struct {
    mp_obj_base_t base;
    uint16_t width;
    uint16_t height;
    uint32_t *data;
    uint16_t stride; // uint32_t's
    uint8_t bits_per_value;
    uint8_t x_shift;
    size_t x_mask;
    uint16_t bitmask;
    bool read_only;
    bool data_alloc; // did bitmap allocate data or someone else
} displayio_bitmap_t;

void displayio_bitmap_write_pixel(displayio_bitmap_t *self, int16_t x, int16_t y, uint32_t value);
// Higher-level dirty notification. The rectangular region is in bitmap pixel coordinates;
// the LVGL compositor currently invalidates coarsely but the legacy backend unions it into
// its dirty area.
void displayio_bitmap_set_dirty(displayio_bitmap_t *self, int16_t x1, int16_t y1, int16_t x2, int16_t y2);
