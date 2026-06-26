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

#include "shared/runtime/interrupt_char.h"

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
    int up_res = net_if_up(self->netif);
    // -EALREADY means the interface is already up. That's not an error.
    if (up_res < 0 && up_res != -EALREADY) {
        raise_zephyr_error(up_res);
    }
    self->started = true;
    net_dhcpv4_start(self->netif);
}

// Raw uint32_t sibling of common_hal_ethernet_ethernet_get_ipv4_address(),
// used internally by supervisor/shared/web_workflow/web_workflow.c.
uint32_t ethernet_get_ipv4_address(void) {
    ethernet_ethernet_obj_t *self = &common_hal_ethernet_ethernet_obj;
    if (self->netif == NULL || !net_if_is_up(self->netif)) {
        return 0;
    }
    struct in_addr *addr = net_if_ipv4_get_global_addr(self->netif, NET_ADDR_PREFERRED);
    if (addr == NULL) {
        return 0;
    }
    return addr->s_addr;
}

// Raw hostname sibling of common_hal_ethernet_ethernet_get_hostname(),
// used internally by supervisor/shared/web_workflow/web_workflow.c.
const char *ethernet_get_hostname_raw(void) {
    ethernet_ethernet_obj_t *self = &common_hal_ethernet_ethernet_obj;
    if (self->netif == NULL) {
        return "";
    }
    return net_hostname_get();
}

// Blocks until the DHCP lease is bound. DHCPv4 parses its options (which adds
// the DNS server to the resolver) and only then binds the address, so a
// preferred global address implies getaddrinfo() has everything it needs.
// Starting a query before that fails with DNS_EAI_SYSTEM ("getaddrinfo
// failure: -11") because the resolver still has no server to send to.
//
// Returns immediately when the interface cannot ever get a lease: it is down,
// disabled or has no carrier. Used by the supervisor so the web workflow's
// listening socket can be created during this boot instead of being deferred
// until the next VM start.
void ethernet_wait_for_lease(void) {
    ethernet_ethernet_obj_t *self = &common_hal_ethernet_ethernet_obj;
    if (self->netif == NULL || !self->started || !net_if_is_carrier_ok(self->netif)) {
        return;
    }
    int64_t deadline = k_uptime_get() + 20000;
    while (k_uptime_get() < deadline && ethernet_get_ipv4_address() == 0) {
        if (mp_hal_is_interrupted()) {
            break;
        }
        RUN_BACKGROUND_TASKS;
        k_msleep(50);
    }
}

void ethernet_user_reset(void) {
    // Nothing to do on reset for now.
}
