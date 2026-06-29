/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2018 Scott Shawcroft for Adafruit Industries
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef MICROPY_INCLUDED_SHARED_BINDINGS_GBIO___INIT___H
#define MICROPY_INCLUDED_SHARED_BINDINGS_GBIO___INIT___H

#include "py/obj.h"

void common_hal_gbio_queue_commands(const uint8_t *buf, uint32_t len);
void common_hal_gbio_queue_vblank_commands(const uint8_t *buf, uint32_t len, uint32_t additional_cycles);

void common_hal_gbio_set_lcdc(uint8_t value);
uint8_t common_hal_gbio_get_lcdc(void);

uint8_t common_hal_gbio_get_pressed(void);
void common_hal_gbio_wait_for_vblank(void);
uint32_t common_hal_gbio_get_vsync_count(void);
void common_hal_gbio_reset_gameboy(void);
bool common_hal_gbio_is_color(void);

#endif  // MICROPY_INCLUDED_SHARED_BINDINGS_GBIO___INIT___H
