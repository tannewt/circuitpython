// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2025 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "shared-bindings/ethernet/__init__.h"
#include "shared-bindings/ethernet/Ethernet.h"

#include "bindings/zephyr_kernel/__init__.h"

#include "supervisor/port.h"
#include "supervisor/workflow.h"

#if CIRCUITPY_STATUS_BAR
#include "supervisor/shared/status_bar.h"
#endif

#include <zephyr/kernel.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/ethernet_mgmt.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/hostname.h>

#include <string.h>

#define MAC_ADDRESS_LENGTH 6

ethernet_ethernet_obj_t common_hal_ethernet_ethernet_obj;

static struct net_mgmt_event_callback eth_carrier_cb;
static struct net_mgmt_event_callback eth_ipv4_cb;

static void _eth_event_handler(struct net_mgmt_event_callback *cb, uint64_t mgmt_event, struct net_if *iface) {
    (void)iface;

    switch (mgmt_event) {
        case NET_EVENT_ETHERNET_CARRIER_ON:
            #if CIRCUITPY_STATUS_BAR
            supervisor_status_bar_request_update(false);
            #endif
            port_wake_main_task();
            break;
        case NET_EVENT_ETHERNET_CARRIER_OFF:
            #if CIRCUITPY_STATUS_BAR
            supervisor_status_bar_request_update(false);
            #endif
            port_wake_main_task();
            break;
        case NET_EVENT_IPV4_ADDR_ADD:
            #if CIRCUITPY_STATUS_BAR
            supervisor_status_bar_request_update(false);
            #endif
            port_wake_main_task();
            break;
    }
}

void common_hal_ethernet_ethernet_construct(ethernet_ethernet_obj_t *self) {
    self->base.type = &ethernet_ethernet_type;

    // Find the first ethernet interface
    self->netif = net_if_get_first_by_type(&NET_L2_GET_NAME(ETHERNET));
    if (self->netif == NULL) {
        return;
    }

    net_mgmt_init_event_callback(&eth_carrier_cb, _eth_event_handler,
        NET_EVENT_ETHERNET_CARRIER_ON |
        NET_EVENT_ETHERNET_CARRIER_OFF);
    net_mgmt_init_event_callback(&eth_ipv4_cb, _eth_event_handler,
        NET_EVENT_IPV4_ADDR_ADD);

    net_mgmt_add_event_callback(&eth_carrier_cb);
    net_mgmt_add_event_callback(&eth_ipv4_cb);

    #if defined(CONFIG_NET_HOSTNAME)
    // Set hostname based on board name and MAC address.
    size_t board_len = MIN(NET_HOSTNAME_MAX_LEN - ((MAC_ADDRESS_LENGTH * 2) + 6), strlen(CIRCUITPY_BOARD_ID));
    size_t board_trim = strlen(CIRCUITPY_BOARD_ID) - board_len;
    if (CIRCUITPY_BOARD_ID[board_trim] == '_') {
        board_trim++;
    }

    char cpy_default_hostname[board_len + (MAC_ADDRESS_LENGTH * 2) + 6];
    struct net_linkaddr *mac = net_if_get_link_addr(self->netif);
    if (mac->len >= MAC_ADDRESS_LENGTH) {
        snprintf(cpy_default_hostname, sizeof(cpy_default_hostname), "cpy-%s-%02x%02x%02x%02x%02x%02x",
            CIRCUITPY_BOARD_ID + board_trim,
            mac->addr[0], mac->addr[1], mac->addr[2],
            mac->addr[3], mac->addr[4], mac->addr[5]);
        net_hostname_set(cpy_default_hostname, strlen(cpy_default_hostname));
    }
    #endif

    // Enabled by default, start DHCP
    CHECK_ZEPHYR_RESULT(net_if_up(self->netif));
    self->started = true;
    net_dhcpv4_start(self->netif);
}

void ethernet_user_reset(void) {
    // Nothing to do on reset for now.
}
