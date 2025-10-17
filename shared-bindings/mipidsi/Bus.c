// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2025 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "py/obj.h"
#include "py/objproperty.h"
#include "py/runtime.h"

#include "shared-bindings/mipidsi/Bus.h"
#include "shared-bindings/util.h"
#include "shared/runtime/context_manager_helpers.h"

//| class Bus:
//|     def __init__(
//|         self,
//|         *,
//|         frequency: int = 500_000_000,
//|         num_lanes: int = 2,
//|     ) -> None:
//|         """Create a MIPI DSI Bus object.
//|
//|         This creates a DSI bus interface. The specific pins used are determined by the board.
//|         DSI supports 1-4 data lanes.
//|
//|         :param int frequency: the high speed clock frequency in Hz (default 500 MHz)
//|         :param int num_lanes: the number of data lanes to use (default 2, range 1-4)
//|         """
//|

static mp_obj_t mipidsi_bus_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_frequency, ARG_num_lanes };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_frequency, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 500000000} },
        { MP_QSTR_num_lanes, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 2} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mipidsi_bus_obj_t *self = mp_obj_malloc(mipidsi_bus_obj_t, &mipidsi_bus_type);

    mp_uint_t frequency = (mp_uint_t)mp_arg_validate_int_min(args[ARG_frequency].u_int, 1, MP_QSTR_frequency);
    uint8_t num_lanes = (uint8_t)mp_arg_validate_int_range(args[ARG_num_lanes].u_int, 1, 4, MP_QSTR_num_lanes);

    common_hal_mipidsi_bus_construct(self, frequency, num_lanes);

    return MP_OBJ_FROM_PTR(self);
}

//|     def deinit(self) -> None:
//|         """Free the resources (pins, timers, etc.) associated with this
//|         `mipidsi.Bus` instance.  After deinitialization, no further operations
//|         may be performed."""
//|         ...
//|
static mp_obj_t mipidsi_bus_deinit(mp_obj_t self_in) {
    mipidsi_bus_obj_t *self = (mipidsi_bus_obj_t *)self_in;
    common_hal_mipidsi_bus_deinit(self);
    return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_1(mipidsi_bus_deinit_obj, mipidsi_bus_deinit);

//|     def __enter__(self) -> Bus:
//|         """No-op used by Context Managers."""
//|         ...
//|
//  Provided by context manager helper.

//|     def __exit__(self) -> None:
//|         """Automatically deinitializes the hardware when exiting a context. See
//|         :ref:`lifetime-and-contextmanagers` for more info."""
//|         ...
//|
static mp_obj_t mipidsi_bus_obj___exit__(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    common_hal_mipidsi_bus_deinit(args[0]);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mipidsi_bus___exit___obj, 4, 4, mipidsi_bus_obj___exit__);

static const mp_rom_map_elem_t mipidsi_bus_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&mipidsi_bus_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&mipidsi_bus___exit___obj) },
};
static MP_DEFINE_CONST_DICT(mipidsi_bus_locals_dict, mipidsi_bus_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    mipidsi_bus_type,
    MP_QSTR_Bus,
    MP_TYPE_FLAG_NONE,
    make_new, mipidsi_bus_make_new,
    locals_dict, &mipidsi_bus_locals_dict
    );
