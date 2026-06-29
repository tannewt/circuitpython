#include "shared-bindings/board/__init__.h"

// This mapping only includes functional names because pins broken
// out on connectors are labeled with their MCU name available from
// microcontroller.pin.
static const mp_rom_map_elem_t board_global_dict_table[] = {
    CIRCUITPYTHON_BOARD_DICT_STANDARD_ITEMS

    { MP_OBJ_NEW_QSTR(MP_QSTR_LED),  (mp_obj_t)&pin_PB23 },

    { MP_OBJ_NEW_QSTR(MP_QSTR_MIDI_IN),  (mp_obj_t)&pin_PA00 },
    { MP_OBJ_NEW_QSTR(MP_QSTR_MIDI_OUT),  (mp_obj_t)&pin_PB31 },

    { MP_OBJ_NEW_QSTR(MP_QSTR_DEBUG),  (mp_obj_t)&pin_PB17 },

    { MP_ROM_QSTR(MP_QSTR_I2C), MP_ROM_PTR(&board_i2c_obj) },
    { MP_ROM_QSTR(MP_QSTR_SPI), MP_ROM_PTR(&board_spi_obj) },
    { MP_ROM_QSTR(MP_QSTR_UART), MP_ROM_PTR(&board_uart_obj) },
};
MP_DEFINE_CONST_DICT(board_module_globals, board_global_dict_table);
