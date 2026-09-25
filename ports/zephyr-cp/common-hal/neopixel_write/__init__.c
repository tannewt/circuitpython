// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Dan Halbert for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// SoC-family-independent part of neopixel_write: wait for any previous
// write() to finish. The neopixel datastream itself is produced by a per-family
// file in this directory (nrf.c for Nordic), selected here.

#include "shared-bindings/neopixel_write/__init__.h"
#include "common-hal/neopixel_write/__init__.h"
#include "supervisor/port.h"

#if !defined(CONFIG_SOC_FAMILY_NORDIC_NRF)
#error "neopixel_write has no implementation for this SoC family"
#endif

// Earliest raw tick at which the next write may start. The strip latches the
// previous data while its input stays low for the reset period (a few tens
// of microseconds); a raw tick is about a millisecond, so 4 ticks is ample.
static uint64_t next_start_raw_ticks = 0;

void common_hal_neopixel_write(const digitalio_digitalinout_obj_t *digitalinout,
    uint8_t *pixels, uint32_t num_bytes) {
    while (port_get_raw_ticks(NULL) < next_start_raw_ticks) {
    }

    neopixel_write_send(digitalinout, pixels, num_bytes);

    next_start_raw_ticks = port_get_raw_ticks(NULL) + 4;
}
