#define MICROPY_HW_BOARD_NAME "Adafruit Pyrate"
#define MICROPY_HW_MCU_NAME "rp2040"

#define MICROPY_HW_NEOPIXEL (&pin_GPIO16)

// Don't give these to CircuitPython because we'll do it from user code for the
// second CDC connection.
// #define MICROPY_HW_LED_TX   (&pin_GPIO08)
// #define MICROPY_HW_LED_RX   (&pin_GPIO09)

#define CIRCUITPY_BOARD_I2C         (2)
#define CIRCUITPY_BOARD_I2C_PIN     {{.scl = &pin_GPIO1, .sda = &pin_GPIO0}, \
                                     {.scl = &pin_GPIO19, .sda = &pin_GPIO18}}

#define DEFAULT_UART_BUS_TX (&pin_GPIO0)
#define DEFAULT_UART_BUS_RX (&pin_GPIO1)

#define DEFAULT_SPI_BUS_SCK (&pin_GPIO2)
#define DEFAULT_SPI_BUS_MOSI (&pin_GPIO3)
#define DEFAULT_SPI_BUS_MISO (&pin_GPIO0)
