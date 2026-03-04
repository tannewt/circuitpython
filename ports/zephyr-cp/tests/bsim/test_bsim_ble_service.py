# SPDX-FileCopyrightText: 2026 Scott Shawcroft for Adafruit Industries
# SPDX-License-Identifier: MIT

"""BLE GATT service tests for nrf5340bsim."""

import pytest

pytestmark = pytest.mark.circuitpython_board("native_nrf5340bsim")

BSIM_SERVICE_CODE = """\
import _bleio
import time

adapter = _bleio.adapter

# Create Battery Service (UUID 0x180F) with Battery Level characteristic (UUID 0x2A19)
bas_uuid = _bleio.UUID(0x180F)
bat_level_uuid = _bleio.UUID(0x2A19)

svc = _bleio.Service(bas_uuid)
char = _bleio.Characteristic.add_to_service(
    svc,
    bat_level_uuid,
    properties=_bleio.Characteristic.READ | _bleio.Characteristic.NOTIFY,
    read_perm=_bleio.Attribute.OPEN,
    write_perm=_bleio.Attribute.NO_ACCESS,
    max_length=1,
    fixed_length=True,
    initial_value=bytes([75]),
)
print("service created")

name = b"CPSVC"
advertisement = bytes((2, 0x01, 0x06, len(name) + 1, 0x09)) + name
adapter.start_advertising(advertisement, connectable=True)
print("advertising")

for _ in range(80):
    if adapter.connected:
        break
    time.sleep(0.1)
print("connected", adapter.connected)

for _ in range(80):
    if not adapter.connected:
        break
    time.sleep(0.1)
print("done")
"""


@pytest.mark.zephyr_sample("tests/bsim/samples/central_battery_client")
@pytest.mark.duration(14)
@pytest.mark.circuitpy_drive({"code.py": BSIM_SERVICE_CODE})
def test_bsim_service_battery(bsim_phy, circuitpython, zephyr_sample):
    """CP hosts BatteryService; Zephyr central reads battery level."""
    circuitpython.wait_until_done()

    cp_output = circuitpython.serial.all_output
    sample_output = zephyr_sample.serial.all_output

    assert "service created" in cp_output
    assert "connected True" in cp_output
    assert "Battery Level: 75" in sample_output


BSIM_SERVER_CODE = """\
import _bleio
import time

adapter = _bleio.adapter

# Create Battery Service (UUID 0x180F) with Battery Level characteristic (UUID 0x2A19)
bas_uuid = _bleio.UUID(0x180F)
bat_level_uuid = _bleio.UUID(0x2A19)

svc = _bleio.Service(bas_uuid)
char = _bleio.Characteristic.add_to_service(
    svc,
    bat_level_uuid,
    properties=_bleio.Characteristic.READ | _bleio.Characteristic.WRITE,
    read_perm=_bleio.Attribute.OPEN,
    write_perm=_bleio.Attribute.OPEN,
    max_length=1,
    fixed_length=True,
    initial_value=bytes([75]),
)
print("service created")

name = b"CPBAT"
advertisement = bytes((2, 0x01, 0x06, len(name) + 1, 0x09)) + name
adapter.start_advertising(advertisement, connectable=True)
print("advertising")

for _ in range(80):
    if adapter.connected:
        break
    time.sleep(0.1)
print("connected", adapter.connected)

for _ in range(80):
    if not adapter.connected:
        break
    time.sleep(0.1)

print("final value", list(char.value))
print("done")
"""

BSIM_CLIENT_CODE = """\
import _bleio
import time

adapter = _bleio.adapter

print("client start")
target = None
for entry in adapter.start_scan(timeout=6.0, active=True):
    if entry.connectable and b"CPBAT" in entry.advertisement_bytes:
        target = entry.address
        print("found server")
        break
adapter.stop_scan()
print("have target", target is not None)

if target is None:
    raise RuntimeError("No server found")

connection = adapter.connect(target, timeout=5.0)
print("connected", connection.connected)

services = connection.discover_remote_services([_bleio.UUID(0x180F)])
print("discovered services", len(services))

bat_svc = services[0]
chars = bat_svc.characteristics
print("discovered chars", len(chars))

bat_char = chars[0]
value = bat_char.value
print("battery level", list(value))

# Write a new value
bat_char.value = bytes([42])
print("wrote new value")

time.sleep(0.5)

connection.disconnect()

timeout = time.monotonic() + 4.0
while connection.connected and time.monotonic() < timeout:
    time.sleep(0.1)

print("disconnected", not connection.connected)
"""


@pytest.mark.duration(14)
@pytest.mark.circuitpy_drive({"code.py": BSIM_SERVER_CODE})
@pytest.mark.circuitpy_drive({"code.py": BSIM_CLIENT_CODE})
def test_bsim_service_cp_client(bsim_phy, circuitpython1, circuitpython2):
    """CP peripheral hosts BatteryService; CP central discovers, reads, and writes."""
    server = circuitpython1
    client = circuitpython2

    client.wait_until_done()
    server.wait_until_done()

    server_output = server.serial.all_output
    client_output = client.serial.all_output

    assert "service created" in server_output
    assert "connected True" in server_output
    assert "final value [42]" in server_output

    assert "client start" in client_output
    assert "found server" in client_output
    assert "discovered services 1" in client_output
    assert "discovered chars 1" in client_output
    assert "battery level [75]" in client_output
    assert "wrote new value" in client_output
    assert "disconnected True" in client_output


# --- Discover all services (no whitelist) ---

BSIM_DISCOVER_ALL_SERVER_CODE = """\
import _bleio
import time

adapter = _bleio.adapter

# Create two services: Battery (0x180F) and Heart Rate (0x180D)
bas = _bleio.Service(_bleio.UUID(0x180F))
_bleio.Characteristic.add_to_service(
    bas, _bleio.UUID(0x2A19),
    properties=_bleio.Characteristic.READ,
    read_perm=_bleio.Attribute.OPEN,
    write_perm=_bleio.Attribute.NO_ACCESS,
    max_length=1, fixed_length=True, initial_value=bytes([80]),
)

hrs = _bleio.Service(_bleio.UUID(0x180D))
_bleio.Characteristic.add_to_service(
    hrs, _bleio.UUID(0x2A37),
    properties=_bleio.Characteristic.READ,
    read_perm=_bleio.Attribute.OPEN,
    write_perm=_bleio.Attribute.NO_ACCESS,
    max_length=2, fixed_length=True, initial_value=bytes([0, 72]),
)
print("services created")

name = b"CPALL"
advertisement = bytes((2, 0x01, 0x06, len(name) + 1, 0x09)) + name
adapter.start_advertising(advertisement, connectable=True)

for _ in range(80):
    if adapter.connected:
        break
    time.sleep(0.1)
print("connected", adapter.connected)

for _ in range(80):
    if not adapter.connected:
        break
    time.sleep(0.1)
print("done")
"""

BSIM_DISCOVER_ALL_CLIENT_CODE = """\
import _bleio
import time

adapter = _bleio.adapter

print("client start")
target = None
for entry in adapter.start_scan(timeout=8.0, active=True):
    if entry.connectable and b"CPALL" in entry.advertisement_bytes:
        target = entry.address
        print("found server")
        break
adapter.stop_scan()
print("have target", target is not None)

if target is None:
    raise RuntimeError("No server found")

connection = adapter.connect(target, timeout=5.0)
print("connected", connection.connected)

# Discover ALL services (no whitelist)
services = connection.discover_remote_services()
print("total services", len(services))

# Filter to our two known UUIDs (ignore GATT/GAP services the stack may expose)
user_svcs = [s for s in services if s.uuid.uuid16 in (0x180F, 0x180D)]
print("user services", len(user_svcs))

uuids = sorted([s.uuid.uuid16 for s in user_svcs])
print("service uuids", uuids)

for svc in user_svcs:
    for ch in svc.characteristics:
        val = list(ch.value)
        print("char", hex(ch.uuid.uuid16), val)

connection.disconnect()

timeout = time.monotonic() + 4.0
while connection.connected and time.monotonic() < timeout:
    time.sleep(0.1)
print("done")
"""


@pytest.mark.duration(14)
@pytest.mark.circuitpy_drive({"code.py": BSIM_DISCOVER_ALL_SERVER_CODE})
@pytest.mark.circuitpy_drive({"code.py": BSIM_DISCOVER_ALL_CLIENT_CODE})
def test_bsim_service_discover_all(bsim_phy, circuitpython1, circuitpython2):
    """Discover all services without a UUID whitelist, verify two user services found."""
    server = circuitpython1
    client = circuitpython2

    client.wait_until_done()
    server.wait_until_done()

    client_output = client.serial.all_output

    assert "user services 2" in client_output
    assert "service uuids [6157, 6159]" in client_output  # 0x180D=6157, 0x180F=6159
    assert "char 0x2a37 [0, 72]" in client_output
    assert "char 0x2a19 [80]" in client_output


# --- Write-no-response ---

BSIM_WRITE_NR_SERVER_CODE = """\
import _bleio
import time

adapter = _bleio.adapter

svc = _bleio.Service(_bleio.UUID(0x180F))
char = _bleio.Characteristic.add_to_service(
    svc, _bleio.UUID(0x2A19),
    properties=_bleio.Characteristic.READ | _bleio.Characteristic.WRITE_NO_RESPONSE,
    read_perm=_bleio.Attribute.OPEN,
    write_perm=_bleio.Attribute.OPEN,
    max_length=1, fixed_length=True, initial_value=bytes([50]),
)
print("service created")

name = b"CPWNR"
advertisement = bytes((2, 0x01, 0x06, len(name) + 1, 0x09)) + name
adapter.start_advertising(advertisement, connectable=True)

for _ in range(80):
    if adapter.connected:
        break
    time.sleep(0.1)
print("connected", adapter.connected)

for _ in range(80):
    if not adapter.connected:
        break
    time.sleep(0.1)

print("final value", list(char.value))
print("done")
"""

BSIM_WRITE_NR_CLIENT_CODE = """\
import _bleio
import time

adapter = _bleio.adapter

target = None
for entry in adapter.start_scan(timeout=6.0, active=True):
    if entry.connectable and b"CPWNR" in entry.advertisement_bytes:
        target = entry.address
        break
adapter.stop_scan()

if target is None:
    raise RuntimeError("No server found")

connection = adapter.connect(target, timeout=5.0)

services = connection.discover_remote_services([_bleio.UUID(0x180F)])
char = services[0].characteristics[0]

print("initial", list(char.value))

# Write-no-response
char.value = bytes([99])
print("wrote wnr")

# Give the server time to process the write
time.sleep(0.5)

connection.disconnect()

timeout = time.monotonic() + 4.0
while connection.connected and time.monotonic() < timeout:
    time.sleep(0.1)
print("done")
"""


@pytest.mark.duration(14)
@pytest.mark.circuitpy_drive({"code.py": BSIM_WRITE_NR_SERVER_CODE})
@pytest.mark.circuitpy_drive({"code.py": BSIM_WRITE_NR_CLIENT_CODE})
def test_bsim_service_write_no_response(bsim_phy, circuitpython1, circuitpython2):
    """Client writes a characteristic using WRITE_NO_RESPONSE."""
    server = circuitpython1
    client = circuitpython2

    client.wait_until_done()
    server.wait_until_done()

    server_output = server.serial.all_output
    client_output = client.serial.all_output

    assert "final value [99]" in server_output
    assert "initial [50]" in client_output
    assert "wrote wnr" in client_output


# --- Multiple characteristics on one service ---

BSIM_MULTI_CHAR_SERVER_CODE = """\
import _bleio
import time

adapter = _bleio.adapter

svc = _bleio.Service(_bleio.UUID(0x180F))
char_a = _bleio.Characteristic.add_to_service(
    svc, _bleio.UUID(0x2A19),
    properties=_bleio.Characteristic.READ,
    read_perm=_bleio.Attribute.OPEN,
    write_perm=_bleio.Attribute.NO_ACCESS,
    max_length=1, fixed_length=True, initial_value=bytes([10]),
)
char_b = _bleio.Characteristic.add_to_service(
    svc, _bleio.UUID(0x2A1A),
    properties=_bleio.Characteristic.READ | _bleio.Characteristic.WRITE,
    read_perm=_bleio.Attribute.OPEN,
    write_perm=_bleio.Attribute.OPEN,
    max_length=1, fixed_length=True, initial_value=bytes([20]),
)
char_c = _bleio.Characteristic.add_to_service(
    svc, _bleio.UUID(0x2A1B),
    properties=_bleio.Characteristic.READ,
    read_perm=_bleio.Attribute.OPEN,
    write_perm=_bleio.Attribute.NO_ACCESS,
    max_length=1, fixed_length=True, initial_value=bytes([30]),
)
print("service created")

name = b"CPMCH"
advertisement = bytes((2, 0x01, 0x06, len(name) + 1, 0x09)) + name
adapter.start_advertising(advertisement, connectable=True)

for _ in range(80):
    if adapter.connected:
        break
    time.sleep(0.1)
print("connected", adapter.connected)

for _ in range(80):
    if not adapter.connected:
        break
    time.sleep(0.1)

print("char_b final", list(char_b.value))
print("done")
"""

BSIM_MULTI_CHAR_CLIENT_CODE = """\
import _bleio
import time

adapter = _bleio.adapter

target = None
for entry in adapter.start_scan(timeout=6.0, active=True):
    if entry.connectable and b"CPMCH" in entry.advertisement_bytes:
        target = entry.address
        break
adapter.stop_scan()

if target is None:
    raise RuntimeError("No server found")

connection = adapter.connect(target, timeout=5.0)

services = connection.discover_remote_services([_bleio.UUID(0x180F)])
chars = services[0].characteristics
print("num chars", len(chars))

# Read each characteristic and print uuid + value
for ch in chars:
    print("char", hex(ch.uuid.uuid16), list(ch.value))

# Write to the second characteristic (0x2A1A)
for ch in chars:
    if ch.uuid.uuid16 == 0x2A1A:
        ch.value = bytes([77])
        print("wrote 0x2a1a")

time.sleep(0.5)

connection.disconnect()

timeout = time.monotonic() + 4.0
while connection.connected and time.monotonic() < timeout:
    time.sleep(0.1)
print("done")
"""


@pytest.mark.duration(14)
@pytest.mark.circuitpy_drive({"code.py": BSIM_MULTI_CHAR_SERVER_CODE})
@pytest.mark.circuitpy_drive({"code.py": BSIM_MULTI_CHAR_CLIENT_CODE})
def test_bsim_service_multi_char(bsim_phy, circuitpython1, circuitpython2):
    """Service with three characteristics: discover all, read each, write one."""
    server = circuitpython1
    client = circuitpython2

    client.wait_until_done()
    server.wait_until_done()

    server_output = server.serial.all_output
    client_output = client.serial.all_output

    assert "num chars 3" in client_output
    assert "char 0x2a19 [10]" in client_output
    assert "char 0x2a1a [20]" in client_output
    assert "char 0x2a1b [30]" in client_output
    assert "wrote 0x2a1a" in client_output
    assert "char_b final [77]" in server_output


# --- 128-bit custom UUID ---

BSIM_CUSTOM_UUID_SERVER_CODE = """\
import _bleio
import time

adapter = _bleio.adapter

# Custom 128-bit UUID service and characteristic
svc_uuid = _bleio.UUID("12345678-1234-5678-1234-56789abcdef0")
char_uuid = _bleio.UUID("12345678-1234-5678-1234-56789abcdef1")

svc = _bleio.Service(svc_uuid)
char = _bleio.Characteristic.add_to_service(
    svc, char_uuid,
    properties=_bleio.Characteristic.READ | _bleio.Characteristic.WRITE,
    read_perm=_bleio.Attribute.OPEN,
    write_perm=_bleio.Attribute.OPEN,
    max_length=4, fixed_length=False, initial_value=bytes([0xDE, 0xAD]),
)
print("service created")

name = b"CPUUI"
advertisement = bytes((2, 0x01, 0x06, len(name) + 1, 0x09)) + name
adapter.start_advertising(advertisement, connectable=True)

for _ in range(80):
    if adapter.connected:
        break
    time.sleep(0.1)
print("connected", adapter.connected)

for _ in range(80):
    if not adapter.connected:
        break
    time.sleep(0.1)

print("final value", list(char.value))
print("done")
"""

BSIM_CUSTOM_UUID_CLIENT_CODE = """\
import _bleio
import time

adapter = _bleio.adapter

target = None
for entry in adapter.start_scan(timeout=6.0, active=True):
    if entry.connectable and b"CPUUI" in entry.advertisement_bytes:
        target = entry.address
        break
adapter.stop_scan()

if target is None:
    raise RuntimeError("No server found")

connection = adapter.connect(target, timeout=5.0)

svc_uuid = _bleio.UUID("12345678-1234-5678-1234-56789abcdef0")
services = connection.discover_remote_services([svc_uuid])
print("discovered services", len(services))

char = services[0].characteristics[0]
print("char uuid", str(char.uuid))
print("char value", list(char.value))

char.value = bytes([0xBE, 0xEF])
print("wrote custom")

time.sleep(0.5)

connection.disconnect()

timeout = time.monotonic() + 4.0
while connection.connected and time.monotonic() < timeout:
    time.sleep(0.1)
print("done")
"""


@pytest.mark.duration(14)
@pytest.mark.circuitpy_drive({"code.py": BSIM_CUSTOM_UUID_SERVER_CODE})
@pytest.mark.circuitpy_drive({"code.py": BSIM_CUSTOM_UUID_CLIENT_CODE})
def test_bsim_service_custom_uuid(bsim_phy, circuitpython1, circuitpython2):
    """128-bit custom UUID service: discover, read, and write."""
    server = circuitpython1
    client = circuitpython2

    client.wait_until_done()
    server.wait_until_done()

    server_output = server.serial.all_output
    client_output = client.serial.all_output

    assert "discovered services 1" in client_output
    assert "char value [222, 173]" in client_output  # 0xDE, 0xAD
    assert "wrote custom" in client_output
    assert "final value [190, 239]" in server_output  # 0xBE, 0xEF


# --- Empty discovery result ---

BSIM_EMPTY_DISC_SERVER_CODE = """\
import _bleio
import time

adapter = _bleio.adapter

# Only create Battery Service
svc = _bleio.Service(_bleio.UUID(0x180F))
_bleio.Characteristic.add_to_service(
    svc, _bleio.UUID(0x2A19),
    properties=_bleio.Characteristic.READ,
    read_perm=_bleio.Attribute.OPEN,
    write_perm=_bleio.Attribute.NO_ACCESS,
    max_length=1, fixed_length=True, initial_value=bytes([1]),
)

name = b"CPEMT"
advertisement = bytes((2, 0x01, 0x06, len(name) + 1, 0x09)) + name
adapter.start_advertising(advertisement, connectable=True)

for _ in range(80):
    if adapter.connected:
        break
    time.sleep(0.1)

for _ in range(80):
    if not adapter.connected:
        break
    time.sleep(0.1)
print("done")
"""

BSIM_EMPTY_DISC_CLIENT_CODE = """\
import _bleio
import time

adapter = _bleio.adapter

target = None
for entry in adapter.start_scan(timeout=6.0, active=True):
    if entry.connectable and b"CPEMT" in entry.advertisement_bytes:
        target = entry.address
        break
adapter.stop_scan()

if target is None:
    raise RuntimeError("No server found")

connection = adapter.connect(target, timeout=5.0)

# Ask for Heart Rate Service which doesn't exist on this server
services = connection.discover_remote_services([_bleio.UUID(0x180D)])
print("found services", len(services))

connection.disconnect()

timeout = time.monotonic() + 4.0
while connection.connected and time.monotonic() < timeout:
    time.sleep(0.1)
print("done")
"""


@pytest.mark.duration(14)
@pytest.mark.circuitpy_drive({"code.py": BSIM_EMPTY_DISC_SERVER_CODE})
@pytest.mark.circuitpy_drive({"code.py": BSIM_EMPTY_DISC_CLIENT_CODE})
def test_bsim_service_empty_discovery(bsim_phy, circuitpython1, circuitpython2):
    """Filter for a UUID that doesn't exist, verify empty tuple returned."""
    server = circuitpython1
    client = circuitpython2

    client.wait_until_done()
    server.wait_until_done()

    client_output = client.serial.all_output

    assert "found services 0" in client_output
    assert "done" in client_output
