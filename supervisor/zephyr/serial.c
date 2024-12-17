// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2017 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "supervisor/shared/serial.h"

void port_serial_early_init(void) {
}

void port_serial_init(void) {
}

bool port_serial_connected(void) {
    return false;
}

char port_serial_read(void) {
    return -1;
}

uint32_t port_serial_bytes_available(void) {
    return 0;
}

void port_serial_write_substring(const char *text, uint32_t length) {
}
