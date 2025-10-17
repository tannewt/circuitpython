// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2025 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"
#include <esp_lcd_mipi_dsi.h>

typedef struct {
    mp_obj_base_t base;
    mp_uint_t frequency;
    esp_lcd_dsi_bus_handle_t bus_handle;
    uint8_t num_data_lanes;
} mipidsi_bus_obj_t;
