// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2024 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include <stdint.h>

#include "py/obj.h"
#include "py/runtime.h"

#include "shared-bindings/lvfontio/__init__.h"
#include "shared-bindings/lvfontio/OnDiskFont.h"

//| """Core font related data structures for LVGL
//|
//| .. note:: This module is intended only for low-level usage with LVGL.
//|
//| """

static const mp_rom_map_elem_t lvfontio_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_lvfontio) },
    { MP_ROM_QSTR(MP_QSTR_OnDiskFont), MP_ROM_PTR(&lvfontio_ondiskfont_type) },
};

static MP_DEFINE_CONST_DICT(lvfontio_module_globals, lvfontio_module_globals_table);

const mp_obj_module_t lvfontio_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&lvfontio_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_lvfontio, lvfontio_module);
