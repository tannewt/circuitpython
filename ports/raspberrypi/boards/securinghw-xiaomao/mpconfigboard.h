// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2021 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

#define MICROPY_HW_BOARD_NAME "SecuringHardware.com Xiaomao"
#define MICROPY_HW_MCU_NAME "rp2040"

#define MICROPY_HW_NEOPIXEL (&pin_GPIO7)
#define CIRCUITPY_STATUS_LED_POWER (&pin_GPIO6)

#define CIRCUITPY_BOARD_UART        (2)
#define CIRCUITPY_BOARD_UART_PIN    {{.tx = &pin_GPIO16, .rx = &pin_GPIO17}, \
                                     {.tx = &pin_GPIO20, .rx = &pin_GPIO21}}
