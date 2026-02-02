# SPDX-FileCopyrightText: 2025 Scott Shawcroft for Adafruit Industries
# SPDX-License-Identifier: MIT

"""USB/IP tests for native_sim using serial-usbipclient."""

import logging
import subprocess
import sys
import time
from pathlib import Path

import pytest

# Add the serial-usbipclient library to the path
sys.path.insert(0, str(Path(__file__).parent / "serial-usbipclient"))

from serial_usbipclient import USBIPClient, HardwareID, USBIPError

logger = logging.getLogger(__name__)

# CircuitPython USB IDs (from build output)
CP_VID = 0x1209
CP_PID = 0x000C

USBIP_PORT = 3240


def wait_for_usbip_server(host: str, port: int, timeout: float = 10.0) -> bool:
    """Wait for the USB/IP server to be ready and have devices."""
    start = time.time()
    client = None
    while time.time() - start < timeout:
        client = None
        try:
            client = USBIPClient(remote=(host, port))
            client.connect_server()
            break
        except (OSError, USBIPError, ValueError) as e:
            print(f"USB/IP connection attempt failed: {e}")
        finally:
            if client:
                try:
                    client.disconnect_server()
                except Exception:
                    pass
        time.sleep(0.2)
    if not client:
        return False

    print("connected to usbip server. waiting for devices")
    while time.time() - start < timeout:
        devices = client.list_published()
        if devices and devices.paths:
            print(f"Found {len(devices.paths)} USB/IP device(s)")
            return True
        print("USB/IP server connected but no devices yet")
    return False


def test_usbip_list_devices(native_sim_binary, create_flash_image, tmp_path):
    """Test that USB/IP server lists the CDC device."""
    server = "127.0.0.1"
    code = 'print("hello")\n'
    flash = create_flash_image({"code.py": code})
    trace_file = tmp_path / "trace.perfetto"
    cmd = [
        str(native_sim_binary),
        f"--flash={flash}",
        "--flash_rm",
        "-no-rt",
        "-stop_at=10",
        f"--trace-file={trace_file}",
    ]
    print("hello")

    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    try:
        # Wait for USB/IP server to be ready
        if not wait_for_usbip_server(server, USBIP_PORT, timeout=5.0):
            print(proc.stdout.read())
            print("test test")
            pytest.fail("USB/IP server not ready or no devices exported")

        # List devices
        client = USBIPClient(remote=(server, USBIP_PORT))
        devices = client.list_published()
        client.disconnect_server()

        assert devices is not None
        assert len(devices.paths) > 0

        # Check that we have a device with our VID/PID
        found = False
        for path in devices.paths:
            logger.info(
                f"Found device: VID=0x{path.idVendor:04x} PID=0x{path.idProduct:04x} busid={path.busid}"
            )
            if path.idVendor == CP_VID and path.idProduct == CP_PID:
                found = True
        assert found, f"CircuitPython device (VID=0x{CP_VID:04x} PID=0x{CP_PID:04x}) not found"

    finally:
        proc.terminate()
        proc.wait(timeout=1)


def test_usbip_attach_and_read(native_sim_binary, create_flash_image, tmp_path):
    """Test attaching to the CDC device and reading output."""
    server = "127.0.0.1"
    code = """\
print("usbip-test-output")
import time
time.sleep(1)
"""
    flash = create_flash_image({"code.py": code})
    trace_file = tmp_path / "trace.perfetto"
    cmd = [
        str(native_sim_binary),
        f"--flash={flash}",
        "--flash_rm",
        "-no-rt",
        "-stop_at=15",
        f"--trace-file={trace_file}",
    ]

    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    client = None
    try:
        # Wait for USB/IP server to be ready
        if not wait_for_usbip_server(server, USBIP_PORT, timeout=5.0):
            pytest.skip("USB/IP server not ready or no devices exported")

        # Create client and attach to device
        client = USBIPClient(remote=(server, USBIP_PORT))
        device_id = HardwareID(vid=CP_VID, pid=CP_PID)

        try:
            client.attach(devices=[device_id])
        except USBIPError as e:
            pytest.skip(f"Failed to attach to device: {e}")

        # Get the connection
        connections = client.get_connection(device_id)
        assert len(connections) > 0, "No connections established"
        usb = connections[0]

        # Read data from the CDC device
        output = ""
        start = time.time()
        timeout = 10.0

        while time.time() - start < timeout:
            try:
                # Queue read URBs
                client.queue_urbs(usb)

                # Try to get response data
                data = usb.response_data(size=0)
                if data:
                    text = data.decode("utf-8", errors="replace")
                    output += text
                    logger.info(f"Received: {repr(text)}")

                    if "usbip-test-output" in output:
                        break
            except TimeoutError:
                pass
            except USBIPError as e:
                logger.warning(f"USB/IP error: {e}")
                break
            time.sleep(0.01)

        assert "usbip-test-output" in output, f"Expected output not found. Got: {repr(output)}"

    finally:
        if client:
            client.shutdown()
        proc.terminate()
        proc.wait(timeout=1)


def test_usbip_repl_interaction(native_sim_binary, create_flash_image, tmp_path):
    """Test REPL interaction over USB/IP CDC."""
    server = "127.0.0.1"
    code = """\
print("ready")
"""
    flash = create_flash_image({"code.py": code})
    trace_file = tmp_path / "trace.perfetto"
    cmd = [
        str(native_sim_binary),
        f"--flash={flash}",
        "--flash_rm",
        "-no-rt",
        "-stop_at=20",
        f"--trace-file={trace_file}",
    ]

    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    client = None
    try:
        # Wait for USB/IP server to be ready
        if not wait_for_usbip_server(server, USBIP_PORT, timeout=5.0):
            pytest.skip("USB/IP server not ready or no devices exported")

        # Create client and attach to device
        client = USBIPClient(remote=(server, USBIP_PORT))
        device_id = HardwareID(vid=CP_VID, pid=CP_PID)

        try:
            client.attach(devices=[device_id])
        except USBIPError as e:
            pytest.skip(f"Failed to attach to device: {e}")

        connections = client.get_connection(device_id)
        assert len(connections) > 0
        usb = connections[0]

        # Helper to read until we see expected text
        def read_until(needle: str, timeout: float = 5.0) -> str:
            output = ""
            start = time.time()
            while time.time() - start < timeout:
                try:
                    client.queue_urbs(usb)
                    data = usb.response_data(size=0)
                    if data:
                        output += data.decode("utf-8", errors="replace")
                        if needle in output:
                            return output
                except TimeoutError:
                    pass
                time.sleep(0.01)
            return output

        # Wait for initial output
        output = read_until("ready", timeout=5.0)
        logger.info(f"Initial output: {repr(output)}")

        # Send Ctrl+C to interrupt and get to REPL
        client.send(usb, b"\x03")
        output = read_until(">>>", timeout=5.0)
        logger.info(f"After Ctrl+C: {repr(output)}")
        assert ">>>" in output, f"REPL prompt not found. Got: {repr(output)}"

        # Send a simple expression
        client.send(usb, b"print(2+2)\r\n")
        output = read_until("4", timeout=5.0)
        logger.info(f"After print: {repr(output)}")
        assert "4" in output, f"Expected '4' in output. Got: {repr(output)}"

    finally:
        if client:
            client.shutdown()
        proc.terminate()
        proc.wait(timeout=1)
