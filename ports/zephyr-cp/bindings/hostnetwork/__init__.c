// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "py/obj.h"

#include "bindings/hostnetwork/__init__.h"
#include "bindings/hostnetwork/HostNetwork.h"

//| """Host networking support for the native simulator."""
//|

static const mp_rom_map_elem_t hostnetwork_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_hostnetwork) },
    { MP_ROM_QSTR(MP_QSTR_HostNetwork), MP_ROM_PTR(&hostnetwork_hostnetwork_type) },
};
static MP_DEFINE_CONST_DICT(hostnetwork_module_globals, hostnetwork_module_globals_table);

const mp_obj_module_t hostnetwork_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&hostnetwork_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_hostnetwork, hostnetwork_module);
