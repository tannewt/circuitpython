// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2020 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "supervisor/board.h"

#if CIRCUITPY_ETHERNET
#include "common-hal/ethernet/__init__.h"
#include "shared-bindings/ethernet/Ethernet.h"

#include "esp_eth.h"
#include "esp_eth_mac_esp.h"
#include "esp_eth_phy.h"
#include "esp_event.h"
#include "esp_netif.h"

static void _board_ethernet_init(void) {
    ethernet_ethernet_obj_t *self = &common_hal_ethernet_ethernet_obj;

    // Initialize TCP/IP stack and event loop (may already be done by wifi)
    esp_netif_init();
    esp_event_loop_create_default();

    // Use the P4 default EMAC config — GPIOs already match the Function EV board
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t esp32_emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&esp32_emac_config, &mac_config);

    // Configure IP101 PHY at address 1 with reset on GPIO51
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = 1;
    phy_config.reset_gpio_num = 51;

    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_config);

    // Install ethernet driver
    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_driver_install(&config, &self->eth_handle);

    // Construct and start the ethernet interface
    common_hal_ethernet_ethernet_construct(self);
    common_hal_ethernet_ethernet_start(self);
}
#endif

void board_init(void) {
    #if CIRCUITPY_ETHERNET
    _board_ethernet_init();
    #endif
}

// Use the MP_WEAK supervisor/shared/board.c versions of routines not defined here.
