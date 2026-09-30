// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2022 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"

typedef struct {
    mp_obj_base_t base;
    uint8_t device_address;
    uint8_t configuration_index; // not bConfigurationValue
    uint8_t *configuration_descriptor; // Contains the length of the all descriptors.
    uint8_t open_endpoints[8];
    uint16_t first_langid;
    // Continuous bulk IN capture losses, kept after stop until the next start.
    uint32_t bulk_in_lost_packets;
} usb_core_device_obj_t;
