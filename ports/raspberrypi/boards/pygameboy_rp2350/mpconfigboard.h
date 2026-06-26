// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2024 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// PyGameBoy RP2350: an RP2350B-based Game Boy cartridge.
// Pin mapping derived from ~/repos/pcbs/pygb (pygameboy.kicad_pcb, rp2350 branch).

#define MICROPY_HW_BOARD_NAME "PyGameBoy RP2350"
#define MICROPY_HW_MCU_NAME "rp2350b"

// D3 LED: anode to +3V3, cathode via R4 to GPIO45. Drive low to light.
#define MICROPY_HW_LED_STATUS (&pin_GPIO45)
#define MICROPY_HW_LED_STATUS_INVERTED (1)

// J14 STEMMA QT (I2C) connector: SDA=GPIO46, SCL=GPIO47.
#define DEFAULT_I2C_BUS_SDA (&pin_GPIO46)
#define DEFAULT_I2C_BUS_SCL (&pin_GPIO47)

// MIDI UART: GPIO0 = MIDI OUT (TX), GPIO1 = MIDI IN (RX, via H11L1 optocoupler).
#define DEFAULT_UART_BUS_TX (&pin_GPIO0)
#define DEFAULT_UART_BUS_RX (&pin_GPIO1)
