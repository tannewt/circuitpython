// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Dan Halbert for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>

#include "common-hal/digitalio/DigitalInOut.h"

// Called from reset_port() to drop any buffer held across writes.
void neopixel_write_reset(void);

// Per-SoC-family neopixel write, called by common_hal_neopixel_write() after the
// latch (reset) period of the previous write has elapsed. The pin is already
// configured as an output driving low, and must be left that way. When the
// preferred transmit hardware is busy the family falls back to bit-banging.
// Any other failure is logged and the write is skipped, because the status
// LED calls this before the VM exists and cannot catch an error.
void neopixel_write_send(const digitalio_digitalinout_obj_t *gpio,
    const uint8_t *pixels, uint32_t num_bytes);
