// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2025 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common-hal/ethernet/Ethernet.h"

extern ethernet_ethernet_obj_t common_hal_ethernet_ethernet_obj;

void common_hal_ethernet_ethernet_construct(ethernet_ethernet_obj_t *self);
