// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2025 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "common-hal/ethernet/__init__.h"
#include "shared-bindings/ethernet/__init__.h"
#include "shared-bindings/ethernet/Ethernet.h"

#include "bindings/espidf/__init__.h"

#include "supervisor/port.h"
#include "supervisor/workflow.h"

#if CIRCUITPY_STATUS_BAR
#include "supervisor/shared/status_bar.h"
#endif

#include "esp_eth.h"
#include "esp_eth_netif_glue.h"
#include "esp_event.h"
#include "esp_netif.h"

#include <string.h>

#define MAC_ADDRESS_LENGTH 6

ethernet_ethernet_obj_t common_hal_ethernet_ethernet_obj;

static void _eth_event_handler(void *arg, esp_event_base_t event_base,
    int32_t event_id, void *event_data) {
    ethernet_ethernet_obj_t *self = (ethernet_ethernet_obj_t *)arg;

    if (event_base == ETH_EVENT) {
        switch (event_id) {
            case ETHERNET_EVENT_CONNECTED:
                self->connected = true;
                break;
            case ETHERNET_EVENT_DISCONNECTED:
                self->connected = false;
                break;
            default:
                break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        self->ip_info = event->ip_info;
    }

    #if CIRCUITPY_STATUS_BAR
    supervisor_status_bar_request_update(false);
    #endif
    port_wake_main_task();
}

void common_hal_ethernet_ethernet_construct(ethernet_ethernet_obj_t *self) {
    self->base.type = &ethernet_ethernet_type;
    self->started = false;
    self->connected = false;
    self->eth_handle = NULL;
    self->netif = NULL;
    self->glue = NULL;

    // The actual MAC/PHY initialization is board-specific and must be done
    // in board.c before calling this function. The board sets self->eth_handle.
    // If no eth_handle is set, the interface won't work.
}

void common_hal_ethernet_ethernet_start(ethernet_ethernet_obj_t *self) {
    if (self->eth_handle == NULL || self->started) {
        return;
    }

    // Create netif with default ethernet config
    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    self->netif = esp_netif_new(&netif_config);

    // Create glue to attach ethernet driver to netif
    self->glue = esp_eth_new_netif_glue(self->eth_handle);
    CHECK_ESP_RESULT(esp_netif_attach(self->netif, self->glue));

    // Register event handlers
    CHECK_ESP_RESULT(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, _eth_event_handler, self));
    CHECK_ESP_RESULT(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, _eth_event_handler, self));

    // Set hostname
    uint8_t mac[MAC_ADDRESS_LENGTH];
    esp_eth_ioctl(self->eth_handle, ETH_CMD_G_MAC_ADDR, mac);
    char hostname[32];
    snprintf(hostname, sizeof(hostname), "cpy-%02x%02x%02x%02x%02x%02x",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    esp_netif_set_hostname(self->netif, hostname);

    // Start the ethernet driver (DHCP starts automatically with default netif config)
    CHECK_ESP_RESULT(esp_eth_start(self->eth_handle));
    self->started = true;
}

void ethernet_user_reset(void) {
    // Nothing to do on user reset for now.
}
