/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2016 Radomir Dopieralski for Adafruit Industries
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

#ifndef MICROPY_INCLUDED_GAMEPAD_GAMEPAD_H
#define MICROPY_INCLUDED_GAMEPAD_GAMEPAD_H

#include <stdint.h>

#include "shared-bindings/busio/SPI.h"
#include "shared-bindings/digitalio/DigitalInOut.h"

typedef struct {
    mp_obj_base_t base;
    digitalio_digitalinout_obj_t* pins[8];
    digitalio_digitalinout_obj_t* cs_pin;
    busio_spi_obj_t *spi_bus;
    volatile uint8_t last;
    volatile uint8_t pressed;
    uint8_t pulls;
} gamepad_obj_t;

void gamepad_init_pins(size_t n_pins, const mp_obj_t* pins);
void gamepad_init_bus(digitalio_digitalinout_obj_t* cs_pin, busio_spi_obj_t* spi_bus);

#endif  // MICROPY_INCLUDED_GAMEPAD_GAMEPAD_H
