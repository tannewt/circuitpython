// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Hackaday
//
// SPDX-License-Identifier: MIT

#include "board.h"
#include "mpconfigboard.h"
#include "shared-bindings/board/__init__.h"
#include "shared-module/displayio/__init__.h"

CIRCUITPY_BOARD_BUS_SINGLETON(epd_spi, spi, 0)
CIRCUITPY_BOARD_BUS_SINGLETON(i2c_sao1, i2c, 1)
CIRCUITPY_BOARD_BUS_SINGLETON(i2c_sao2, i2c, 2)
CIRCUITPY_BOARD_BUS_SINGLETON(i2c_sao3, i2c, 3)

static mp_map_elem_t board_module_globals_table[] = {
    CIRCUITPYTHON_MUTABLE_BOARD_DICT_STANDARD_ITEMS

    // Buttons
    { MP_ROM_QSTR(MP_QSTR_BUTTON_BOOT), MP_OBJ_FROM_PTR(&pin_GPIO0) },
    { MP_ROM_QSTR(MP_QSTR_BUTTON_MENU_MINUS), MP_OBJ_FROM_PTR(&pin_GPIO40) },
    { MP_ROM_QSTR(MP_QSTR_BUTTON_MENU_PLUS), MP_OBJ_FROM_PTR(&pin_GPIO15) },
    { MP_ROM_QSTR(MP_QSTR_BUTTON_USER_A), MP_OBJ_FROM_PTR(&pin_GPIO39) },
    { MP_ROM_QSTR(MP_QSTR_BUTTON_USER_B), MP_OBJ_FROM_PTR(&pin_GPIO16) },

    { MP_ROM_QSTR(MP_QSTR_BOOT), MP_OBJ_FROM_PTR(&pin_GPIO0) },
    { MP_ROM_QSTR(MP_QSTR_MENU_MINUS), MP_OBJ_FROM_PTR(&pin_GPIO40) },
    { MP_ROM_QSTR(MP_QSTR_MENU_PLUS), MP_OBJ_FROM_PTR(&pin_GPIO15) },
    { MP_ROM_QSTR(MP_QSTR_USER_A), MP_OBJ_FROM_PTR(&pin_GPIO39) },
    { MP_ROM_QSTR(MP_QSTR_USER_B), MP_OBJ_FROM_PTR(&pin_GPIO16) },

    // LEDs
    { MP_ROM_QSTR(MP_QSTR_STATUS_LED), MP_OBJ_FROM_PTR(&pin_GPIO1) },
    { MP_ROM_QSTR(MP_QSTR_LED), MP_OBJ_FROM_PTR(&pin_GPIO1) },
    { MP_ROM_QSTR(MP_QSTR_NEOPIXEL), MP_OBJ_FROM_PTR(&pin_GPIO38) },

    // Battery voltage divider
    { MP_ROM_QSTR(MP_QSTR_BATTERY_SENSE), MP_OBJ_FROM_PTR(&pin_GPIO14) },
    { MP_ROM_QSTR(MP_QSTR_BATTERY_VOLTAGE), MP_OBJ_FROM_PTR(&pin_GPIO14) },

    // Shared I2C bus and SAO ports
    { MP_ROM_QSTR(MP_QSTR_SDA), MP_OBJ_FROM_PTR(&pin_GPIO17) },
    { MP_ROM_QSTR(MP_QSTR_SCL), MP_OBJ_FROM_PTR(&pin_GPIO18) },

    { MP_ROM_QSTR(MP_QSTR_SAO_SDA), MP_OBJ_FROM_PTR(&pin_GPIO17) },
    { MP_ROM_QSTR(MP_QSTR_SAO_SCL), MP_OBJ_FROM_PTR(&pin_GPIO18) },

    { MP_ROM_QSTR(MP_QSTR_SAO1_GPIO1), MP_OBJ_FROM_PTR(&pin_GPIO2) },
    { MP_ROM_QSTR(MP_QSTR_SAO1_GPIO2), MP_OBJ_FROM_PTR(&pin_GPIO42) },
    { MP_ROM_QSTR(MP_QSTR_SAO2_GPIO1), MP_OBJ_FROM_PTR(&pin_GPIO4) },
    { MP_ROM_QSTR(MP_QSTR_SAO2_GPIO2), MP_OBJ_FROM_PTR(&pin_GPIO5) },
    { MP_ROM_QSTR(MP_QSTR_SAO3_GPIO1), MP_OBJ_FROM_PTR(&pin_GPIO7) },
    { MP_ROM_QSTR(MP_QSTR_SAO3_GPIO2), MP_OBJ_FROM_PTR(&pin_GPIO6) },

    // E-Paper display
    { MP_ROM_QSTR(MP_QSTR_EPD_CS1), MP_OBJ_FROM_PTR(&pin_GPIO13) },
    { MP_ROM_QSTR(MP_QSTR_EPD_CS2), MP_OBJ_FROM_PTR(&pin_GPIO3) },
    { MP_ROM_QSTR(MP_QSTR_EPD_BUSY1), MP_OBJ_FROM_PTR(&pin_GPIO12) },
    { MP_ROM_QSTR(MP_QSTR_EPD_BUSY2), MP_OBJ_FROM_PTR(&pin_GPIO8) },
    { MP_ROM_QSTR(MP_QSTR_EPD_DC), MP_OBJ_FROM_PTR(&pin_GPIO10) },
    { MP_ROM_QSTR(MP_QSTR_EPD_RST), MP_OBJ_FROM_PTR(&pin_GPIO11) },
    { MP_ROM_QSTR(MP_QSTR_EPD_RESET), MP_OBJ_FROM_PTR(&pin_GPIO11) },
    { MP_ROM_QSTR(MP_QSTR_EPD_SCK), MP_OBJ_FROM_PTR(&pin_GPIO21) },
    { MP_ROM_QSTR(MP_QSTR_EPD_SCL), MP_OBJ_FROM_PTR(&pin_GPIO21) },
    { MP_ROM_QSTR(MP_QSTR_EPD_MOSI), MP_OBJ_FROM_PTR(&pin_GPIO47) },
    { MP_ROM_QSTR(MP_QSTR_EPD_SDA), MP_OBJ_FROM_PTR(&pin_GPIO47) },
    { MP_ROM_QSTR(MP_QSTR_EPD_MISO), MP_OBJ_FROM_PTR(&pin_GPIO45) },
    { MP_ROM_QSTR(MP_QSTR_EPD_PWR_ENABLE), MP_OBJ_FROM_PTR(&pin_GPIO48) },
    { MP_ROM_QSTR(MP_QSTR_EPD_PWR_EN), MP_OBJ_FROM_PTR(&pin_GPIO48) },
    { MP_ROM_QSTR(MP_QSTR_EPD_POWER), MP_OBJ_FROM_PTR(&pin_GPIO48) },

    // UART test points
    { MP_ROM_QSTR(MP_QSTR_TX), MP_OBJ_FROM_PTR(&pin_GPIO43) },
    { MP_ROM_QSTR(MP_QSTR_RX), MP_OBJ_FROM_PTR(&pin_GPIO44) },
    { MP_ROM_QSTR(MP_QSTR_IO43), MP_OBJ_FROM_PTR(&pin_GPIO43) },
    { MP_ROM_QSTR(MP_QSTR_IO44), MP_OBJ_FROM_PTR(&pin_GPIO44) },

    // Free GPIOs
    { MP_ROM_QSTR(MP_QSTR_IO9), MP_OBJ_FROM_PTR(&pin_GPIO9) },
    { MP_ROM_QSTR(MP_QSTR_IO41), MP_OBJ_FROM_PTR(&pin_GPIO41) },

    // Filled in by board_init(): 1 = B1, 84 = 084, 0 = unrecognized OTP.
    { MP_ROM_QSTR(MP_QSTR_ITC97_VARIANT), MP_OBJ_NEW_SMALL_INT(0) },

    // Buses
    { MP_ROM_QSTR(MP_QSTR_I2C), MP_OBJ_FROM_PTR(&board_i2c_obj) },
    { MP_ROM_QSTR(MP_QSTR_I2C_SAO1), MP_OBJ_FROM_PTR(&board_i2c_sao1_obj) },
    { MP_ROM_QSTR(MP_QSTR_I2C_SAO2), MP_OBJ_FROM_PTR(&board_i2c_sao2_obj) },
    { MP_ROM_QSTR(MP_QSTR_I2C_SAO3), MP_OBJ_FROM_PTR(&board_i2c_sao3_obj) },
    { MP_ROM_QSTR(MP_QSTR_EPD_SPI), MP_OBJ_FROM_PTR(&board_epd_spi_obj) },
    { MP_ROM_QSTR(MP_QSTR_UART), MP_OBJ_FROM_PTR(&board_uart_obj) },
    { MP_ROM_QSTR(MP_QSTR_DISPLAY), mp_const_none },
};
MP_DEFINE_MUTABLE_DICT(board_module_globals, board_module_globals_table);

void board_set(qstr q, mp_obj_t value) {
    mp_obj_t key = MP_OBJ_NEW_QSTR(q);
    for (size_t i = 0; i < MP_ARRAY_SIZE(board_module_globals_table); i++) {
        if (board_module_globals_table[i].key == key) {
            board_module_globals_table[i].value = value;
            return;
        }
    }
}
