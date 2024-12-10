// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2017 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "supervisor/shared/serial.h"
#include "py/ringbuf.h"

#include <zephyr/autoconf.h>

// #if defined(CONFIG_CONSOLE_SUBSYS)

#include <zephyr/console/console.h>
#include <zephyr/sys/ring_buffer.h>

// init the console ourselves so we can set the timeout to zero.
static struct tty_serial console_serial;

static uint8_t console_rxbuf[CONFIG_CONSOLE_GETCHAR_BUFSIZE];
static uint8_t console_txbuf[CONFIG_CONSOLE_PUTCHAR_BUFSIZE];

void port_serial_early_init(void) {
    const struct device *uart_dev;
    int ret;

    uart_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
    if (!device_is_ready(uart_dev)) {
        return -ENODEV;
    }

    ret = tty_init(&console_serial, uart_dev);

    if (ret) {
        return ret;
    }

    /* Checks device driver supports for interrupt driven data transfers. */
    if (CONFIG_CONSOLE_GETCHAR_BUFSIZE + CONFIG_CONSOLE_PUTCHAR_BUFSIZE) {
        const struct uart_driver_api *api =
            (const struct uart_driver_api *)uart_dev->api;
        if (!api->irq_callback_set) {
            return -ENOTSUP;
        }
    }

    tty_set_tx_buf(&console_serial, console_txbuf, sizeof(console_txbuf));
    tty_set_rx_buf(&console_serial, console_rxbuf, sizeof(console_rxbuf));

    printk("port_serial_early_init done");
}

void port_serial_init(void) {
}

bool port_serial_connected(void) {
    return true;
}

char port_serial_read(void) {
    char buf[1];
    ssize_t read = tty_read(&console_serial, buf, 1);
    if (read == 0) {
        return -1;
    }
    return buf[0];
}

uint32_t port_serial_bytes_available(void) {
    _fill_ring_buf();
    return ring_buf_size_get(&rb);
}

void port_serial_write_substring(const char *text, uint32_t length) {
    uint32_t total_written = 0;
    while (total_written < length) {
        total_written += tty_write(NULL, text + total_written, length - total_written);
    }
}

// #endif
