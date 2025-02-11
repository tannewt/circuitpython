// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2021 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "common-hal/microcontroller/Pin.h"
#include "hardware/gpio.h"
#include "shared-bindings/usb_host/Port.h"
#include "supervisor/board.h"

#include "shared-module/displayio/__init__.h"
#include "common-hal/picodvi/__init__.h"

// Use the MP_WEAK supervisor/shared/board.c versions of routines not defined here.


#if defined(DEFAULT_USB_HOST_5V_POWER)
bool board_reset_pin_number(uint8_t pin_number) {
    if (pin_number == DEFAULT_USB_HOST_5V_POWER->number) {
        // doing this (rather than gpio_init) in this specific order ensures no
        // glitch if pin was already configured as a high output. gpio_init() temporarily
        // configures the pin as an input, so the power enable value would potentially
        // glitch.
        gpio_put(pin_number, 1);
        gpio_set_dir(pin_number, GPIO_OUT);
        gpio_set_function(pin_number, GPIO_FUNC_SIO);

        return true;
    }
    return false;
}
#endif

extern void mp_module_board_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest);

void mp_module_board_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    if (attr == MP_QSTR_DISPLAY && dest[0] == MP_OBJ_NULL) {
        mp_obj_base_t *first_display = &displays[0].display_base;
        if (first_display->type != &mp_type_NoneType && first_display->type != NULL) {
            dest[0] = MP_OBJ_FROM_PTR(first_display);
        }
    }
}

MP_REGISTER_MODULE_DELEGATION(board_module, mp_module_board_attr);

void board_init(void) {
    picodvi_autoconstruct();

    common_hal_usb_host_port_construct(DEFAULT_USB_HOST_DATA_PLUS, DEFAULT_USB_HOST_DATA_MINUS);
}
