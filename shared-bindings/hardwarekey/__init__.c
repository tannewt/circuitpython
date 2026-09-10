// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Mike Mabey
//
// SPDX-License-Identifier: MIT

#include "py/obj.h"
#include "py/runtime.h"

#include "shared-bindings/hardwarekey/__init__.h"
#include "shared-bindings/hardwarekey/HardwareKey.h"

//| """Cryptographic operations with keys held in hardware
//|
//| The ``hardwarekey`` module exposes keys that live in a hardware key store --
//| eFuse, a key manager, a secure element -- and can be *used* but never read
//| back. Application code can compute a MAC (and, in the future, a signature)
//| with the key; there is no API to read the raw key bytes, and no API to
//| write or burn keys. Provisioning a key is a manufacturing-time step done
//| with vendor tools (for example ``espefuse.py`` on Espressif chips).
//|
//| The operations are portable. Selecting *which* hardware key to use is not:
//| the `HardwareKey` constructor takes a port-defined identifier, in the same
//| way that :mod:`board` pin objects are port-defined.
//|
//| Availability by port:
//|
//| * **espressif** (ESP32-S2/S3/C3/C6/H2/P4): the on-chip HMAC peripheral
//|   against an eFuse key block burned with purpose ``HMAC_UP``.
//| """

static const mp_rom_map_elem_t hardwarekey_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_hardwarekey) },
    { MP_ROM_QSTR(MP_QSTR_HardwareKey), MP_ROM_PTR(&hardwarekey_hardwarekey_type) },
};
static MP_DEFINE_CONST_DICT(hardwarekey_module_globals, hardwarekey_module_globals_table);

const mp_obj_module_t hardwarekey_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&hardwarekey_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_hardwarekey, hardwarekey_module);
