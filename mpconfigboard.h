
#pragma once

#define MICROPY_HW_BOARD_NAME "Microchip Curiosity CircuitPython Nano"
#define MICROPY_HW_MCU_NAME "same51j20"
#define CIRCUITPY_MCU_FAMILY samd51

#define MICROPY_HW_LED_STATUS   (&pin_PB23)
#define MICROPY_HW_NEOPIXEL (&pin_PB22)

#define EXTERNAL_FLASH_QSPI_DUAL

#define BOARD_HAS_CRYSTAL 1

// USB is always used internally so skip the pin objects for it.
#define IGNORE_PIN_PA24     1
#define IGNORE_PIN_PA25     1

#define DEFAULT_I2C_BUS_SCL (&pin_PB31)
#define DEFAULT_I2C_BUS_SDA (&pin_PB30)

#define LCD_SPI_BUS_SCK (&pin_PA05)
#define LCD_SPI_BUS_MOSI (&pin_PA04)
#define LCD_SPI_BUS_CS (&pin_PA07)
#define LCD_BACKLIGHT (&pin_PA06)

#define SDCARD_SPI_BUS_SCK (&pin_PA17)
#define SDCARD_SPI_BUS_MOSI (&pin_PA16)
#define SDCARD_SPI_BUS_MISO (&pin_PA18)
#define SDCARD_SPI_BUS_CS (&pin_PA19)

#define DEFAULT_CAN_BUS_TX (&pin_PB12)
#define DEFAULT_CAN_BUS_RX (&pin_PB13)
#define DEFAULT_CAN_BUS_STDBY (&pin_PB17)

#define DEFAULT_UART_BUS_RX (&pin_PA23)
#define DEFAULT_UART_BUS_TX (&pin_PA22)
