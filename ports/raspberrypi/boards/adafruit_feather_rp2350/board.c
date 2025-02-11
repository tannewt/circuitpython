// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2021 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "py/obj.h"
#include "supervisor/board.h"

#include "shared-module/displayio/__init__.h"
#include "common-hal/picodvi/__init__.h"

// Use the MP_WEAK supervisor/shared/board.c versions of routines not defined here.


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
}
