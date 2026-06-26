// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2025 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "shared-bindings/ethernet/Ethernet.h"

#include <string.h>

#include "bindings/zephyr_kernel/__init__.h"

#include "py/runtime.h"
#include "shared-bindings/ipaddress/IPv4Address.h"
#include "shared-module/ipaddress/__init__.h"

#include <zephyr/kernel.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/hostname.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/phy.h>

#define MAC_ADDRESS_LENGTH 6

bool common_hal_ethernet_ethernet_get_enabled(ethernet_ethernet_obj_t *self) {
    return self->started;
}

void common_hal_ethernet_ethernet_set_enabled(ethernet_ethernet_obj_t *self, bool enabled) {
    if (self->netif == NULL) {
        return;
    }
    if (self->started && !enabled) {
        net_dhcpv4_stop(self->netif);
        CHECK_ZEPHYR_RESULT(net_if_down(self->netif));
        self->started = false;
    } else if (!self->started && enabled) {
        CHECK_ZEPHYR_RESULT(net_if_up(self->netif));
        self->started = true;
        net_dhcpv4_start(self->netif);
    }
}

bool common_hal_ethernet_ethernet_get_connected(ethernet_ethernet_obj_t *self) {
    if (self->netif == NULL) {
        return false;
    }
    return net_if_is_carrier_ok(self->netif) && net_if_is_up(self->netif);
}

mp_obj_t common_hal_ethernet_ethernet_get_hostname(ethernet_ethernet_obj_t *self) {
    const char *hostname = net_hostname_get();
    return mp_obj_new_str(hostname, strlen(hostname));
}

void common_hal_ethernet_ethernet_set_hostname(ethernet_ethernet_obj_t *self, const char *hostname) {
    CHECK_ZEPHYR_RESULT(net_hostname_set((char *)hostname, strlen(hostname)));
}

mp_obj_t common_hal_ethernet_ethernet_get_mac_address(ethernet_ethernet_obj_t *self) {
    if (self->netif == NULL) {
        return mp_obj_new_bytes((const uint8_t *)"\0\0\0\0\0\0", MAC_ADDRESS_LENGTH);
    }
    struct net_linkaddr *mac = net_if_get_link_addr(self->netif);
    return mp_obj_new_bytes(mac->addr, MAC_ADDRESS_LENGTH);
}

mp_int_t common_hal_ethernet_ethernet_get_link_speed(ethernet_ethernet_obj_t *self) {
    if (self->netif == NULL || !net_if_is_carrier_ok(self->netif)) {
        return 0;
    }
    const struct device *phy = net_eth_get_phy(self->netif);
    if (phy == NULL) {
        return 0;
    }
    struct phy_link_state state;
    if (phy_get_link_state(phy, &state) != 0) {
        return 0;
    }
    switch (state.speed) {
        case LINK_HALF_10BASE:
        case LINK_FULL_10BASE:
            return 10;
        case LINK_HALF_100BASE:
        case LINK_FULL_100BASE:
            return 100;
        case LINK_HALF_1000BASE:
        case LINK_FULL_1000BASE:
            return 1000;
        default:
            return 0;
    }
}

bool common_hal_ethernet_ethernet_get_full_duplex(ethernet_ethernet_obj_t *self) {
    if (self->netif == NULL || !net_if_is_carrier_ok(self->netif)) {
        return false;
    }
    const struct device *phy = net_eth_get_phy(self->netif);
    if (phy == NULL) {
        return false;
    }
    struct phy_link_state state;
    if (phy_get_link_state(phy, &state) != 0) {
        return false;
    }
    switch (state.speed) {
        case LINK_FULL_10BASE:
        case LINK_FULL_100BASE:
        case LINK_FULL_1000BASE:
            return true;
        default:
            return false;
    }
}

void common_hal_ethernet_ethernet_start_dhcp_client(ethernet_ethernet_obj_t *self, bool ipv4, bool ipv6) {
    if (self->netif == NULL) {
        return;
    }
    if (ipv4) {
        net_dhcpv4_start(self->netif);
    } else {
        net_dhcpv4_stop(self->netif);
    }
    if (ipv6) {
        // TODO: DHCPv6 support
    }
}

void common_hal_ethernet_ethernet_stop_dhcp_client(ethernet_ethernet_obj_t *self) {
    if (self->netif == NULL) {
        return;
    }
    net_dhcpv4_stop(self->netif);
}

mp_obj_t common_hal_ethernet_ethernet_get_ipv4_gateway(ethernet_ethernet_obj_t *self) {
    if (self->netif == NULL || !net_if_is_up(self->netif)) {
        return mp_const_none;
    }
    struct net_if_config *config = net_if_get_config(self->netif);
    if (config == NULL || config->ip.ipv4 == NULL) {
        return mp_const_none;
    }
    return common_hal_ipaddress_new_ipv4address(config->ip.ipv4->gw.s_addr);
}

mp_obj_t common_hal_ethernet_ethernet_get_ipv4_subnet(ethernet_ethernet_obj_t *self) {
    if (self->netif == NULL || !net_if_is_up(self->netif)) {
        return mp_const_none;
    }
    struct net_if_config *config = net_if_get_config(self->netif);
    if (config == NULL || config->ip.ipv4 == NULL) {
        return mp_const_none;
    }
    // Return netmask of first active address
    for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
        if (config->ip.ipv4->unicast[i].ipv4.addr_state == NET_ADDR_PREFERRED) {
            return common_hal_ipaddress_new_ipv4address(config->ip.ipv4->unicast[i].netmask.s_addr);
        }
    }
    return mp_const_none;
}

mp_obj_t common_hal_ethernet_ethernet_get_ipv4_address(ethernet_ethernet_obj_t *self) {
    if (self->netif == NULL || !net_if_is_up(self->netif)) {
        return mp_const_none;
    }
    struct net_if_config *config = net_if_get_config(self->netif);
    if (config == NULL || config->ip.ipv4 == NULL) {
        return mp_const_none;
    }
    // Return the first preferred unicast address
    for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
        struct net_if_addr *addr = &config->ip.ipv4->unicast[i].ipv4;
        if (addr->addr_state == NET_ADDR_PREFERRED) {
            return common_hal_ipaddress_new_ipv4address(addr->address.in_addr.s_addr);
        }
    }
    return mp_const_none;
}

mp_obj_t common_hal_ethernet_ethernet_get_ipv4_dns(ethernet_ethernet_obj_t *self) {
    // TODO: Implement DNS query via Zephyr DNS resolver
    return mp_const_none;
}

void common_hal_ethernet_ethernet_set_ipv4_dns(ethernet_ethernet_obj_t *self, mp_obj_t ipv4_dns_addr) {
    // TODO: Implement DNS set via Zephyr DNS resolver
}

mp_obj_t common_hal_ethernet_ethernet_get_addresses(ethernet_ethernet_obj_t *self) {
    if (self->netif == NULL || !net_if_is_up(self->netif)) {
        return mp_const_empty_tuple;
    }
    struct net_if_config *config = net_if_get_config(self->netif);
    if (config == NULL || config->ip.ipv4 == NULL) {
        return mp_const_empty_tuple;
    }

    // Count valid addresses
    int count = 0;
    for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
        if (config->ip.ipv4->unicast[i].ipv4.addr_state == NET_ADDR_PREFERRED) {
            count++;
        }
    }
    if (count == 0) {
        return mp_const_empty_tuple;
    }

    mp_obj_tuple_t *result = MP_OBJ_TO_PTR(mp_obj_new_tuple(count, NULL));
    int idx = 0;
    for (int i = 0; i < NET_IF_MAX_IPV4_ADDR && idx < count; i++) {
        struct net_if_addr *addr = &config->ip.ipv4->unicast[i].ipv4;
        if (addr->addr_state == NET_ADDR_PREFERRED) {
            char buf[NET_IPV4_ADDR_LEN];
            net_addr_ntop(AF_INET, &addr->address.in_addr, buf, sizeof(buf));
            result->items[idx++] = mp_obj_new_str(buf, strlen(buf));
        }
    }
    return MP_OBJ_FROM_PTR(result);
}

mp_obj_t common_hal_ethernet_ethernet_get_dns(ethernet_ethernet_obj_t *self) {
    // TODO: Implement via Zephyr DNS resolver
    return mp_const_empty_tuple;
}

void common_hal_ethernet_ethernet_set_dns(ethernet_ethernet_obj_t *self, mp_obj_t dns_addr) {
    // TODO: Implement via Zephyr DNS resolver
}

void common_hal_ethernet_ethernet_set_ipv4_address(ethernet_ethernet_obj_t *self, mp_obj_t ipv4, mp_obj_t netmask, mp_obj_t gateway, mp_obj_t ipv4_dns) {
    if (self->netif == NULL) {
        return;
    }

    // Stop DHCP first
    common_hal_ethernet_ethernet_stop_dhcp_client(self);

    // Get packed IPv4 bytes from ipaddress objects
    mp_obj_t packed;
    size_t len;
    const char *bytes;

    // Set the address
    struct in_addr addr;
    packed = common_hal_ipaddress_ipv4address_get_packed(ipv4);
    bytes = mp_obj_str_get_data(packed, &len);
    memcpy(&addr, bytes, sizeof(addr));
    struct net_if_addr *if_addr = net_if_ipv4_addr_add(self->netif, &addr, NET_ADDR_MANUAL, 0);
    if (if_addr == NULL) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("Failed to set IP address"));
    }

    // Set the netmask
    struct in_addr mask;
    packed = common_hal_ipaddress_ipv4address_get_packed(netmask);
    bytes = mp_obj_str_get_data(packed, &len);
    memcpy(&mask, bytes, sizeof(mask));
    net_if_ipv4_set_netmask_by_addr(self->netif, &addr, &mask);

    // Set the gateway
    struct in_addr gw;
    packed = common_hal_ipaddress_ipv4address_get_packed(gateway);
    bytes = mp_obj_str_get_data(packed, &len);
    memcpy(&gw, bytes, sizeof(gw));
    net_if_ipv4_set_gw(self->netif, &gw);

    if (ipv4_dns != MP_OBJ_NULL) {
        common_hal_ethernet_ethernet_set_ipv4_dns(self, ipv4_dns);
    }
}

mp_int_t common_hal_ethernet_ethernet_ping(ethernet_ethernet_obj_t *self, mp_obj_t ip_address, mp_float_t timeout) {
    // TODO: Implement ping via Zephyr net_icmp
    return -1;
}
