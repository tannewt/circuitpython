# SPDX-FileCopyrightText: 2026 Scott Shawcroft for Adafruit Industries
# SPDX-License-Identifier: MIT

"""USB/IP + host-mass-storage smoke tests for native_sim."""

from __future__ import annotations

import array
import os
import struct
import time

import pytest


USBIP_WAIT_CODE = """\
import time

time.sleep(8)
"""

USB_VID = 0x1209
USB_PID = 0x000C
DRIVE_TEST_FILE = "HELLO.TXT"
DRIVE_TEST_CONTENT = "hello from usb host mass storage test\n"


class _FlashBackedMassStorageDevice:
    """Minimal USB MSC transport shim backed by the native_sim flash image."""

    _SIG_CBW = b"USBC"
    _SIG_CSW = b"USBS"

    def __init__(self, flash_path):
        self.idVendor = USB_VID
        self.idProduct = USB_PID

        self._flash = open(flash_path, "r+b")
        self._block_size = 512
        self._last_lba = os.path.getsize(flash_path) // self._block_size - 1

        self._tag = 0
        self._pending_in = b""
        self._pending_csw = b""
        self._pending_write = None

    def close(self):
        self._flash.close()

    def set_configuration(self, _config_value):
        return None

    def ctrl_transfer(self, bm_request_type, b_request, w_value, _w_index, data):
        # Standard GET_DESCRIPTOR
        if bm_request_type == 0x80 and b_request == 0x06:
            desc_type = (w_value >> 8) & 0xFF
            desc_index = w_value & 0xFF

            if desc_type == 0x01 and desc_index == 0:
                return self._copy_descriptor(data, self._device_descriptor())
            if desc_type == 0x02 and desc_index == 0:
                return self._copy_descriptor(data, self._configuration_descriptor())

        # Mass Storage class GET_MAX_LUN
        if bm_request_type == 0xA1 and b_request == 0xFE:
            return self._copy_descriptor(data, b"\x00")

        raise RuntimeError("unsupported control transfer in flash-backed test device")

    def _copy_descriptor(self, target, payload):
        count = min(len(target), len(payload))
        self._fill_target(target, payload[:count])
        return count

    def _device_descriptor(self):
        return struct.pack(
            "<BBHBBBBHHHBBBB",
            18,  # bLength
            0x01,  # DEVICE descriptor
            0x0200,  # bcdUSB
            0x00,  # bDeviceClass
            0x00,  # bDeviceSubClass
            0x00,  # bDeviceProtocol
            64,  # bMaxPacketSize0
            self.idVendor,
            self.idProduct,
            0x0100,  # bcdDevice
            0,  # iManufacturer
            0,  # iProduct
            0,  # iSerialNumber
            1,  # bNumConfigurations
        )

    def _configuration_descriptor(self):
        # Single-interface BOT MSC configuration:
        #  - interface class/subclass/proto: 08/06/50
        #  - endpoints: 0x81 IN bulk, 0x01 OUT bulk
        return bytes(
            (
                # Configuration descriptor
                9,
                0x02,
                32,
                0,
                1,
                1,
                0,
                0x80,
                50,
                # Interface descriptor
                9,
                0x04,
                0,
                0,
                2,
                0x08,
                0x06,
                0x50,
                0,
                # Endpoint IN descriptor
                7,
                0x05,
                0x81,
                0x02,
                64,
                0,
                0,
                # Endpoint OUT descriptor
                7,
                0x05,
                0x01,
                0x02,
                64,
                0,
                0,
            )
        )

    def write(self, _endpoint, data):
        payload = bytes(data)

        if len(payload) == 31 and payload[:4] == self._SIG_CBW:
            self._handle_cbw(payload)
            return len(payload)

        if self._pending_write is not None:
            self._handle_write_data(payload)
            return len(payload)

        return len(payload)

    def read(self, _endpoint, target):
        length = len(target)

        if self._pending_in:
            chunk = self._pending_in[:length]
            self._pending_in = self._pending_in[len(chunk) :]
            self._fill_target(target, chunk)
            return len(chunk)

        if self._pending_csw:
            chunk = self._pending_csw[:length]
            self._pending_csw = self._pending_csw[len(chunk) :]
            self._fill_target(target, chunk)
            return len(chunk)

        self._fill_target(target, b"\x00" * length)
        return length

    def _fill_target(self, target, data):
        if isinstance(target, array.array):
            target[: len(data)] = array.array("B", data)
        else:
            target[: len(data)] = data

    def _build_csw(self, status):
        return struct.pack("<4sIIB", self._SIG_CSW, self._tag, 0, status)

    def _read_blocks(self, lba, block_count):
        self._flash.seek(lba * self._block_size)
        return self._flash.read(block_count * self._block_size)

    def _handle_write_data(self, data):
        state = self._pending_write
        take = min(len(data), state["remaining"])
        state["buffer"].extend(data[:take])
        state["remaining"] -= take

        if state["remaining"] == 0:
            self._flash.seek(state["lba"] * self._block_size)
            self._flash.write(state["buffer"])
            self._flash.flush()
            self._pending_write = None
            self._pending_csw = self._build_csw(0)

    def _handle_cbw(self, cbw):
        self._pending_in = b""
        self._pending_csw = b""

        self._tag = struct.unpack_from("<I", cbw, 4)[0]
        transfer_len = struct.unpack_from("<I", cbw, 8)[0]
        cb = cbw[15:31]
        opcode = cb[0]

        # INQUIRY
        if opcode == 0x12:
            inquiry = bytearray(36)
            inquiry[0] = 0x00
            inquiry[1] = 0x80
            inquiry[2] = 0x00
            inquiry[3] = 0x01
            inquiry[4] = 31
            inquiry[8:16] = b"PYUSBIP "
            inquiry[16:32] = b"CIRCUITPY DRIVE "
            inquiry[32:36] = b"1.0 "
            self._pending_in = bytes(inquiry[:transfer_len])
            self._pending_csw = self._build_csw(0)
            return

        # TEST UNIT READY
        if opcode == 0x00:
            self._pending_csw = self._build_csw(0)
            return

        # REQUEST SENSE
        if opcode == 0x03:
            sense = bytearray(18)
            sense[0] = 0x70
            sense[7] = 10
            self._pending_in = bytes(sense[:transfer_len])
            self._pending_csw = self._build_csw(0)
            return

        # READ CAPACITY (10)
        if opcode == 0x25:
            capacity = struct.pack(">II", self._last_lba, self._block_size)
            self._pending_in = capacity[:transfer_len]
            self._pending_csw = self._build_csw(0)
            return

        # READ (10)
        if opcode == 0x28:
            lba = struct.unpack(">I", cb[2:6])[0]
            blocks = struct.unpack(">H", cb[7:9])[0]
            data = self._read_blocks(lba, blocks)
            self._pending_in = data[:transfer_len]
            self._pending_csw = self._build_csw(0)
            return

        # WRITE (10)
        if opcode == 0x2A:
            lba = struct.unpack(">I", cb[2:6])[0]
            blocks = struct.unpack(">H", cb[7:9])[0]
            total = blocks * self._block_size
            if total == 0:
                self._pending_csw = self._build_csw(0)
            else:
                self._pending_write = {
                    "lba": lba,
                    "remaining": total,
                    "buffer": bytearray(),
                }
            return

        self._pending_csw = self._build_csw(1)


@pytest.mark.circuitpy_drive({"code.py": USBIP_WAIT_CODE})
@pytest.mark.native_sim_usb
@pytest.mark.native_sim_rt
@pytest.mark.duration(30)
def test_usbip_pyusb_find_device(circuitpython):
    """Use the PyUSB backend to find the exported native_sim device."""

    usb_core = pytest.importorskip("usb.core")
    usb_util = pytest.importorskip("usb.util")
    usbip_backend = pytest.importorskip("usbip_backend")

    backend = usbip_backend.get_backend("127.0.0.1", timeout=1.0)

    # Avoid repeatedly reconnecting while native_sim is still completing
    # early USB bring-up. A single query after startup is much more stable.
    time.sleep(4.0)

    try:
        dev = usb_core.find(backend=backend, idVendor=USB_VID, idProduct=USB_PID)
    except usb_core.USBError as exc:
        pytest.fail(f"pyusb usbip query failed: {exc}")

    assert dev is not None, "failed to find usbip device via pyusb"
    assert dev.idVendor == USB_VID
    assert dev.idProduct == USB_PID

    desc = array.array("B", b"\x00" * 4)
    try:
        n = dev.ctrl_transfer(0x80, 0x06, 0x0200, 0, desc)
    except usb_core.USBError as exc:
        pytest.fail(f"pyusb usbip control transfer failed: {exc}")

    assert n == 4
    assert desc[0] == 9
    assert desc[1] == 2

    # native_sim USB/IP currently serves one attached session at a time;
    # release this handle before fixture teardown / next USB/IP client.
    usb_util.dispose_resources(dev)

    circuitpython.wait_until_done()


@pytest.mark.circuitpy_drive(
    {
        "code.py": USBIP_WAIT_CODE,
        DRIVE_TEST_FILE: DRIVE_TEST_CONTENT,
    }
)
@pytest.mark.native_sim_usb
@pytest.mark.native_sim_rt
@pytest.mark.duration(30)
def test_usb_host_mass_storage_reads_circuitpy_drive(circuitpython):
    """Use Adafruit USB host mass-storage driver to read CIRCUITPY contents."""

    adafruit_usb_host_mass_storage = pytest.importorskip("adafruit_usb_host_mass_storage")

    device = _FlashBackedMassStorageDevice(circuitpython.flash_path)
    try:
        msc = adafruit_usb_host_mass_storage.USBMassStorage(device)

        block_count = msc.ioctl(4)
        assert block_count is not None and block_count > 0

        needle = DRIVE_TEST_CONTENT.encode("utf-8")
        block = bytearray(512)
        found = False

        for block_num in range(min(block_count, 4096)):
            msc.readblocks(block_num, block)
            if needle in block:
                found = True
                break

        assert found, "failed to find expected file content on CIRCUITPY drive"
    finally:
        device.close()

    circuitpython.wait_until_done()
