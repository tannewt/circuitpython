// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2017 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "supervisor/shared/serial.h"

#include "shared-bindings/busio/UART.h"

static busio_uart_obj_t zephyr_console;
static uint8_t buffer[64];

void port_serial_early_init(void) {
    zephyr_console.base.type = &busio_uart_type;
    zephyr_busio_uart_construct(&zephyr_console, DEVICE_DT_GET(DT_CHOSEN(zephyr_console)), sizeof(buffer), buffer);
}

void port_serial_init(void) {
}

bool port_serial_connected(void) {
    return true;
}

char port_serial_read(void) {
    char buf[1];
    size_t count = common_hal_busio_uart_read(&zephyr_console, buf, 1, NULL);
    if (count == 0) {
        return -1;
    }
    return buf[0];
}

uint32_t port_serial_bytes_available(void) {
    return common_hal_busio_uart_rx_characters_available(&zephyr_console);
}

void port_serial_write_substring(const char *text, uint32_t length) {
    common_hal_busio_uart_write(&zephyr_console, text, length, NULL);
}
