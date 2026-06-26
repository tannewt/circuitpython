// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2025 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "shared-bindings/ethernet/__init__.h"
#include "shared-bindings/ethernet/Ethernet.h"

//| """
//| The `ethernet` module provides necessary low-level functionality for managing
//| wired ethernet connections. Use `socketpool` for communicating over the network.
//|
//| The `ethernet.Ethernet` object is available as ``board.ETHERNET`` on boards
//| with onboard ethernet. The interface is enabled and DHCP is started
//| automatically.
//| """
//|

static const mp_rom_map_elem_t ethernet_module_globals_table[] = {
    // Name
    { MP_ROM_QSTR(MP_QSTR___name__),    MP_ROM_QSTR(MP_QSTR_ethernet) },

    // Classes
    { MP_ROM_QSTR(MP_QSTR_Ethernet),    MP_ROM_PTR(&ethernet_ethernet_type) },
};
static MP_DEFINE_CONST_DICT(ethernet_module_globals, ethernet_module_globals_table);

const mp_obj_module_t ethernet_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&ethernet_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_ethernet, ethernet_module);
