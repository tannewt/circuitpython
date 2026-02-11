// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "bindings/hostnetwork/HostNetwork.h"

hostnetwork_hostnetwork_obj_t common_hal_hostnetwork_obj = {
    .base = { &hostnetwork_hostnetwork_type },
    .port_offset = 0,
};

void common_hal_hostnetwork_hostnetwork_construct(hostnetwork_hostnetwork_obj_t *self) {
    self->port_offset = 0;
}

uint16_t common_hal_hostnetwork_get_port_offset(hostnetwork_hostnetwork_obj_t *self) {
    return self->port_offset;
}

void common_hal_hostnetwork_set_port_offset(hostnetwork_hostnetwork_obj_t *self, uint16_t offset) {
    self->port_offset = offset;
}
