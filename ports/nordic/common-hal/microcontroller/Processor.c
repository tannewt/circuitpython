// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2017 Dan Halbert for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "py/runtime.h"

#include "common-hal/microcontroller/Processor.h"
#include "shared-bindings/microcontroller/Processor.h"

#include "common-hal/alarm/__init__.h"
#include "shared-bindings/microcontroller/ResetReason.h"


float common_hal_mcu_processor_get_temperature(void) {
    return 0.0;
}

uint32_t common_hal_mcu_processor_get_frequency(void) {
    return 64000000ul;
}

float common_hal_mcu_processor_get_voltage(void) {
    return 3.3f;
}


void common_hal_mcu_processor_get_uid(uint8_t raw_id[]) {
    for (int i = 0; i < 2; i++) {
        ((uint32_t *)raw_id)[i] = NRF_FICR->DEVICEID[i];
    }
}

mcu_reset_reason_t common_hal_mcu_processor_get_reset_reason(void) {
    mcu_reset_reason_t r = RESET_REASON_UNKNOWN;
    if (reset_reason_saved == 0) {
        r = RESET_REASON_POWER_ON;
    } else if (reset_reason_saved & POWER_RESETREAS_RESETPIN_Msk) {
        r = RESET_REASON_RESET_PIN;
    } else if (reset_reason_saved & POWER_RESETREAS_DOG_Msk) {
        r = RESET_REASON_WATCHDOG;
    } else if (reset_reason_saved & POWER_RESETREAS_SREQ_Msk) {
        r = RESET_REASON_SOFTWARE;
        #if CIRCUITPY_ALARM
        // Our "deep sleep" is still actually light sleep followed by a software
        // reset. Adding this check here ensures we treat it as-if we're waking
        // from deep sleep.
        if (sleepmem_wakeup_event != SLEEPMEM_WAKEUP_BY_NONE) {
            r = RESET_REASON_DEEP_SLEEP_ALARM;
        }
        #endif
    } else if ((reset_reason_saved & POWER_RESETREAS_OFF_Msk) ||
               (reset_reason_saved & POWER_RESETREAS_LPCOMP_Msk) ||
               (reset_reason_saved & POWER_RESETREAS_NFC_Msk) ||
               (reset_reason_saved & POWER_RESETREAS_VBUS_Msk)) {
        r = RESET_REASON_DEEP_SLEEP_ALARM;
    }
    return r;
}
