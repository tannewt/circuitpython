from ports import raspberrypi

import pathlib

async def build():
    await raspberrypi.board(
        pathlib.Path(__file__).parent.name,
        "Adafruit Feather RP2040",
        usb_vid=0x239A,
        usb_pid=0x80F2,
        usb_product="Feather RP2040",
        usb_manufacturer="Adafruit",
        external_flash_devices=["GD25Q64C", "W25Q64JVxQ"],
    )
