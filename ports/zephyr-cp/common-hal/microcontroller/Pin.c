// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2016 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "shared-bindings/microcontroller/Pin.h"
#include "shared-bindings/digitalio/DigitalInOut.h"

#if CIRCUITPY_BUSIO
#include "common-hal/busio/dynamic_bus.h"
#endif

#include "py/mphal.h"

#include <zephyr/drivers/gpio.h>

// Pin claim tracking. Pins are keyed by their mcu_pin_obj_t pointer, which is
// unique per pin. never_reset pins survive soft reloads.
#define MAX_TRACKED_PINS 128

typedef struct {
    const mcu_pin_obj_t *pin;
    bool never_reset;
} tracked_pin_t;

static tracked_pin_t tracked_pins[MAX_TRACKED_PINS];

static tracked_pin_t *find_tracked_pin(const mcu_pin_obj_t *pin) {
    if (pin == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < MAX_TRACKED_PINS; i++) {
        if (tracked_pins[i].pin == pin) {
            return &tracked_pins[i];
        }
    }
    return NULL;
}

static tracked_pin_t *find_free_slot(void) {
    for (size_t i = 0; i < MAX_TRACKED_PINS; i++) {
        if (tracked_pins[i].pin == NULL) {
            return &tracked_pins[i];
        }
    }
    return NULL;
}

// Configure a pin back to a quiescent state: disconnected from any peripheral
// routing, input buffer and driver off, no pulls.
static void deconfigure_pin(const mcu_pin_obj_t *pin) {
    int ret = gpio_pin_configure(pin->port, pin->number, GPIO_DISCONNECTED);
    if (ret == -ENOTSUP) {
        // SoCs without GPIO_DISCONNECTED support settle for a plain input.
        gpio_pin_configure(pin->port, pin->number, GPIO_INPUT);
    }
}

void reset_all_pins(void) {
    // Release dynamically allocated bus peripherals first so that drivers
    // give up their pins before the pads are reconfigured below.
    #if CIRCUITPY_BUSIO
    dynamic_bus_reset_all();
    #endif

    for (size_t i = 0; i < MAX_TRACKED_PINS; i++) {
        tracked_pin_t *tracked = &tracked_pins[i];
        if (tracked->pin == NULL || tracked->never_reset) {
            continue;
        }
        deconfigure_pin(tracked->pin);
        tracked->pin = NULL;
        tracked->never_reset = false;
    }
}

// Mark pin as free and return it to a quiescent state.
void reset_pin(const mcu_pin_obj_t *pin) {
    if (pin == NULL) {
        return;
    }

    tracked_pin_t *tracked = find_tracked_pin(pin);
    if (tracked != NULL) {
        tracked->pin = NULL;
        tracked->never_reset = false;
    }

    deconfigure_pin(pin);
}

void claim_pin(const mcu_pin_obj_t *pin) {
    if (pin == NULL) {
        return;
    }
    tracked_pin_t *tracked = find_tracked_pin(pin);
    if (tracked == NULL) {
        tracked = find_free_slot();
        if (tracked == NULL) {
            // The table is full; the pin cannot be tracked. Future claim
            // checks will still report it as free, which is safe for boards
            // with more GPIOs than we track.
            return;
        }
        tracked->pin = pin;
        tracked->never_reset = false;
    }
}

void never_reset_pin_number(uint8_t pin_number) {
    // Deprecated single-byte pin number API; not used by this port.
    (void)pin_number;
}

void common_hal_never_reset_pin(const mcu_pin_obj_t *pin) {
    if (pin == NULL) {
        return;
    }
    tracked_pin_t *tracked = find_tracked_pin(pin);
    if (tracked == NULL) {
        tracked = find_free_slot();
        if (tracked == NULL) {
            return;
        }
        tracked->pin = pin;
    }
    tracked->never_reset = true;
}

void common_hal_reset_pin(const mcu_pin_obj_t *pin) {
    if (pin == NULL) {
        return;
    }
    reset_pin(pin);
}

bool pin_number_is_free(uint8_t pin_number) {
    // Deprecated single-byte pin number API; not used by this port.
    (void)pin_number;
    return true;
}

bool common_hal_mcu_pin_is_free(const mcu_pin_obj_t *pin) {
    return find_tracked_pin(pin) == NULL;
}

void common_hal_mcu_pin_claim(const mcu_pin_obj_t *pin) {
    claim_pin(pin);
}

uint8_t common_hal_mcu_pin_number(const mcu_pin_obj_t *pin) {
    return pin->number;
}

void common_hal_mcu_pin_claim_number(uint8_t pin_no) {
    (void)pin_no;
}

void common_hal_mcu_pin_reset_number(uint8_t pin_no) {
    (void)pin_no;
}
