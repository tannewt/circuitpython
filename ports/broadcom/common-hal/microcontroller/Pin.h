// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2021 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

#include <assert.h>
#include <stdint.h>

#include <py/obj.h>

#include "peripherals/broadcom/pins.h"

void reset_all_pins(void);
void common_hal_reset_pin(const mcu_pin_obj_t *pin);
// reset_pin_number takes the pin number instead of the pointer so that objects don't
// need to store a full pointer.
void reset_pin_number(uint8_t pin_number);
void never_reset_pin_number(uint8_t pin_number);
void claim_pin(const mcu_pin_obj_t *pin);
bool pin_number_is_free(uint8_t pin_number);

uint8_t common_hal_mcu_pin_number(const mcu_pin_obj_t *pin);
