/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2018 Scott Shawcroft for Adafruit Industries
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

#include <stdint.h>

#include "py/binary.h"
#include "py/obj.h"
#include "py/runtime.h"

#include "shared-bindings/_gbio/__init__.h"

//| """Interface with GameBoy hardware
//|
//| The `_gbio` module manages GameBoy hardware. This module is private and may
//| change without a major version bump. Use the `adafruit_gbio` library for
//| stability instead.
//| """
//|
//|

//| def queue_commands(instructions: ReadableBuffer) -> None:
//|     """Run instructions immediately and block until they finish.
//|
//|     :param instructions: A bytes-like object (bytearray or array of type 'B')
//|         containing the instructions to queue."""
//|     ...
//|
//|
static mp_obj_t gbio_queue_commands(mp_obj_t instructions) {
    mp_buffer_info_t bufinfo;
    if (!mp_get_buffer(instructions, &bufinfo, MP_BUFFER_READ)) {
        mp_raise_TypeError(MP_ERROR_TEXT("buffer must be a bytes-like object"));
    } else if (bufinfo.typecode != 'B' && bufinfo.typecode != BYTEARRAY_TYPECODE) {
        mp_raise_ValueError(MP_ERROR_TEXT("instruction buffer must be a bytearray or array of type 'B'"));
    }
    common_hal_gbio_queue_commands(bufinfo.buf, bufinfo.len);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(gbio_queue_commands_obj, gbio_queue_commands);

//| def queue_vblank_commands(instructions: ReadableBuffer, *, additional_cycles: int) -> None:
//|     """Queue instructions to run at the next vblank. Does not wait for a vblank."""
//|     ...
//|
//|
static mp_obj_t gbio_queue_vblank_commands(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_additional_cycles };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_additional_cycles, MP_ARG_REQUIRED | MP_ARG_KW_ONLY | MP_ARG_INT },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_obj_t instructions = pos_args[0];
    mp_buffer_info_t bufinfo;
    if (!mp_get_buffer(instructions, &bufinfo, MP_BUFFER_READ)) {
        mp_raise_TypeError(MP_ERROR_TEXT("buffer must be a bytes-like object"));
    } else if (bufinfo.typecode != 'B' && bufinfo.typecode != BYTEARRAY_TYPECODE) {
        mp_raise_ValueError(MP_ERROR_TEXT("instruction buffer must be a bytearray or array of type 'B'"));
    }
    common_hal_gbio_queue_vblank_commands(bufinfo.buf, bufinfo.len, args[ARG_additional_cycles].u_int);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(gbio_queue_vblank_commands_obj, 1, gbio_queue_vblank_commands);

//| def set_lcdc(value: int) -> None:
//|     """Set the value of the LCD control register."""
//|     ...
//|
//|
static mp_obj_t gbio_set_lcdc(mp_obj_t value_obj) {
    common_hal_gbio_set_lcdc(MP_OBJ_SMALL_INT_VALUE(value_obj));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(gbio_set_lcdc_obj, gbio_set_lcdc);

//| def get_lcdc() -> int:
//|     """Return the value of the LCDC register."""
//|     ...
//|
//|
static mp_obj_t gbio_get_lcdc(void) {
    return MP_OBJ_NEW_SMALL_INT(common_hal_gbio_get_lcdc());
}
MP_DEFINE_CONST_FUN_OBJ_0(gbio_get_lcdc_obj, gbio_get_lcdc);

//| def get_pressed() -> int:
//|     """Return a bitmask of buttons pressed since the last call."""
//|     ...
//|
//|
static mp_obj_t gbio_get_pressed(void) {
    return MP_OBJ_NEW_SMALL_INT(common_hal_gbio_get_pressed());
}
MP_DEFINE_CONST_FUN_OBJ_0(gbio_get_pressed_obj, gbio_get_pressed);

//| def wait_for_vblank() -> None:
//|     """Wait until the next vblank and then return."""
//|     ...
//|
//|
static mp_obj_t gbio_wait_for_vblank(void) {
    common_hal_gbio_wait_for_vblank();
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(gbio_wait_for_vblank_obj, gbio_wait_for_vblank);

//| def get_vsync_count() -> int:
//|     """Return the frame number."""
//|     ...
//|
//|
static mp_obj_t gbio_get_vsync_count(void) {
    return MP_OBJ_NEW_SMALL_INT(common_hal_gbio_get_vsync_count());
}
MP_DEFINE_CONST_FUN_OBJ_0(gbio_get_vsync_count_obj, gbio_get_vsync_count);


//| def reset_gameboy() -> None:
//|     """Reset the GameBoy."""
//|     ...
//|
//|
static mp_obj_t gbio_reset_gameboy(void) {
    common_hal_gbio_reset_gameboy();
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(gbio_reset_gameboy_obj, gbio_reset_gameboy);


//| def is_color() -> bool:
//|     """Return ``True`` when the cart is in a GameBoy Color."""
//|     ...
//|
//|
static mp_obj_t gbio_is_color(void) {
    return mp_obj_new_bool(common_hal_gbio_is_color());
}
MP_DEFINE_CONST_FUN_OBJ_0(gbio_is_color_obj, gbio_is_color);

static const mp_rom_map_elem_t gbio_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR__gbio) },
    { MP_ROM_QSTR(MP_QSTR_queue_commands), MP_ROM_PTR(&gbio_queue_commands_obj) },
    { MP_ROM_QSTR(MP_QSTR_queue_vblank_commands), MP_ROM_PTR(&gbio_queue_vblank_commands_obj) },

    { MP_ROM_QSTR(MP_QSTR_set_lcdc), MP_ROM_PTR(&gbio_set_lcdc_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_lcdc), MP_ROM_PTR(&gbio_get_lcdc_obj) },

    { MP_ROM_QSTR(MP_QSTR_get_pressed), MP_ROM_PTR(&gbio_get_pressed_obj) },
    { MP_ROM_QSTR(MP_QSTR_wait_for_vblank), MP_ROM_PTR(&gbio_wait_for_vblank_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_vsync_count), MP_ROM_PTR(&gbio_get_vsync_count_obj) },
    { MP_ROM_QSTR(MP_QSTR_reset_gameboy), MP_ROM_PTR(&gbio_reset_gameboy_obj) },
    { MP_ROM_QSTR(MP_QSTR_is_color), MP_ROM_PTR(&gbio_is_color_obj) },
};

static MP_DEFINE_CONST_DICT(gbio_module_globals, gbio_module_globals_table);

const mp_obj_module_t gbio_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&gbio_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR__gbio, gbio_module);
