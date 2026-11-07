// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Hackaday
//
// SPDX-License-Identifier: MIT

#include "shared-bindings/itc97/ITC97Bus.h"

#include "py/obj.h"
#include "py/runtime.h"
#include "shared-bindings/microcontroller/Pin.h"
#include "shared-bindings/util.h"
#include "shared-module/displayio/__init__.h"

//| class ITC97Bus:
//|     """Display bus for the Pervasive Displays iTC97 9.7-inch dual-COG E-paper panel.
//|
//|     This bus owns the SPI clock/data pins, both chip selects, the data/command
//|     pin, the panel reset, and the active-low panel power enable. It is intended
//|     to be used with `epaperdisplay.EPaperDisplay`. Supply the COG busy pins to
//|     ``EPaperDisplay`` so this bus can wait for booster readiness and confirm
//|     BUSY assertion after triggering a refresh.
//|     """
//|
//|     def __init__(
//|         self,
//|         *,
//|         sck: microcontroller.Pin,
//|         mosi: microcontroller.Pin,
//|         chip_select_1: microcontroller.Pin,
//|         chip_select_2: microcontroller.Pin,
//|         command: microcontroller.Pin,
//|         reset: microcontroller.Pin,
//|         power: Optional[microcontroller.Pin] = None,
//|         baudrate: int = 4000000,
//|         polarity: int = 0,
//|         phase: int = 0,
//|     ) -> None:
//|         """Create an ITC97 bus.
//|
//|         The panel power pin is active-low: the bus drives it high at
//|         construction and performs the full power/reset sequence at the start
//|         of every display refresh.
//|
//|         :param microcontroller.Pin sck: SPI clock pin
//|         :param microcontroller.Pin mosi: SPI data pin
//|         :param microcontroller.Pin chip_select_1: Master COG chip select
//|         :param microcontroller.Pin chip_select_2: Slave COG chip select
//|         :param microcontroller.Pin command: Data/command pin
//|         :param microcontroller.Pin reset: Panel reset pin
//|         :param microcontroller.Pin power: Active-low panel power enable
//|         :param int baudrate: SPI baudrate for plane transfers. Register writes use 50 kHz.
//|         :param int polarity: SPI clock polarity
//|         :param int phase: SPI clock phase
//|         """
//|         ...
//|
static mp_obj_t itc97_itc97bus_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum {
        ARG_sck, ARG_mosi, ARG_chip_select_1, ARG_chip_select_2,
        ARG_command, ARG_reset, ARG_power, ARG_baudrate, ARG_polarity, ARG_phase,
        NUM_ARGS
    };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_sck, MP_ARG_OBJ | MP_ARG_KW_ONLY | MP_ARG_REQUIRED, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_mosi, MP_ARG_OBJ | MP_ARG_KW_ONLY | MP_ARG_REQUIRED, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_chip_select_1, MP_ARG_OBJ | MP_ARG_KW_ONLY | MP_ARG_REQUIRED, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_chip_select_2, MP_ARG_OBJ | MP_ARG_KW_ONLY | MP_ARG_REQUIRED, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_command, MP_ARG_OBJ | MP_ARG_KW_ONLY | MP_ARG_REQUIRED, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_reset, MP_ARG_OBJ | MP_ARG_KW_ONLY | MP_ARG_REQUIRED, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_power, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = mp_const_none} },
        { MP_QSTR_baudrate, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 4000000} },
        { MP_QSTR_polarity, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0} },
        { MP_QSTR_phase, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    MP_STATIC_ASSERT(MP_ARRAY_SIZE(allowed_args) == NUM_ARGS);
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    const mcu_pin_obj_t *sck = validate_obj_is_free_pin(args[ARG_sck].u_obj, MP_QSTR_sck);
    const mcu_pin_obj_t *mosi = validate_obj_is_free_pin(args[ARG_mosi].u_obj, MP_QSTR_mosi);
    const mcu_pin_obj_t *chip_select_1 = validate_obj_is_free_pin(args[ARG_chip_select_1].u_obj, MP_QSTR_chip_select_1);
    const mcu_pin_obj_t *chip_select_2 = validate_obj_is_free_pin(args[ARG_chip_select_2].u_obj, MP_QSTR_chip_select_2);
    const mcu_pin_obj_t *command = validate_obj_is_free_pin(args[ARG_command].u_obj, MP_QSTR_command);
    const mcu_pin_obj_t *reset = validate_obj_is_free_pin(args[ARG_reset].u_obj, MP_QSTR_reset);
    const mcu_pin_obj_t *power = validate_obj_is_free_pin_or_none(args[ARG_power].u_obj, MP_QSTR_power);

    uint8_t polarity = (uint8_t)mp_arg_validate_int_range(args[ARG_polarity].u_int, 0, 1, MP_QSTR_polarity);
    uint8_t phase = (uint8_t)mp_arg_validate_int_range(args[ARG_phase].u_int, 0, 1, MP_QSTR_phase);

    itc97_itc97bus_obj_t *self = &allocate_display_bus_or_raise()->itc97_bus;
    self->base.type = &itc97_itc97bus_type;
    common_hal_itc97_itc97bus_construct(self, sck, mosi, chip_select_1, chip_select_2,
        command, reset, power, args[ARG_baudrate].u_int, polarity, phase);
    return MP_OBJ_FROM_PTR(self);
}

//|     def reset(self) -> None:
//|         """Perform the panel power-on and reset sequence."""
//|         ...
//|
static mp_obj_t itc97_itc97bus_reset(mp_obj_t self_in) {
    common_hal_itc97_itc97bus_reset(self_in);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(itc97_itc97bus_reset_obj, itc97_itc97bus_reset);

static const mp_rom_map_elem_t itc97_itc97bus_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_reset), MP_ROM_PTR(&itc97_itc97bus_reset_obj) },
};
static MP_DEFINE_CONST_DICT(itc97_itc97bus_locals_dict, itc97_itc97bus_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    itc97_itc97bus_type,
    MP_QSTR_ITC97Bus,
    MP_TYPE_FLAG_NONE,
    make_new, itc97_itc97bus_make_new,
    locals_dict, &itc97_itc97bus_locals_dict
    );
