/*
 * This file is part of the Micro Python project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * SPDX-FileCopyrightText: Copyright (c) 2022 Scott Shawcroft for Adafruit Industries
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <string.h>

#include "extmod/vfs.h"
#include "lib/oofatfs/ff.h"
#include "lib/oofatfs/diskio.h"
#include "py/mpstate.h"
#include "py/obj.h"
#include "py/objstr.h"
#include "py/runtime.h"
#include "shared-bindings/dotenv/__init__.h"

//| """Functions to manage environment variables from a .env file.
//|
//|    A subset of the CPython `dotenv library <https://saurabh-kumar.com/python-dotenv/>`_. It does
//|    not support variables.
//| """
//|
//| import typing

//| def get_key(dotenv_path: str, key_to_get: str) -> Optional[str]:
//|     """Get the value for the given key from the given .env file.
//|
//|        Returns None if the key isn't found or doesn't have a value."""
//|     ...
//|
STATIC mp_obj_t dotenv_get_key(mp_obj_t path_in, mp_obj_t key_to_get_in) {
    // Use the stack for short values. Longer values will require a heap allocation after we know
    // the length.
    char value[64];
    mp_int_t actual_len = common_hal_dotenv_get_key(mp_obj_str_get_str(path_in),
        mp_obj_str_get_str(key_to_get_in), value, sizeof(value));
    if (actual_len <= 0) {
        return mp_const_none;
    }
    if ((size_t)actual_len >= sizeof(value)) {
        mp_obj_str_t *str = MP_OBJ_TO_PTR(mp_obj_new_str_copy(&mp_type_str, NULL, actual_len + 1));
        common_hal_dotenv_get_key(mp_obj_str_get_str(path_in),
            mp_obj_str_get_str(key_to_get_in), (char *)str->data, actual_len + 1);
        str->hash = qstr_compute_hash(str->data, actual_len);
        return MP_OBJ_FROM_PTR(str);
    }
    return mp_obj_new_str(value, actual_len);
}
MP_DEFINE_CONST_FUN_OBJ_2(dotenv_get_key_obj, dotenv_get_key);

//| def load_dotenv() -> None:
//|     """Does nothing in CircuitPython because os.getenv will automatically read .env when
//|        available.
//|
//|        Present in CircuitPython so CPython-compatible code can use it without error."""
//|     ...
//|
STATIC mp_obj_t dotenv_load_dotenv(void) {
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(dotenv_load_dotenv_obj, dotenv_load_dotenv);

//| def set_key(dotenv_path: str, key_to_set: str, value_to_set: str) -> None:
//|     """Sets the value for the given key in the given .env file.
//|
//|        Raises an exception if the .env file doesn't exist."
//|     ...
//|
STATIC mp_obj_t dotenv_set_key(mp_obj_t path_in, mp_obj_t key_to_set_in, mp_obj_t value_to_set) {
    mp_int_t result = common_hal_dotenv_set_key(
        mp_obj_str_get_str(path_in),
        mp_obj_str_get_str(key_to_set_in),
        mp_obj_str_get_str(value_to_set));
    if (result < 0) {
        mp_raise_OSError(-1 * result);
    }
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_3(dotenv_set_key_obj, dotenv_set_key);

//| def unset_key(dotenv_path: str, key_to_unset: str) -> None:
//|     """Removes the given key in the given .env file.
//|
//|        Raises an exception if the .env file doesn't exist or the key isn't in the file."
//|     ...
//|
STATIC mp_obj_t dotenv_unset_key(mp_obj_t path_in, mp_obj_t key_to_unset_in) {
    mp_int_t result = common_hal_dotenv_unset_key(
        mp_obj_str_get_str(path_in),
        mp_obj_str_get_str(key_to_unset_in));
    if (result < 0) {
        mp_raise_OSError(-1 * result);
    }
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(dotenv_unset_key_obj, dotenv_unset_key);

STATIC const mp_rom_map_elem_t dotenv_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_dotenv) },

    { MP_ROM_QSTR(MP_QSTR_get_key), MP_ROM_PTR(&dotenv_get_key_obj) },
    { MP_ROM_QSTR(MP_QSTR_load_dotenv), MP_ROM_PTR(&dotenv_load_dotenv_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_key), MP_ROM_PTR(&dotenv_set_key_obj) },
    { MP_ROM_QSTR(MP_QSTR_unset_key), MP_ROM_PTR(&dotenv_unset_key_obj) },
};

STATIC MP_DEFINE_CONST_DICT(dotenv_module_globals, dotenv_module_globals_table);

const mp_obj_module_t dotenv_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&dotenv_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_dotenv, dotenv_module, CIRCUITPY_DOTENV);
