// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2017 Dan Halbert for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "py/runtime.h"

#include "common-hal/microcontroller/Processor.h"
#include "shared-bindings/microcontroller/Processor.h"

#include "shared-bindings/microcontroller/ResetReason.h"


float common_hal_mcu_processor_get_temperature(void) {
    return 0.0;
}

extern uint32_t SystemCoreClock;
uint32_t common_hal_mcu_processor_get_frequency(void) {
    return SystemCoreClock;
}

float common_hal_mcu_processor_get_voltage(void) {
    return 3.3f;
}

void common_hal_mcu_processor_get_uid(uint8_t raw_id[]) {
}

mcu_reset_reason_t common_hal_mcu_processor_get_reset_reason(void) {
    mcu_reset_reason_t r = RESET_REASON_UNKNOWN;
    return r;
}
