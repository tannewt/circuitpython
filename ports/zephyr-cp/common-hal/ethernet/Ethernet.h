// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2025 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"

#include <zephyr/net/net_if.h>

typedef struct {
    mp_obj_base_t base;
    struct net_if *netif;
    bool started;
} ethernet_ethernet_obj_t;
