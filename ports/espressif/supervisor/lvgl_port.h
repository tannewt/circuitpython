// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

#include "py/mpconfig.h"

#if CIRCUITPY_DISPLAYIO_LVGL

// Initialize LVGL itself (lv_init and the tick timer). Safe to call repeatedly.
void espressif_lvgl_init(void);

// Run LVGL's timer handler. Called from the port's background tick.
void espressif_lvgl_background(void);

#endif
