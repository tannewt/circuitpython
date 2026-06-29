// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2017 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common-hal/microcontroller/Pin.h"

#include "py/obj.h"

// Mark a TCC as never-to-be-reset so it survives VM restarts (used by gbio).
void never_reset_tcc(uint8_t index);

typedef struct {
    mp_obj_base_t base;
    const mcu_pin_obj_t *pin;
    const pin_timer_t *timer;
    bool variable_frequency;
    uint16_t duty_cycle;
} pwmio_pwmout_obj_t;
