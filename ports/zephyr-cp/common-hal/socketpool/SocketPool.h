// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2020 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "py/obj.h"

typedef struct {
    mp_obj_base_t base;
    bool is_hostnetwork;
    uint16_t port_offset;
} socketpool_socketpool_obj_t;
