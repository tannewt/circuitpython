// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2025 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"
#include "py/circuitpy_objawaitable.h"

void *common_hal_asyncexample_delay_start(circuitpy_async_flag_t *flag, mp_int_t ms);
mp_obj_t common_hal_asyncexample_delay_end(void *context);
void common_hal_asyncexample_delay_cancel(void *context);
