// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2024 Blues Wireless Contributors.
//
// SPDX-License-Identifier: MIT

#pragma once

// Micropython setup

#define MICROPY_HW_BOARD_NAME       "Cygnet"
#define MICROPY_HW_MCU_NAME         "STM32L433CCT6"

#define MICROPY_PY_SYS_PLATFORM     MICROPY_HW_BOARD_NAME

#define STM32L433XX
#define BOARD_CYGNET

#define LSE_VALUE ((uint32_t)32768)
#define BOARD_HAS_LOW_SPEED_CRYSTAL (1)
#define BOARD_HAS_HIGH_SPEED_CRYSTAL (0)

// Increase drive strength of 32kHz external crystal, in line with calculations specified in ST AN2867 sections 3.3, 3.4, and STM32L4 datasheet DS12023 Table 58. LSE oscillator characteristics.
// The drive strength RCC_LSEDRIVE_LOW is marginal for the 32kHz crystal oscillator stability, and RCC_LSEDRIVE_MEDIUMLOW meets the calculated drive strength with a small margin for parasitic capacitance.
#define BOARD_LSE_DRIVE_LEVEL RCC_LSEDRIVE_MEDIUMLOW

// Bootloader only
#ifdef UF2_BOOTLOADER_ENABLED
    #define BOARD_VTOR_DEFER (1) // Leave VTOR relocation to bootloader
#endif

#define BOARD_NO_VBUS_SENSE (1)
#define BOARD_NO_USB_OTG_ID_SENSE (1)

#define DEFAULT_I2C_BUS_SCL (&pin_PB06)
#define DEFAULT_I2C_BUS_SDA (&pin_PB07)

#define DEFAULT_SPI_BUS_SS (&pin_PB08)
#define DEFAULT_SPI_BUS_SCK (&pin_PA05)
#define DEFAULT_SPI_BUS_MOSI (&pin_PB05)
#define DEFAULT_SPI_BUS_MISO (&pin_PB06)

#define DEFAULT_UART_BUS_RX (&pin_PA10)
#define DEFAULT_UART_BUS_TX (&pin_PA09)

#define CYGNET_DISCHARGE_3V3 (&pin_PH01)
#define CYGNET_ENABLE_3V3 (&pin_PH00)
