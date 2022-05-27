USB_VID = 0x239A
USB_PID = 0x8023
USB_PRODUCT = "Feather M0 Express"
USB_MANUFACTURER = "Adafruit Industries LLC"

CHIP_VARIANT = SAMD21G18A
CHIP_FAMILY = samd21

SPI_FLASH_FILESYSTEM = 1
EXTERNAL_FLASH_DEVICES = "S25FL216K, GD25Q16C"
LONGINT_IMPL = NONE

CIRCUITPY_FULL_BUILD = 0

# Build from dc5565a5c is 65664 bytes free in flash.
# With clang, no lto, 12824 bytes are free.

# -Oz not supported in the linker: https://reviews.llvm.org/D63976
