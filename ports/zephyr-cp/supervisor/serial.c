// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2017 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "supervisor/shared/serial.h"

#include "supervisor/zephyr-cp.h"

#include "shared-bindings/busio/UART.h"
static busio_uart_obj_t uart_console;
static uint8_t uart_buffer[64];

#if CIRCUITPY_USB_DEVICE == 1
#include "shared-bindings/usb_cdc/Serial.h"
static usb_cdc_serial_obj_t *usb_console;
static bool use_usb_console;
#endif

static void uart_console_init(void) {
    uart_console.base.type = &busio_uart_type;
    common_hal_busio_uart_construct_from_device(&uart_console, DEVICE_DT_GET(DT_CHOSEN(zephyr_console)), sizeof(uart_buffer), uart_buffer);
}

void port_serial_early_init(void) {
    #if CIRCUITPY_USB_DEVICE == 1
    use_usb_console = native_sim_usb_enabled();
    if (!use_usb_console) {
        uart_console_init();
    }
    #else
    uart_console_init();
    #endif
}

void port_serial_init(void) {
    #if CIRCUITPY_USB_DEVICE == 1
    if (use_usb_console) {
        usb_console = usb_cdc_serial_get_console();
        #if defined(CONFIG_ARCH_POSIX)
        if (usb_console == NULL) {
            use_usb_console = false;
            uart_console_init();
        }
        #endif
    }
    #endif
}

bool port_serial_connected(void) {
    #if CIRCUITPY_USB_DEVICE == 1
    if (use_usb_console) {
        if (usb_console == NULL) {
            return false;
        }
        return common_hal_usb_cdc_serial_get_connected(usb_console);
    }
    #endif
    return true;
}

char port_serial_read(void) {
    char buf[1];
    size_t count;

    #if CIRCUITPY_USB_DEVICE == 1
    if (use_usb_console) {
        if (usb_console == NULL) {
            return -1;
        }
        count = common_hal_usb_cdc_serial_read(usb_console, buf, 1, NULL);
        if (count == 0) {
            return -1;
        }
        return buf[0];
    }
    #endif

    count = common_hal_busio_uart_read(&uart_console, buf, 1, NULL);
    if (count == 0) {
        return -1;
    }
    return buf[0];
}

uint32_t port_serial_bytes_available(void) {
    #if CIRCUITPY_USB_DEVICE == 1
    if (use_usb_console) {
        if (usb_console == NULL) {
            return 0;
        }
        return common_hal_usb_cdc_serial_get_in_waiting(usb_console);
    }
    #endif

    return common_hal_busio_uart_rx_characters_available(&uart_console);
}

void port_serial_write_substring(const char *text, uint32_t length) {
    #if CIRCUITPY_USB_DEVICE == 1
    if (use_usb_console) {
        if (usb_console != NULL) {
            common_hal_usb_cdc_serial_write(usb_console, text, length, NULL);
        }
        return;
    }
    #endif

    common_hal_busio_uart_write(&uart_console, text, length, NULL);
}
