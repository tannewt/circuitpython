#define MICROPY_HW_BOARD_NAME "Metro MIMXRT1011"
#define MICROPY_HW_MCU_NAME "IMXRT1011DAE5A"

#define MICROPY_HW_NEOPIXEL (&pin_GPIO_00)
#define MICROPY_HW_LED_STATUS (&pin_GPIO_03)

// If you change this, then make sure to update the linker scripts as well to
// make sure you don't overwrite code
#define CIRCUITPY_INTERNAL_NVM_SIZE 0

#define BOARD_FLASH_SIZE (8 * 1024 * 1024)

#define DEFAULT_I2C_BUS_SCL (&pin_GPIO_02)
#define DEFAULT_I2C_BUS_SDA (&pin_GPIO_01)

#define DEFAULT_SPI_BUS_SCK  (&pin_GPIO_AD_06)
#define DEFAULT_SPI_BUS_MOSI (&pin_GPIO_AD_04)
#define DEFAULT_SPI_BUS_MISO (&pin_GPIO_AD_03)

#define DEFAULT_UART_BUS_RX (&pin_GPIO_09)
#define DEFAULT_UART_BUS_TX (&pin_GPIO_10)
