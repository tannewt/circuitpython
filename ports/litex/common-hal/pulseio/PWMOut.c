/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2017 Scott Shawcroft for Adafruit Industries
 * Copyright (c) 2016 Damien P. George
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

#include <stdint.h>

#include "py/runtime.h"
#include "common-hal/pulseio/PWMOut.h"
#include "shared-bindings/pulseio/PWMOut.h"
#include "shared-bindings/microcontroller/Processor.h"

#include "supervisor/shared/translate.h"

#include "generated/csr.h"

#undef ENABLE


void common_hal_pulseio_pwmout_never_reset(pulseio_pwmout_obj_t *self) {
    //never_reset_pin_number(0, self->pin->number);
}

void common_hal_pulseio_pwmout_reset_ok(pulseio_pwmout_obj_t *self) {
}

void pwmout_reset(void) {
}


pwmout_result_t common_hal_pulseio_pwmout_construct(pulseio_pwmout_obj_t* self,
                                                    const mcu_pin_obj_t* pin,
                                                    uint16_t duty,
                                                    uint32_t frequency,
                                                    bool variable_frequency) {
    self->pin = pin;
    self->variable_frequency = variable_frequency;
    self->duty_cycle = duty;

    return PWMOUT_OK;
}

bool common_hal_pulseio_pwmout_deinited(pulseio_pwmout_obj_t* self) {
    return self->pin == NULL;
}

void common_hal_pulseio_pwmout_deinit(pulseio_pwmout_obj_t* self) {
    if (common_hal_pulseio_pwmout_deinited(self)) {
        return;
    }

    reset_pin_number(0, self->pin->number);
    self->pin = NULL;
}

extern void common_hal_pulseio_pwmout_set_duty_cycle(pulseio_pwmout_obj_t* self, uint16_t duty) {
    self->duty_cycle = duty;

    if(self->pin->number == 0) rgb_r_write(self->duty_cycle);
    if(self->pin->number == 1) rgb_g_write(self->duty_cycle);
    if(self->pin->number == 2) rgb_b_write(self->duty_cycle);
}

uint16_t common_hal_pulseio_pwmout_get_duty_cycle(pulseio_pwmout_obj_t* self) {
    return 0;
}


void common_hal_pulseio_pwmout_set_frequency(pulseio_pwmout_obj_t* self,
                                              uint32_t frequency) {
    if (frequency == 0 || frequency > 6000000) {
        mp_raise_ValueError(translate("Invalid PWM frequency"));
    }

    common_hal_pulseio_pwmout_set_duty_cycle(self, self->duty_cycle);
}

uint32_t common_hal_pulseio_pwmout_get_frequency(pulseio_pwmout_obj_t* self) {

    return 0;

    //return (system_clock / prescaler[divisor]) / (top + 1);
}

bool common_hal_pulseio_pwmout_get_variable_frequency(pulseio_pwmout_obj_t* self) {
    return self->variable_frequency;
}
