#include "shared-bindings/board/__init__.h"

STATIC const mp_rom_map_elem_t board_global_dict_table[] = {
  { MP_ROM_QSTR(MP_QSTR_RGB_LED_RED),   MP_ROM_PTR(&pin_LED_R) },
  { MP_ROM_QSTR(MP_QSTR_RGB_LED_GREEN), MP_ROM_PTR(&pin_LED_G) },
  { MP_ROM_QSTR(MP_QSTR_RGB_LED_BLUE),  MP_ROM_PTR(&pin_LED_B) },
};
MP_DEFINE_CONST_DICT(board_module_globals, board_global_dict_table);
