// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "bindings/hostnetwork/HostNetwork.h"

#include "py/objproperty.h"
#include "py/runtime.h"

//| class HostNetwork:
//|     """Native networking for the host simulator."""
//|
//|     def __init__(self) -> None:
//|         """Create a HostNetwork instance."""
//|         ...
//|
static mp_obj_t hostnetwork_hostnetwork_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 0, 0, false);

    hostnetwork_hostnetwork_obj_t *self = mp_obj_malloc(hostnetwork_hostnetwork_obj_t, &hostnetwork_hostnetwork_type);
    common_hal_hostnetwork_hostnetwork_construct(self);
    return MP_OBJ_FROM_PTR(self);
}

//|     port_offset: int
//|     """Offset added to local ports bound by sockets."""
//|
static mp_obj_t hostnetwork_hostnetwork_get_port_offset(mp_obj_t self_in) {
    hostnetwork_hostnetwork_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_int(common_hal_hostnetwork_get_port_offset(self));
}
static MP_DEFINE_CONST_FUN_OBJ_1(hostnetwork_hostnetwork_get_port_offset_obj, hostnetwork_hostnetwork_get_port_offset);

static mp_obj_t hostnetwork_hostnetwork_set_port_offset(mp_obj_t self_in, mp_obj_t offset_in) {
    hostnetwork_hostnetwork_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_int_t offset = mp_obj_get_int(offset_in);
    mp_arg_validate_int_range(offset, 0, UINT16_MAX, MP_QSTR_port_offset);
    common_hal_hostnetwork_set_port_offset(self, (uint16_t)offset);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(hostnetwork_hostnetwork_set_port_offset_obj, hostnetwork_hostnetwork_set_port_offset);

MP_PROPERTY_GETSET(hostnetwork_hostnetwork_port_offset_obj,
    (mp_obj_t)&hostnetwork_hostnetwork_get_port_offset_obj,
    (mp_obj_t)&hostnetwork_hostnetwork_set_port_offset_obj);

static const mp_rom_map_elem_t hostnetwork_hostnetwork_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_port_offset), MP_ROM_PTR(&hostnetwork_hostnetwork_port_offset_obj) },
};
static MP_DEFINE_CONST_DICT(hostnetwork_hostnetwork_locals_dict, hostnetwork_hostnetwork_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    hostnetwork_hostnetwork_type,
    MP_QSTR_HostNetwork,
    MP_TYPE_FLAG_NONE,
    make_new, hostnetwork_hostnetwork_make_new,
    locals_dict, &hostnetwork_hostnetwork_locals_dict
    );
