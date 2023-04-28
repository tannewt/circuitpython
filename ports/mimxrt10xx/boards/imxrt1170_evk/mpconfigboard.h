#define MICROPY_HW_BOARD_NAME "iMX RT 1170 EVK"
#define MICROPY_HW_MCU_NAME "IMXRT1176DVMAA"

// If you change this, then make sure to update the linker scripts as well to
// make sure you don't overwrite code
#define CIRCUITPY_INTERNAL_NVM_SIZE 0

#define BOARD_FLASH_SIZE (16 * 1024 * 1024)

#define MICROPY_HW_LED_STATUS (&pin_GPIO_AD_26)
#define MICROPY_HW_LED_STATUS_INVERTED (0)

#define CIRCUITPY_CONSOLE_UART_RX (&pin_GPIO_DISP_B2_11)
#define CIRCUITPY_CONSOLE_UART_TX (&pin_GPIO_DISP_B2_10)

// Put host on the first USB so that right angle OTG adapters can fit. This is
// the right port when looking at the board.
#define CIRCUITPY_USB_DEVICE_INSTANCE 1
#define CIRCUITPY_USB_HOST_INSTANCE 0
