// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2024 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#ifndef MICROPY_INCLUDED_RASPBERRYPI_COMMON_HAL_GBIO___INIT___H
#define MICROPY_INCLUDED_RASPBERRYPI_COMMON_HAL_GBIO___INIT___H

// Called once from port_init() to set up the game boy cartridge interface
// (PIO state machines, DMA channel, pins). Safe to call even when no game boy
// is connected.
void gbio_init(void);

// Debug helper: print a hex dump of gb_data_buffer[start..start+len-1].
// Useful for tracking memory corruption in the 64K buffer.
void gbio_print_memory_range(uint16_t start, uint16_t len);

#endif  // MICROPY_INCLUDED_RASPBERRYPI_COMMON_HAL_GBIO___INIT___H
