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
    #if !CIRCUITPY_ALL_MEMORY_DMA_CAPABLE
    uint8_t *temp_buffer;  // Temporary buffer for PSRAM data
    size_t temp_buffer_size;  // Size of temporary buffer
    #endif
} usb_core_device_obj_t;
