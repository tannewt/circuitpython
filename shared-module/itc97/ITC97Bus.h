// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Hackaday
//
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"
#include "shared-bindings/busio/SPI.h"
#include "shared-bindings/digitalio/DigitalInOut.h"
#include "shared-bindings/displayio/__init__.h"
#include "shared-module/displayio/Group.h"

typedef struct {
    mp_obj_base_t base;
    busio_spi_obj_t bus;
    digitalio_digitalinout_obj_t chip_select_1;
    digitalio_digitalinout_obj_t chip_select_2;
    digitalio_digitalinout_obj_t command;
    digitalio_digitalinout_obj_t reset;
    digitalio_digitalinout_obj_t power;
    // Owned by EPaperDisplay, which outlives transactions on this bus.
    digitalio_digitalinout_obj_t *busy_1;
    digitalio_digitalinout_obj_t *busy_2;
    uint32_t frequency;
    uint32_t plane_bytes_sent;
    uint8_t ram_pointer[3];
    uint8_t polarity;
    uint8_t phase;
    uint8_t last_command;
    bool plane_active;
    bool ram_pointer_valid;
    bool busy_state;
} itc97_itc97bus_obj_t;

void common_hal_itc97_itc97bus_construct(itc97_itc97bus_obj_t *self,
    const mcu_pin_obj_t *sck, const mcu_pin_obj_t *mosi,
    const mcu_pin_obj_t *chip_select_1, const mcu_pin_obj_t *chip_select_2,
    const mcu_pin_obj_t *command, const mcu_pin_obj_t *reset,
    const mcu_pin_obj_t *power, uint32_t baudrate, uint8_t polarity, uint8_t phase);
void common_hal_itc97_itc97bus_set_busy_pins(itc97_itc97bus_obj_t *self,
    digitalio_digitalinout_obj_t *busy_1, digitalio_digitalinout_obj_t *busy_2, bool busy_state);
void common_hal_itc97_itc97bus_deinit(itc97_itc97bus_obj_t *self);
bool common_hal_itc97_itc97bus_reset(mp_obj_t self_in);
bool common_hal_itc97_itc97bus_bus_free(mp_obj_t self_in);
bool common_hal_itc97_itc97bus_begin_transaction(mp_obj_t self_in);
void common_hal_itc97_itc97bus_send(mp_obj_t self_in, display_byte_type_t byte_type,
    display_chip_select_behavior_t chip_select, const uint8_t *data, uint32_t data_length);
void common_hal_itc97_itc97bus_end_transaction(mp_obj_t self_in);
void common_hal_itc97_itc97bus_collect_ptrs(mp_obj_t self_in);
