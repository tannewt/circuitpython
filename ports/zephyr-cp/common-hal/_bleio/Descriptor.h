// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2019 Dan Halbert for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2026 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

#include <zephyr/bluetooth/uuid.h>

#include "py/obj.h"
#include "shared-bindings/_bleio/Attribute.h"
#include "common-hal/_bleio/UUID.h"

// Forward declaration to break circular dependency
struct _bleio_characteristic_obj;

typedef struct _bleio_descriptor_obj {
    mp_obj_base_t base;
    bleio_uuid_obj_t *uuid;
    uint16_t handle;
    bleio_attribute_security_mode_t read_perm;
    bleio_attribute_security_mode_t write_perm;
    uint16_t max_length;
    bool fixed_length;
    uint8_t *value;
    uint16_t value_length;
    struct _bleio_characteristic_obj *characteristic;
    struct bt_uuid_128 zephyr_uuid;
} bleio_descriptor_obj_t;
