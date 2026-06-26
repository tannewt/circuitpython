// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2025 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common-hal/ethernet/Ethernet.h"

extern ethernet_ethernet_obj_t common_hal_ethernet_ethernet_obj;

void common_hal_ethernet_ethernet_construct(ethernet_ethernet_obj_t *self);
void common_hal_ethernet_ethernet_start(ethernet_ethernet_obj_t *self);

// Raw hostname of the network interface as configured via esp_netif_set_hostname
// (DHCP option 12). Never allocates. The returned pointer points into
// storage owned by the port and stays valid until set_hostname() replaces it,
// so use it promptly rather than holding onto it. Used by
// supervisor/shared/web_workflow.
const char *ethernet_get_hostname_raw(void);
