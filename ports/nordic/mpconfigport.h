// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2015 Glenn Ruben Bakke
// SPDX-FileCopyrightText: Copyright (c) 2019 Dan Halbert for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

#define MICROPY_PY_FUNCTION_ATTRS                (1)
#define MICROPY_PY_REVERSE_SPECIAL_METHODS       (1)
#define MICROPY_PY_SYS_STDIO_BUFFER              (1)

// 24kiB stack
#define CIRCUITPY_DEFAULT_STACK_SIZE            (24 * 1024)

#ifdef NRF52840
#define MICROPY_PY_SYS_PLATFORM "nRF52840"
#endif

#ifdef NRF52833
#define MICROPY_PY_SYS_PLATFORM "nRF52833"
#endif

////////////////////////////////////////////////////////////////////////////////////////////////////

// This also includes mpconfigboard.h.
#include "py/circuitpy_mpconfig.h"

// Definitions that might be overridden by mpconfigboard.h

#ifndef CIRCUITPY_INTERNAL_NVM_SIZE
#define CIRCUITPY_INTERNAL_NVM_SIZE (8 * 1024)
#endif

#ifndef BOARD_HAS_32KHZ_XTAL
// Assume crystal is present, which is the most common case.
#define BOARD_HAS_32KHZ_XTAL (1)
#endif
