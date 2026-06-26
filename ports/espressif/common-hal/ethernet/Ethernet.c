// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2025 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "shared-bindings/ethernet/Ethernet.h"

#include <string.h>

#include "bindings/espidf/__init__.h"

#include "py/runtime.h"
#include "shared-bindings/ipaddress/IPv4Address.h"
#include "shared-module/ipaddress/__init__.h"

#include "esp_eth.h"
#include "esp_netif.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"

#define MAC_ADDRESS_LENGTH 6

static void _ipv4address_to_esp_idf(mp_obj_t ip_address, esp_ip4_addr_t *esp_addr) {
    mp_obj_t packed = common_hal_ipaddress_ipv4address_get_packed(ip_address);
    size_t len;
    const char *bytes = mp_obj_str_get_data(packed, &len);
    esp_netif_set_ip4_addr(esp_addr, bytes[0], bytes[1], bytes[2], bytes[3]);
}

static mp_obj_t _ip4_to_str(const esp_ip4_addr_t *addr) {
    char buf[IPADDR_STRLEN_MAX];
    inet_ntop(AF_INET, addr, buf, sizeof(buf));
    return mp_obj_new_str(buf, strlen(buf));
}

static mp_obj_t _ipaddr_to_str(const esp_ip_addr_t *addr) {
    char buf[IPADDR_STRLEN_MAX];
    inet_ntop(addr->type == ESP_IPADDR_TYPE_V6 ? AF_INET6 : AF_INET, addr, buf, sizeof(buf));
    return mp_obj_new_str(buf, strlen(buf));
}

bool common_hal_ethernet_ethernet_get_enabled(ethernet_ethernet_obj_t *self) {
    return self->started;
}

void common_hal_ethernet_ethernet_set_enabled(ethernet_ethernet_obj_t *self, bool enabled) {
    if (self->eth_handle == NULL) {
        return;
    }
    if (self->started && !enabled) {
        esp_netif_dhcpc_stop(self->netif);
        esp_eth_stop(self->eth_handle);
        self->started = false;
    } else if (!self->started && enabled) {
        CHECK_ESP_RESULT(esp_eth_start(self->eth_handle));
        self->started = true;
        esp_netif_dhcpc_start(self->netif);
    }
}

bool common_hal_ethernet_ethernet_get_connected(ethernet_ethernet_obj_t *self) {
    return self->started && self->connected;
}

mp_obj_t common_hal_ethernet_ethernet_get_hostname(ethernet_ethernet_obj_t *self) {
    if (self->netif == NULL) {
        return mp_obj_new_str("", 0);
    }
    const char *hostname = NULL;
    esp_netif_get_hostname(self->netif, &hostname);
    if (hostname == NULL) {
        return mp_obj_new_str("", 0);
    }
    return mp_obj_new_str(hostname, strlen(hostname));
}

void common_hal_ethernet_ethernet_set_hostname(ethernet_ethernet_obj_t *self, const char *hostname) {
    if (self->netif == NULL) {
        return;
    }
    CHECK_ESP_RESULT(esp_netif_set_hostname(self->netif, hostname));
}

mp_obj_t common_hal_ethernet_ethernet_get_mac_address(ethernet_ethernet_obj_t *self) {
    uint8_t mac[MAC_ADDRESS_LENGTH] = {0};
    if (self->eth_handle != NULL) {
        esp_eth_ioctl(self->eth_handle, ETH_CMD_G_MAC_ADDR, mac);
    }
    return mp_obj_new_bytes(mac, MAC_ADDRESS_LENGTH);
}

mp_int_t common_hal_ethernet_ethernet_get_link_speed(ethernet_ethernet_obj_t *self) {
    if (self->eth_handle == NULL || !self->connected) {
        return 0;
    }
    eth_speed_t speed;
    if (esp_eth_ioctl(self->eth_handle, ETH_CMD_G_SPEED, &speed) != ESP_OK) {
        return 0;
    }
    return speed == ETH_SPEED_100M ? 100 : 10;
}

bool common_hal_ethernet_ethernet_get_full_duplex(ethernet_ethernet_obj_t *self) {
    if (self->eth_handle == NULL || !self->connected) {
        return false;
    }
    eth_duplex_t duplex;
    if (esp_eth_ioctl(self->eth_handle, ETH_CMD_G_DUPLEX_MODE, &duplex) != ESP_OK) {
        return false;
    }
    return duplex == ETH_DUPLEX_FULL;
}

void common_hal_ethernet_ethernet_start_dhcp_client(ethernet_ethernet_obj_t *self, bool ipv4, bool ipv6) {
    if (self->netif == NULL) {
        return;
    }
    if (ipv4) {
        esp_netif_dhcpc_start(self->netif);
    } else {
        esp_netif_dhcpc_stop(self->netif);
    }
    if (ipv6) {
        // TODO: DHCPv6 support
    }
}

void common_hal_ethernet_ethernet_stop_dhcp_client(ethernet_ethernet_obj_t *self) {
    if (self->netif == NULL) {
        return;
    }
    esp_netif_dhcpc_stop(self->netif);
}

mp_obj_t common_hal_ethernet_ethernet_get_ipv4_gateway(ethernet_ethernet_obj_t *self) {
    if (self->netif == NULL || !esp_netif_is_netif_up(self->netif)) {
        return mp_const_none;
    }
    esp_netif_get_ip_info(self->netif, &self->ip_info);
    return common_hal_ipaddress_new_ipv4address(self->ip_info.gw.addr);
}

mp_obj_t common_hal_ethernet_ethernet_get_ipv4_subnet(ethernet_ethernet_obj_t *self) {
    if (self->netif == NULL || !esp_netif_is_netif_up(self->netif)) {
        return mp_const_none;
    }
    esp_netif_get_ip_info(self->netif, &self->ip_info);
    return common_hal_ipaddress_new_ipv4address(self->ip_info.netmask.addr);
}

mp_obj_t common_hal_ethernet_ethernet_get_ipv4_address(ethernet_ethernet_obj_t *self) {
    if (self->netif == NULL || !esp_netif_is_netif_up(self->netif)) {
        return mp_const_none;
    }
    esp_netif_get_ip_info(self->netif, &self->ip_info);
    return common_hal_ipaddress_new_ipv4address(self->ip_info.ip.addr);
}

mp_obj_t common_hal_ethernet_ethernet_get_ipv4_dns(ethernet_ethernet_obj_t *self) {
    if (self->netif == NULL || !esp_netif_is_netif_up(self->netif)) {
        return mp_const_none;
    }
    esp_netif_get_dns_info(self->netif, ESP_NETIF_DNS_MAIN, &self->dns_info);
    if (self->dns_info.ip.type != ESP_IPADDR_TYPE_V4) {
        return mp_const_none;
    }
    return common_hal_ipaddress_new_ipv4address(self->dns_info.ip.u_addr.ip4.addr);
}

void common_hal_ethernet_ethernet_set_ipv4_dns(ethernet_ethernet_obj_t *self, mp_obj_t ipv4_dns_addr) {
    if (self->netif == NULL) {
        return;
    }
    esp_netif_dns_info_t dns_addr;
    _ipv4address_to_esp_idf(ipv4_dns_addr, &dns_addr.ip.u_addr.ip4);
    esp_netif_set_dns_info(self->netif, ESP_NETIF_DNS_MAIN, &dns_addr);
}

mp_obj_t common_hal_ethernet_ethernet_get_addresses(ethernet_ethernet_obj_t *self) {
    if (self->netif == NULL || !esp_netif_is_netif_up(self->netif)) {
        return mp_const_empty_tuple;
    }
    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(self->netif, &ip_info);
    if (ip_info.ip.addr == IPADDR_ANY) {
        return mp_const_empty_tuple;
    }
    mp_obj_t addr = _ip4_to_str(&ip_info.ip);
    return mp_obj_new_tuple(1, &addr);
}

mp_obj_t common_hal_ethernet_ethernet_get_dns(ethernet_ethernet_obj_t *self) {
    if (self->netif == NULL || !esp_netif_is_netif_up(self->netif)) {
        return mp_const_empty_tuple;
    }
    esp_netif_dns_info_t dns_info;
    esp_netif_get_dns_info(self->netif, ESP_NETIF_DNS_MAIN, &dns_info);
    if (dns_info.ip.type == ESP_IPADDR_TYPE_V4 && dns_info.ip.u_addr.ip4.addr == IPADDR_ANY) {
        return mp_const_empty_tuple;
    }
    mp_obj_t addr = _ipaddr_to_str(&dns_info.ip);
    return mp_obj_new_tuple(1, &addr);
}

void common_hal_ethernet_ethernet_set_dns(ethernet_ethernet_obj_t *self, mp_obj_t dns_addrs_obj) {
    // TODO: Implement full DNS set with address parsing
}

void common_hal_ethernet_ethernet_set_ipv4_address(ethernet_ethernet_obj_t *self, mp_obj_t ipv4, mp_obj_t netmask, mp_obj_t gateway, mp_obj_t ipv4_dns) {
    if (self->netif == NULL) {
        return;
    }
    common_hal_ethernet_ethernet_stop_dhcp_client(self);

    esp_netif_ip_info_t ip_info;
    _ipv4address_to_esp_idf(ipv4, &ip_info.ip);
    _ipv4address_to_esp_idf(netmask, &ip_info.netmask);
    _ipv4address_to_esp_idf(gateway, &ip_info.gw);

    esp_netif_set_ip_info(self->netif, &ip_info);

    if (ipv4_dns != MP_OBJ_NULL) {
        common_hal_ethernet_ethernet_set_ipv4_dns(self, ipv4_dns);
    }
}

mp_int_t common_hal_ethernet_ethernet_ping(ethernet_ethernet_obj_t *self, mp_obj_t ip_address, mp_float_t timeout) {
    // TODO: Implement ping via esp_ping
    return -1;
}
