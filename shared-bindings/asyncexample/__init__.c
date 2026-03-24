// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2025 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "py/obj.h"
#include "py/runtime.h"
#include "py/circuitpy_objawaitable.h"

#include "shared-bindings/asyncexample/__init__.h"
#include "common-hal/asyncexample/__init__.h"

//| """Example module demonstrating a native C awaitable.
//|
//| This module provides ``delay(ms)`` which returns an awaitable that
//| completes after the given number of milliseconds. It uses the generic
//| ``circuitpy_awaitable_type`` with a Zephyr timer in the common-hal
//| layer, serving as a reference for implementing C-level awaitables.
//|
//| Example::
//|
//|     import asyncio
//|     import asyncexample
//|
//|     async def main():
//|         result = await asyncexample.delay(100)
//|         print(result)  # 100
//|
//|     asyncio.run(main())
//| """

//| async def delay(ms: int) -> int:
//|     """Delay for ``ms`` milliseconds.
//|
//|     :param int ms: number of milliseconds to delay
//|     :return: the number of milliseconds waited (same as the input)
//|     """
//|     ...
//|
CIRCUITPY_DEFINE_ASYNC_FUN_OBJ_1(asyncexample_delay, asyncexample_delay_obj,
    common_hal_asyncexample_delay_end,
    common_hal_asyncexample_delay_cancel) {
    mp_int_t ms = mp_obj_get_int(data);
    if (ms < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("ms must be >= 0"));
    }
    return common_hal_asyncexample_delay_start(flag, ms);
}

static const mp_rom_map_elem_t asyncexample_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_asyncexample) },
    { MP_ROM_QSTR(MP_QSTR_delay), MP_ROM_PTR(&asyncexample_delay_obj) },
};

static MP_DEFINE_CONST_DICT(asyncexample_module_globals, asyncexample_module_globals_table);

const mp_obj_module_t asyncexample_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&asyncexample_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_asyncexample, asyncexample_module);
