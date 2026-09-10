// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Mike Mabey
//
// SPDX-License-Identifier: MIT

#include "py/objproperty.h"
#include "py/objstr.h"
#include "py/runtime.h"

#include "shared-bindings/hardwarekey/__init__.h"
#include "shared-bindings/hardwarekey/HardwareKey.h"

#define HMAC_SHA256_DIGEST_SIZE HARDWAREKEY_HMAC_SHA256_DIGEST_SIZE

//| class HardwareKey:
//|     """A key held in a hardware key store, usable but not readable.
//|
//|     This class cannot be instantiated. Every hardware key slot the board has
//|     is exposed as a fixed `HardwareKey` in :mod:`board` -- for example
//|     ``board.EFUSE_KEY0`` -- just like pins. A slot with no key burned into it
//|     still has a `HardwareKey` object; its `purpose` is `hardwarekey.UNUSED`.
//|
//|     On espressif the slots are the eFuse key blocks (``BLOCK_KEY0`` -
//|     ``BLOCK_KEY5``); a slot is usable only if its block was burned with
//|     purpose ``HMAC_UP``."""
//|

static void hardwarekey_hardwarekey_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    hardwarekey_hardwarekey_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->name != MP_QSTRnull) {
        mp_printf(print, "<HardwareKey %q>", self->name);
    } else {
        mp_printf(print, "<HardwareKey slot %d>", (int)self->key_slot);
    }
}

//|     def hmac_sha256(self, data: ReadableBuffer) -> bytes:
//|         """Compute the HMAC-SHA256 of ``data`` with this key and return the
//|         32-byte result. The key is never returned or exposed.
//|
//|         :param ~circuitpython_typing.ReadableBuffer data: the message to authenticate
//|         """
//|         ...
static mp_obj_t hardwarekey_hardwarekey_hmac_sha256(mp_obj_t self_in, mp_obj_t data_in) {
    hardwarekey_hardwarekey_obj_t *self = MP_OBJ_TO_PTR(self_in);

    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(data_in, &bufinfo, MP_BUFFER_READ);

    mp_obj_t result = mp_obj_new_bytes_of_zeros(HMAC_SHA256_DIGEST_SIZE);
    mp_obj_str_t *result_bytes = MP_OBJ_TO_PTR(result);

    common_hal_hardwarekey_hardwarekey_hmac_sha256(self, bufinfo.buf, bufinfo.len,
        (uint8_t *)result_bytes->data, HMAC_SHA256_DIGEST_SIZE);
    return result;
}
static MP_DEFINE_CONST_FUN_OBJ_2(hardwarekey_hardwarekey_hmac_sha256_obj, hardwarekey_hardwarekey_hmac_sha256);

//|     def verify_hmac_sha256(self, data: ReadableBuffer, mac: ReadableBuffer) -> bool:
//|         """Return ``True`` if ``mac`` is the correct HMAC-SHA256 of ``data``
//|         for this key. The comparison is constant-time.
//|
//|         :param ~circuitpython_typing.ReadableBuffer data: the message that was authenticated
//|         :param ~circuitpython_typing.ReadableBuffer mac: the MAC to check
//|         """
//|         ...
static mp_obj_t hardwarekey_hardwarekey_verify_hmac_sha256(mp_obj_t self_in, mp_obj_t data_in, mp_obj_t mac_in) {
    hardwarekey_hardwarekey_obj_t *self = MP_OBJ_TO_PTR(self_in);

    mp_buffer_info_t data_info;
    mp_get_buffer_raise(data_in, &data_info, MP_BUFFER_READ);
    mp_buffer_info_t mac_info;
    mp_get_buffer_raise(mac_in, &mac_info, MP_BUFFER_READ);

    bool ok = common_hal_hardwarekey_hardwarekey_verify_hmac_sha256(self,
        data_info.buf, data_info.len, mac_info.buf, mac_info.len);
    return mp_obj_new_bool(ok);
}
static MP_DEFINE_CONST_FUN_OBJ_3(hardwarekey_hardwarekey_verify_hmac_sha256_obj, hardwarekey_hardwarekey_verify_hmac_sha256);

//|     key_slot: int
//|     """The port-defined key identifier this handle is bound to. On espressif,
//|     the eFuse key block index. (read-only)"""
static mp_obj_t hardwarekey_hardwarekey_get_key_slot(mp_obj_t self_in) {
    hardwarekey_hardwarekey_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return MP_OBJ_NEW_SMALL_INT(common_hal_hardwarekey_hardwarekey_get_key_slot(self));
}
MP_DEFINE_CONST_FUN_OBJ_1(hardwarekey_hardwarekey_get_key_slot_obj, hardwarekey_hardwarekey_get_key_slot);
MP_PROPERTY_GETTER(hardwarekey_hardwarekey_key_slot_obj, (mp_obj_t)&hardwarekey_hardwarekey_get_key_slot_obj);

//|     purpose: Purpose
//|     """What this key slot is provisioned for -- `hardwarekey.HMAC_UP` or
//|     `hardwarekey.UNUSED`. (read-only)"""
//|
static mp_obj_t hardwarekey_hardwarekey_get_purpose(mp_obj_t self_in) {
    hardwarekey_hardwarekey_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return hardwarekey_purpose_to_obj(common_hal_hardwarekey_hardwarekey_get_purpose(self));
}
MP_DEFINE_CONST_FUN_OBJ_1(hardwarekey_hardwarekey_get_purpose_obj, hardwarekey_hardwarekey_get_purpose);
MP_PROPERTY_GETTER(hardwarekey_hardwarekey_purpose_obj, (mp_obj_t)&hardwarekey_hardwarekey_get_purpose_obj);

//|     exportable: bool
//|     """Whether the raw key bytes can ever leave the hardware. Always
//|     informational -- it does not gate `hmac_sha256`.
//|
//|     On espressif this is ``False`` once the key block's ``RD_DIS`` eFuse
//|     bit is set (which ``espefuse.py`` does by default). It is meant for
//|     manufacturing-time self-test code to confirm a key block was burned as
//|     expected. (read-only)"""
static mp_obj_t hardwarekey_hardwarekey_get_exportable(mp_obj_t self_in) {
    hardwarekey_hardwarekey_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_bool(common_hal_hardwarekey_hardwarekey_get_exportable(self));
}
MP_DEFINE_CONST_FUN_OBJ_1(hardwarekey_hardwarekey_get_exportable_obj, hardwarekey_hardwarekey_get_exportable);
MP_PROPERTY_GETTER(hardwarekey_hardwarekey_exportable_obj, (mp_obj_t)&hardwarekey_hardwarekey_get_exportable_obj);

static const mp_rom_map_elem_t hardwarekey_hardwarekey_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_hmac_sha256), MP_ROM_PTR(&hardwarekey_hardwarekey_hmac_sha256_obj) },
    { MP_ROM_QSTR(MP_QSTR_verify_hmac_sha256), MP_ROM_PTR(&hardwarekey_hardwarekey_verify_hmac_sha256_obj) },
    { MP_ROM_QSTR(MP_QSTR_key_slot), MP_ROM_PTR(&hardwarekey_hardwarekey_key_slot_obj) },
    { MP_ROM_QSTR(MP_QSTR_purpose), MP_ROM_PTR(&hardwarekey_hardwarekey_purpose_obj) },
    { MP_ROM_QSTR(MP_QSTR_exportable), MP_ROM_PTR(&hardwarekey_hardwarekey_exportable_obj) },
};
static MP_DEFINE_CONST_DICT(hardwarekey_hardwarekey_locals_dict, hardwarekey_hardwarekey_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    hardwarekey_hardwarekey_type,
    MP_QSTR_HardwareKey,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    print, hardwarekey_hardwarekey_print,
    locals_dict, &hardwarekey_hardwarekey_locals_dict
    );
