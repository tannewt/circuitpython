// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2025 Dan Halbert for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

#include "py/enum.h"

typedef enum {
    POWER_MANAGEMENT_NONE = 0,
    POWER_MANAGEMENT_MIN = 1,
    POWER_MANAGEMENT_MAX = 2,
    // Value can't be determined.
    POWER_MANAGEMENT_UNKNOWN = 3,
} wifi_power_management_t;

extern const mp_obj_type_t wifi_power_management_type;
