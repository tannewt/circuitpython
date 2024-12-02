// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2016 Scott Shawcroft
//
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"

#include <zephyr/kernel.h>

typedef struct {
    mp_obj_base_t base;

    const struct device *uart_device;
    struct k_msgq msgq;

    k_timeout_t timeout;

    bool rx_paused;     // set by irq if no space in rbuf
} busio_uart_obj_t;

void uart_reset(void);

void zephyr_busio_uart_construct(busio_uart_obj_t *self, const struct device *const uart_device, uint16_t receiver_buffer_size, byte *receiver_buffer);
