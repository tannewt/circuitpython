#define MICROPY_HW_BOARD_NAME "GameBoy M4 Cart"
#define MICROPY_HW_MCU_NAME "samd51j20"

#define CIRCUITPY_MCU_FAMILY samd51

// These are pins not to reset.
#define MICROPY_PORT_A (PORT_PA04 | PORT_PA05 | PORT_PA06 | PORT_PA07 | PORT_PA11 | 0x00ff0000)
#define MICROPY_PORT_B (PORT_PB22 | 0x0000ffff | PORT_PB17)
#define MICROPY_PORT_C (0)
#define MICROPY_PORT_D (0)

#define AUTORESET_DELAY_MS 500

// If you change this, then make sure to update the linker scripts as well to
// make sure you don't overwrite code
#define CIRCUITPY_INTERNAL_NVM_SIZE 8192

#define BOARD_FLASH_SIZE (FLASH_SIZE - 0x4000 - CIRCUITPY_INTERNAL_NVM_SIZE)

#define CLK0_PIN PIN_PA04
#define A15_PIN PIN_PA05
#define RD0_PIN PIN_PA06

#define RESET_PIN PIN_PB22
#define CLOCK_PIN PIN_PA04
#define WRITE_PIN PIN_PA05

#define DATA_OE_PIN PIN_PA07
#define DATA_DIR_PIN PIN_PA11
