// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

// Micropython setup

#define MICROPY_HW_BOARD_NAME       "P4 HIL"
#define MICROPY_HW_MCU_NAME         "ESP32P4"

#define MICROPY_HW_NEOPIXEL         (&pin_GPIO52)

#define CIRCUITPY_BOOT_BUTTON       (&pin_GPIO35)

#define DEFAULT_I2C_BUS_SCL         (&pin_GPIO54)
#define DEFAULT_I2C_BUS_SDA         (&pin_GPIO53)

// Swap LS/FS USB so that the FS PHY (GPIO24/25) is used for the device
// instead of the HS PHY. This matches the swap done on the M5Stack Tab5.
#define CIRCUITPY_USB_DEVICE_INSTANCE 0
#define CIRCUITPY_ESP32P4_SWAP_LSFS (1)
