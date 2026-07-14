// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2021 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "supervisor/board.h"

#if CIRCUITPY_GBIO
#include "shared-bindings/_gbio/__init__.h"
#endif

void board_init(void) {
    #if CIRCUITPY_GBIO
    common_hal_gbio_reset_gameboy();
    #endif
}

// Use the MP_WEAK supervisor/shared/board.c versions of routines not defined here.
