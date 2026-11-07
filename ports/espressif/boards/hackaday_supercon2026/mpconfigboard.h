// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Hackaday
//
// SPDX-License-Identifier: MIT

#pragma once

// Micropython setup

#define MICROPY_HW_BOARD_NAME       "Hackaday Supercon 2026 Billboard Badge"
#define MICROPY_HW_MCU_NAME         "ESP32-S3-WROOM-1-N8R8"

// Eight WS2812 LEDs used for the CircuitPython status LED
#define MICROPY_HW_NEOPIXEL         (&pin_GPIO38)
#define MICROPY_HW_NEOPIXEL_COUNT   (8)

// board_init() fills in the detected panel variant and display object.
#define CIRCUITPY_MUTABLE_BOARD     (1)

// BOOT button on GPIO0
#define CIRCUITPY_BOOT_BUTTON       (&pin_GPIO0)

// Shared I2C bus on the SAO ports, plus a bus on each SAO's GPIO1/GPIO2 pins
#define CIRCUITPY_BOARD_I2C         (4)
#define CIRCUITPY_BOARD_I2C_PIN     {{.scl = &pin_GPIO18, .sda = &pin_GPIO17}, \
                                     {.scl = &pin_GPIO2, .sda = &pin_GPIO42}, \
                                     {.scl = &pin_GPIO4, .sda = &pin_GPIO5}, \
                                     {.scl = &pin_GPIO7, .sda = &pin_GPIO6}}

// SPI bus used by the E-Paper display
#define CIRCUITPY_BOARD_SPI         (1)
#define CIRCUITPY_BOARD_SPI_PIN     {{.clock = &pin_GPIO21, .mosi = &pin_GPIO47}}

// UART test points on the back of the badge
#define DEFAULT_UART_BUS_TX         (&pin_GPIO43)
#define DEFAULT_UART_BUS_RX         (&pin_GPIO44)
