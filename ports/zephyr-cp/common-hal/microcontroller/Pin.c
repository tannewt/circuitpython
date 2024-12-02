// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2016 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "shared-bindings/microcontroller/Pin.h"
#include "shared-bindings/digitalio/DigitalInOut.h"

#include "py/mphal.h"

// Bit mask of claimed pins on each of up to two ports. nrf52832 has one port; nrf52840 has two.
// static uint32_t claimed_pins[GPIO_COUNT];
// static uint32_t never_reset_pins[GPIO_COUNT];

void reset_all_pins(void) {
    // for (size_t i = 0; i < GPIO_COUNT; i++) {
    //     claimed_pins[i] = never_reset_pins[i];
    // }

    // for (uint32_t pin = 0; pin < NUMBER_OF_PINS; ++pin) {
    //     if ((never_reset_pins[nrf_pin_port(pin)] & (1 << nrf_relative_pin_number(pin))) != 0) {
    //         continue;
    //     }
    //     nrf_gpio_cfg_default(pin);
    // }

    // // After configuring SWD because it may be shared.
    // reset_speaker_enable_pin();
}

// Mark pin as free and return it to a quiescent state.
void reset_pin_number(uint8_t pin_number) {
    if (pin_number == NO_PIN) {
        return;
    }

    // Clear claimed bit.
    // claimed_pins[nrf_pin_port(pin_number)] &= ~(1 << nrf_relative_pin_number(pin_number));
    // never_reset_pins[nrf_pin_port(pin_number)] &= ~(1 << nrf_relative_pin_number(pin_number));
}


void never_reset_pin_number(uint8_t pin_number) {
    if (pin_number == NO_PIN) {
        return;
    }
    // never_reset_pins[nrf_pin_port(pin_number)] |= 1 << nrf_relative_pin_number(pin_number);
}

void common_hal_never_reset_pin(const mcu_pin_obj_t *pin) {
    never_reset_pin_number(pin->number);
}

void common_hal_reset_pin(const mcu_pin_obj_t *pin) {
    if (pin == NULL) {
        return;
    }
    reset_pin_number(pin->number);
}

void claim_pin(const mcu_pin_obj_t *pin) {
    // Set bit in claimed_pins bitmask.
    // claimed_pins[nrf_pin_port(pin->number)] |= 1 << nrf_relative_pin_number(pin->number);
}


bool pin_number_is_free(uint8_t pin_number) {
    return false; // !(claimed_pins[nrf_pin_port(pin_number)] & (1 << nrf_relative_pin_number(pin_number)));
}

bool common_hal_mcu_pin_is_free(const mcu_pin_obj_t *pin) {
    return pin_number_is_free(pin->number);

}

uint8_t common_hal_mcu_pin_number(const mcu_pin_obj_t *pin) {
    return pin->number;
}

void common_hal_mcu_pin_claim(const mcu_pin_obj_t *pin) {
    claim_pin(pin);
}

void common_hal_mcu_pin_reset_number(uint8_t pin_no) {
    reset_pin_number(pin_no);
}
