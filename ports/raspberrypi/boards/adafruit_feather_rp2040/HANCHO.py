from hancho import *
import glob

print("hello world")

rp2040 = load("../../HANCHO.py", None)

# CHIP_VARIANT = RP2040
# CHIP_FAMILY = rp2

print("board", glob.glob("*.c"))

rp2040.board(
    "Adafruit Feather RP2040",
    usb_vid=0x239A,
    usb_pid=0x80F2,
    usb_product="Feather RP2040",
    usb_manufacturer="Adafruit",
    external_flash_devices=["GD25Q64C", "W25Q64JVxQ"],
)
