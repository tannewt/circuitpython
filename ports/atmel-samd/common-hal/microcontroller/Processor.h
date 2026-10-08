// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2017 Dan  Halbert for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

#define COMMON_HAL_MCU_PROCESSOR_UID_LENGTH 16

#include "py/obj.h"

#include "samd/adc.h"

// Turn on and calibrate the ADC, then set it up for one 12-bit reading of pos_input against GND.
// gain is used only on SAMD21. Call samd_adc_read() for the reading and adc_sync_deinit() when done.
void samd_adc_start(struct adc_sync_descriptor *adc, Adc *instance,
    uint8_t reference, uint8_t gain, uint8_t pos_input);
// Read twice and return the second reading. The first one after a configuration change is unreliable.
uint16_t samd_adc_read(struct adc_sync_descriptor *adc);

typedef struct {
    mp_obj_base_t base;
    // Stores no state currently.
} mcu_processor_obj_t;
