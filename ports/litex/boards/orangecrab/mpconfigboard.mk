USB_VID = 0x1209
USB_PID = 0x5AF0
USB_PRODUCT = "OrangeCrab"
USB_MANUFACTURER = "GsD"
USB_DEVICES = "CDC,HID,MSC"

INTERNAL_FLASH_FILESYSTEM = 1
LONGINT_IMPL = MPZ

# Chip supplied serial number, in bytes
USB_SERIAL_NUMBER_LENGTH = 16

# The default queue depth of 16 overflows on release builds,
# so increase it to 32.
CFLAGS += -DCFG_TUD_TASK_QUEUE_SZ=32

# Fomu only implements rv32i
CFLAGS += -march=rv32im -mabi=ilp32 -DORANGECRAB
LDFLAGS += -march=rv32im -mabi=ilp32

CIRCUITPY_DIGITALIO = 1
CIRCUITPY_MICROCONTROLLER = 1
CIRCUITPY_NEOPIXEL_WRITE = 0
