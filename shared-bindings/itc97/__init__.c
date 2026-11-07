// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Hackaday
//
// SPDX-License-Identifier: MIT

#include <stdint.h>

#include "py/obj.h"
#include "py/runtime.h"

#include "shared-bindings/itc97/__init__.h"
#include "shared-bindings/itc97/ITC97Bus.h"

//| """Display bus support for Pervasive Displays iTC97 E-paper panels"""
//|
//| from .ITC97Bus import ITC97Bus

static const mp_rom_map_elem_t itc97_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_itc97) },
    { MP_ROM_QSTR(MP_QSTR_ITC97Bus), MP_ROM_PTR(&itc97_itc97bus_type) },
};
static MP_DEFINE_CONST_DICT(itc97_module_globals, itc97_module_globals_table);

const mp_obj_module_t itc97_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&itc97_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_itc97, itc97_module);
