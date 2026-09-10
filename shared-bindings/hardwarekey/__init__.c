// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Mike Mabey
//
// SPDX-License-Identifier: MIT

#include "py/enum.h"
#include "py/obj.h"
#include "py/runtime.h"

#include "shared-bindings/hardwarekey/__init__.h"
#include "shared-bindings/hardwarekey/HardwareKey.h"

//| """Cryptographic operations with keys held in hardware
//|
//| The ``hardwarekey`` module exposes keys that live in a hardware key store --
//| eFuse, a key manager, a secure element -- and can be *used* but never read
//| back. Application code can compute a MAC with the key; there is no API to
//| read the raw key bytes, and no API to write or burn keys. Provisioning a key
//| is a manufacturing-time step done with vendor tools (for example
//| ``espefuse.py`` on Espressif chips).
//|
//| `HardwareKey` objects are not created by application code. Every hardware key
//| slot the board has is exposed as a fixed object in :mod:`board` (for example
//| ``board.EFUSE_KEY0``), in the same way that pins are. Compute a MAC with one
//| by passing it to `hmac.new()`.
//| """

//| class Purpose:
//|     """What a hardware key slot is provisioned for. Instances are singletons;
//|     compare with ``is``."""
//|
static void hardwarekey_purpose_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    cp_enum_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "%q.%q", MP_QSTR_hardwarekey, self->name);
}

MP_DEFINE_CONST_OBJ_TYPE(
    hardwarekey_purpose_type,
    MP_QSTR_Purpose,
    MP_TYPE_FLAG_NONE,
    print, hardwarekey_purpose_print
    );

//| HMAC_UP: Purpose
//| """The slot holds an HMAC key. It can be used with `hmac.new()`."""
const cp_enum_obj_t hardwarekey_purpose_hmac_obj = {
    { &hardwarekey_purpose_type }, HARDWAREKEY_PURPOSE_HMAC, MP_QSTR_HMAC_UP
};

//| UNUSED: Purpose
//| """No key is burned into the slot (or it is burned for something this module
//| does not expose). The slot's `HardwareKey` still exists but cannot be used."""
//|
const cp_enum_obj_t hardwarekey_purpose_unused_obj = {
    { &hardwarekey_purpose_type }, HARDWAREKEY_PURPOSE_UNUSED, MP_QSTR_UNUSED
};

mp_obj_t hardwarekey_purpose_to_obj(hardwarekey_purpose_t purpose) {
    switch (purpose) {
        case HARDWAREKEY_PURPOSE_HMAC:
            return MP_OBJ_FROM_PTR(&hardwarekey_purpose_hmac_obj);
        default:
            return MP_OBJ_FROM_PTR(&hardwarekey_purpose_unused_obj);
    }
}

static const mp_rom_map_elem_t hardwarekey_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_hardwarekey) },
    { MP_ROM_QSTR(MP_QSTR_HardwareKey), MP_ROM_PTR(&hardwarekey_hardwarekey_type) },
    { MP_ROM_QSTR(MP_QSTR_Purpose), MP_ROM_PTR(&hardwarekey_purpose_type) },
    { MP_ROM_QSTR(MP_QSTR_HMAC_UP), MP_ROM_PTR(&hardwarekey_purpose_hmac_obj) },
    { MP_ROM_QSTR(MP_QSTR_UNUSED), MP_ROM_PTR(&hardwarekey_purpose_unused_obj) },
};
static MP_DEFINE_CONST_DICT(hardwarekey_module_globals, hardwarekey_module_globals_table);

const mp_obj_module_t hardwarekey_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&hardwarekey_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_hardwarekey, hardwarekey_module);
