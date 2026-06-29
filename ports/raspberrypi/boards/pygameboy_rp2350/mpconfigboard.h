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

// J14 STEMMA QT connector, repurposed as a UART serial console (output + input):
//   SDA (GPIO46) = console TX, SCL (GPIO47) = console RX.
// This routes the CircuitPython REPL / print() output to the STEMMA QT pins over UART
// (115200-8-N-1) instead of I2C. The pins are claimed at boot and never reset, so
// board.I2C() on these pins is no longer available.
#define CIRCUITPY_CONSOLE_UART_TX (&pin_GPIO46)
#define CIRCUITPY_CONSOLE_UART_RX (&pin_GPIO47)

// MIDI UART: GPIO0 = MIDI OUT (TX), GPIO1 = MIDI IN (RX, via H11L1 optocoupler).
#define DEFAULT_UART_BUS_TX (&pin_GPIO0)
#define DEFAULT_UART_BUS_RX (&pin_GPIO1)
