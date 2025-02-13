// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2024 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#define MICROPY_HW_BOARD_NAME "Adafruit Fruit Jam"
#define MICROPY_HW_MCU_NAME "rp2350b"

#define MICROPY_HW_NEOPIXEL (&pin_GPIO32)
#define MICROPY_HW_NEOPIXEL_COUNT (5)

#define DEFAULT_I2C_BUS_SCL (&pin_GPIO21)
#define DEFAULT_I2C_BUS_SDA (&pin_GPIO20)

#define DEFAULT_SPI_BUS_SCK (&pin_GPIO30)
#define DEFAULT_SPI_BUS_MOSI (&pin_GPIO31)
#define DEFAULT_SPI_BUS_MISO (&pin_GPIO28)

#define DEFAULT_USB_HOST_DATA_PLUS (&pin_GPIO1)
#define DEFAULT_USB_HOST_DATA_MINUS (&pin_GPIO2)
#define DEFAULT_USB_HOST_5V_POWER (&pin_GPIO11)

#define CIRCUITPY_PSRAM_CHIP_SELECT (&pin_GPIO47)

#define CIRCUITPY_CONSOLE_UART_TX (&pin_GPIO10)
#define CIRCUITPY_CONSOLE_UART_RX (&pin_GPIO9)

#define CIRCUITPY_DEBUG_TINYUSB 2
