// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Romain Boutrois
//
// SPDX-License-Identifier: MIT

#pragma once

// Micropython setup

#define MICROPY_HW_BOARD_NAME       "Eliobot"
#define MICROPY_HW_MCU_NAME         "ESP32S3"
#define CIRCUITPY_DRIVE_LABEL       "ELIOBOT"

#define MICROPY_HW_NEOPIXEL (&pin_GPIO1)
#define MICROPY_HW_NEOPIXEL_COUNT (8)

#define DEFAULT_I2C_BUS_SCL (&pin_GPIO9)
#define DEFAULT_I2C_BUS_SDA (&pin_GPIO8)

#define DEFAULT_SPI_BUS_SCK  (&pin_GPIO39)
#define DEFAULT_SPI_BUS_MOSI (&pin_GPIO40)
#define DEFAULT_SPI_BUS_MISO (&pin_GPIO41)

#define DEFAULT_UART_BUS_RX (&pin_GPIO44)
#define DEFAULT_UART_BUS_TX (&pin_GPIO43)
