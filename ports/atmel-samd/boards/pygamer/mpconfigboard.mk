USB_VID = 0x239A
USB_PID = 0x803E
USB_PRODUCT = "PyGamer"
USB_MANUFACTURER = "Adafruit Industries LLC"

CHIP_VARIANT = SAMD51J19A
CHIP_FAMILY = samd51

QSPI_FLASH_FILESYSTEM = 1
EXTERNAL_FLASH_DEVICES = "GD25Q64C,W25Q64JVxQ"
LONGINT_IMPL = MPZ

CIRCUITPY_AESIO = 0
CIRCUITPY_FLOPPYIO = 0
CIRCUITPY_FRAMEBUFFERIO = 0
CIRCUITPY_GIFIO = 0
CIRCUITPY_I2CDISPLAYBUS = 0
CIRCUITPY_JPEGIO = 0
CIRCUITPY_KEYPAD = 1
CIRCUITPY_PARALLELDISPLAYBUS= 0
CIRCUITPY_STAGE = 1

FROZEN_MPY_DIRS += $(TOP)/frozen/circuitpython-stage/pygamer
