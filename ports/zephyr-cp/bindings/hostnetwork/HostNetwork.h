// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>

#include "py/obj.h"

typedef struct {
    mp_obj_base_t base;
    uint16_t port_offset;
} hostnetwork_hostnetwork_obj_t;

extern const mp_obj_type_t hostnetwork_hostnetwork_type;

void common_hal_hostnetwork_hostnetwork_construct(hostnetwork_hostnetwork_obj_t *self);
uint16_t common_hal_hostnetwork_get_port_offset(hostnetwork_hostnetwork_obj_t *self);
void common_hal_hostnetwork_set_port_offset(hostnetwork_hostnetwork_obj_t *self, uint16_t offset);
