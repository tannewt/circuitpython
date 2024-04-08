/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2021 Scott Shawcroft for Adafruit Industries
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

#include <math.h>
#include <string.h>

#include "py/mphal.h"
#include "py/runtime.h"
#include "common-hal/microcontroller/Processor.h"
#include "shared-bindings/microcontroller/Processor.h"
#include "shared-bindings/microcontroller/ResetReason.h"
#include "shared-bindings/time/__init__.h"

#include "pico/stdlib.h"
#include "src/rp2_common/hardware_adc/include/hardware/adc.h"
#include "src/rp2_common/hardware_clocks/include/hardware/clocks.h"
#include "src/rp2_common/hardware_vreg/include/hardware/vreg.h"
#include "src/rp2_common/hardware_watchdog/include/hardware/watchdog.h"

#include "src/rp2040/hardware_regs/include/hardware/regs/vreg_and_chip_reset.h"
#include "src/rp2040/hardware_regs/include/hardware/regs/watchdog.h"
#include "src/rp2040/hardware_structs/include/hardware/structs/vreg_and_chip_reset.h"
#include "src/rp2040/hardware_structs/include/hardware/structs/watchdog.h"

float common_hal_mcu_processor_get_temperature(void) {
    adc_init();
    adc_set_temp_sensor_enabled(true);
    adc_select_input(4);
    uint16_t value = adc_read();
    adc_set_temp_sensor_enabled(false);
    float voltage = value * 3.3f / (1 << 12);
    // TODO: turn the ADC back off
    return 27 - (voltage - 0.706f) / 0.001721f;
}

float common_hal_mcu_processor_get_voltage(void) {
    return NAN;
}

uint32_t common_hal_mcu_processor_get_frequency(void) {
    return clock_get_hz(clk_sys);
}

void common_hal_mcu_processor_set_frequency(mcu_processor_obj_t *self, uint32_t frequency) {
    uint vco, postdiv1, postdiv2;
    uint32_t freq_khz = frequency / 1000;
    if (!check_sys_clock_khz(freq_khz, &vco, &postdiv1, &postdiv2)) {
        mp_arg_error_invalid(MP_QSTR_frequency);
    }
    // These voltages are approximate based on the PicoDVI examples.
    enum vreg_voltage voltage = VREG_VOLTAGE_1_10;
    if (freq_khz >= 400000) {
        voltage = VREG_VOLTAGE_1_30;
    } else if (freq_khz >= 300000) {
        voltage = VREG_VOLTAGE_1_20;
    } else if (freq_khz > 133000) {
        voltage = VREG_VOLTAGE_1_20;
    }
    vreg_set_voltage(voltage);
    // Wait for a stable voltage
    common_hal_time_delay_ms(10);
    set_sys_clock_khz(freq_khz, false);
}

void common_hal_mcu_processor_get_uid(uint8_t raw_id[]) {
    pico_unique_board_id_t retrieved_id;
    pico_get_unique_board_id(&retrieved_id);
    memcpy(raw_id, retrieved_id.id, COMMON_HAL_MCU_PROCESSOR_UID_LENGTH);
}

mcu_reset_reason_t common_hal_mcu_processor_get_reset_reason(void) {
    mcu_reset_reason_t reason = RESET_REASON_UNKNOWN;

    uint32_t chip_reset_reg = vreg_and_chip_reset_hw->chip_reset;

    if (chip_reset_reg & VREG_AND_CHIP_RESET_CHIP_RESET_HAD_PSM_RESTART_BITS) {
        reason = RESET_REASON_RESCUE_DEBUG;
    }

    if (chip_reset_reg & VREG_AND_CHIP_RESET_CHIP_RESET_HAD_RUN_BITS) {
        reason = RESET_REASON_RESET_PIN;
    }

    if (chip_reset_reg & VREG_AND_CHIP_RESET_CHIP_RESET_HAD_POR_BITS) {
        // NOTE: This register is also used for brownout, but there is no way to differentiate between power on and brown out
        reason = RESET_REASON_POWER_ON;
    }

    // Check watchdog after chip reset since watchdog doesn't clear chip_reset, while chip_reset clears the watchdog

    // The watchdog is used for software reboots such as resetting after copying a UF2 via the bootloader.
    if (watchdog_caused_reboot()) {
        reason = RESET_REASON_SOFTWARE;
    }

    // Actual watchdog usage will set a special value that this function detects.
    if (watchdog_enable_caused_reboot()) {
        reason = RESET_REASON_WATCHDOG;
    }

    return reason;
}
