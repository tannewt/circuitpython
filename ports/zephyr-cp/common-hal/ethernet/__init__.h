// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2025 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common-hal/ethernet/Ethernet.h"

extern ethernet_ethernet_obj_t common_hal_ethernet_ethernet_obj;

void common_hal_ethernet_ethernet_construct(ethernet_ethernet_obj_t *self);

// Raw uint32_t IPv4 address of the interface, in network byte order, or 0
// when the interface has none. Used by supervisor/shared/web_workflow.
uint32_t ethernet_get_ipv4_address(void);

// Raw hostname of the network interface as set via net_hostname_set (and sent
// as DHCP option 12). Never allocates. The returned pointer points into
// storage owned by the port and stays valid until set_hostname() replaces it,
// so use it promptly rather than holding onto it.
const char *ethernet_get_hostname_raw(void);

// Blocks until the interface has an IPv4 address (DHCP lease or static), or
// 20s pass, or a hard interrupt arrives. Returns immediately when the
// interface cannot ever get one (down, disabled or carrier off).
void ethernet_wait_for_lease(void);
