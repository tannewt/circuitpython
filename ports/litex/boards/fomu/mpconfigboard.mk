USB_VID = 0x1209
USB_PID = 0x5BF0
USB_PRODUCT = "Fomu"
USB_MANUFACTURER = "Foosn"

INTERNAL_FLASH_FILESYSTEM = 1
LONGINT_IMPL = MPZ

# Chip supplied serial number, in bytes
USB_SERIAL_NUMBER_LENGTH = 30

# The default queue depth of 16 overflows on release builds,
# so increase it to 32.
CFLAGS += -DCFG_TUD_TASK_QUEUE_SZ=32

# Fomu only implements rv32i
CFLAGS += -march=rv32i -mabi=ilp32 -DFOMU
LDFLAGS += -march=rv32i -mabi=ilp32

CIRCUITPY_NEOPIXEL_WRITE = 1
CIRCUITPY_DIGITALIO = 1
CIRCUITPY_MICROCONTROLLER = 1
CIRCUITPY_PULSEIO = 0
