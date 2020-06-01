/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2017 Scott Shawcroft for Adafruit Industries
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <string.h>

#include "py/mpconfig.h"

#include "supervisor/shared/display.h"
#include "shared-bindings/terminalio/Terminal.h"
#include "supervisor/serial.h"
#include "supervisor/usb.h"
#include "shared-bindings/microcontroller/Pin.h"

#include "tusb.h"

/*
 * Note: DEBUG_UART currently only works on STM32,
 * enabling on another platform will cause a crash.
 */

#if defined(DEBUG_UART_TX) && defined(DEBUG_UART_RX)
#include "shared-bindings/busio/UART.h"
busio_uart_obj_t debug_uart;
byte buf_array[64];
#endif

void serial_early_init(void) {
#if defined(DEBUG_UART_TX) && defined(DEBUG_UART_RX)
    debug_uart.base.type = &busio_uart_type;

    const mcu_pin_obj_t* rx = MP_OBJ_TO_PTR(DEBUG_UART_RX);
    const mcu_pin_obj_t* tx = MP_OBJ_TO_PTR(DEBUG_UART_TX);

    common_hal_busio_uart_construct(&debug_uart, tx, rx, NULL, NULL, NULL,
                                    false, 115200, 8, PARITY_NONE, 1, 1.0f, 64,
                                    buf_array, true);
    common_hal_busio_uart_never_reset(&debug_uart);
#endif
}

void serial_init(void) {
    usb_init();
}

bool serial_connected(void) {
#if defined(DEBUG_UART_TX) && defined(DEBUG_UART_RX)
    return true;
#else
    return tud_cdc_connected();
#endif
}

char serial_read(void) {
#if defined(DEBUG_UART_TX) && defined(DEBUG_UART_RX)
    if (tud_cdc_connected() && tud_cdc_available() > 0) {
        return (char) tud_cdc_read_char();
    }
    int uart_errcode;
    char text;
    common_hal_busio_uart_read(&debug_uart, (uint8_t*) &text, 1, &uart_errcode);
    return text;
#else
    return (char) tud_cdc_read_char();
#endif
}

bool serial_bytes_available(void) {
#if defined(DEBUG_UART_TX) && defined(DEBUG_UART_RX)
    return common_hal_busio_uart_rx_characters_available(&debug_uart) || (tud_cdc_available() > 0);
#else
    return tud_cdc_available() > 0;
#endif
}

void serial_write_substring(const char* text, uint32_t length) {
    if (length == 0) {
        return;
    }
#if CIRCUITPY_DISPLAYIO
    int errcode;
    common_hal_terminalio_terminal_write(&supervisor_terminal, (const uint8_t*) text, length, &errcode);
#endif

    uint32_t count = 0;
    while (count < length && tud_cdc_connected()) {
        count += tud_cdc_write(text + count, length - count);
        usb_background();
    }

#if defined(DEBUG_UART_TX) && defined(DEBUG_UART_RX)
    int uart_errcode;
    common_hal_busio_uart_write(&debug_uart, (const uint8_t*) text, length, &uart_errcode);
#endif
}

void serial_write(const char* text) {
    serial_write_substring(text, strlen(text));
}
