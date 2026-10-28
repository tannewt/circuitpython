# SPDX-FileCopyrightText: 2026 Dan Halbert for Adafruit Industries LLC
# SPDX-License-Identifier: MIT

"""Test bitbangio functionality on native_sim.

I2C is only tested with no device on the bus. gpio_emul has no open-drain
mode, so digitalio emulates it by reconfiguring the pin, which the GPIO trace
does not record. gpio_emul also forces a pulled-up input high on every
reconfigure, so a test can't inject an ACK or clock stretching.
"""

from pathlib import Path

import pytest
from perfetto.trace_processor import TraceProcessor


def parse_gpio_trace(trace_file: Path, pin_name: str) -> list[tuple[int, int]]:
    """Return (timestamp_ns, value) for every change of a GPIO trace track."""
    tp = TraceProcessor(file_path=str(trace_file))
    result = tp.query(
        f'''
        SELECT c.ts, c.value
        FROM counter c
        JOIN track t ON c.track_id = t.id
        WHERE t.name = "{pin_name}"
        ORDER BY c.ts
        '''
    )
    return [(row.ts, int(row.value)) for row in result]


def value_at(trace: list[tuple[int, int]], ts: int) -> int:
    """Return the traced value in effect at ts."""
    value = 0
    for event_ts, event_value in trace:
        if event_ts > ts:
            break
        value = event_value
    return value


SPI_WRITE_CODE = """\
import bitbangio
import microcontroller

spi = bitbangio.SPI(
    microcontroller.pin.P_01, MOSI=microcontroller.pin.P_02, MISO=microcontroller.pin.P_03
)
while not spi.try_lock():
    pass
spi.configure(baudrate=100_000, polarity=0, phase=0)
spi.write(b"\\xa5\\x3c")
spi.unlock()
spi.deinit()
print("done")
"""


@pytest.mark.circuitpy_drive({"code.py": SPI_WRITE_CODE})
def test_bitbangio_spi_write(circuitpython):
    """SPI.write() clocks out the bytes MSB first at the configured baudrate."""
    circuitpython.wait_until_done()
    assert "done" in circuitpython.serial.all_output

    clock = parse_gpio_trace(circuitpython.trace_file, "gpio_emul.01")
    mosi = parse_gpio_trace(circuitpython.trace_file, "gpio_emul.02")

    # (rise, fall) timestamps of each clock pulse, skipping the initial value at ts=0.
    pulses = []
    level = 0
    rise = None
    for ts, value in clock:
        if ts == 0 or value == level:
            continue
        level = value
        if value:
            rise = ts
        elif rise is not None:
            pulses.append((rise, ts))
    assert len(pulses) == 16, f"Expected 16 clock pulses, got {len(pulses)}"

    # Mode 0: MOSI is sampled on the rising edge.
    bits = [value_at(mosi, rise) for rise, _ in pulses]
    data = bytes(int("".join(str(b) for b in bits[i : i + 8]), 2) for i in (0, 8))
    assert data == b"\xa5\x3c"

    # 100 kHz is a 5 us half period. Without a working common_hal_mcu_delay_us(),
    # simulated time would not advance and every edge would share a timestamp.
    for rise, fall in pulses:
        assert 4_000 <= fall - rise <= 6_000, f"high time {fall - rise} ns"
    for (rise1, _), (rise2, _) in zip(pulses[:8], pulses[1:8]):
        assert 9_000 <= rise2 - rise1 <= 11_000, f"period {rise2 - rise1} ns"


SPI_READ_CODE = """\
import time
import bitbangio
import microcontroller

spi = bitbangio.SPI(
    microcontroller.pin.P_01, MOSI=microcontroller.pin.P_02, MISO=microcontroller.pin.P_03
)
while not spi.try_lock():
    pass
spi.configure(baudrate=100_000)
buf = bytearray(2)
spi.readinto(buf)
print("before", buf.hex())
# The input trace drives MISO high at 3.0 s.
time.sleep(4.0)
spi.readinto(buf)
print("after", buf.hex())
spi.unlock()
spi.deinit()
print("done")
"""


@pytest.mark.circuitpy_drive({"code.py": SPI_READ_CODE})
@pytest.mark.input_trace({"gpio_emul.03": [(3_000_000_000, 1)]})
def test_bitbangio_spi_readinto(circuitpython):
    """SPI.readinto() samples the MISO pin."""
    circuitpython.wait_until_done()

    output = circuitpython.serial.all_output
    assert "before 0000" in output
    assert "after ffff" in output
    assert "done" in output


I2C_CODE = """\
import bitbangio
import microcontroller

i2c = bitbangio.I2C(microcontroller.pin.P_04, microcontroller.pin.P_05, frequency=100_000)
while not i2c.try_lock():
    pass
print("scan", i2c.scan())
try:
    i2c.writeto(0x26, b"\\x01")
    print("writeto succeeded")
except OSError as e:
    print("writeto OSError", e.errno)
i2c.unlock()
i2c.deinit()
print("done")
"""


@pytest.mark.circuitpy_drive({"code.py": I2C_CODE})
def test_bitbangio_i2c_no_device(circuitpython):
    """With nothing on the bus, every address NACKs."""
    circuitpython.wait_until_done()

    output = circuitpython.serial.all_output
    assert "scan []" in output
    # MP_ENODEV
    assert "writeto OSError 19" in output
    assert "done" in output


DEINIT_THEN_GC_CODE = """\
import gc
import bitbangio
import digitalio
import microcontroller

p = microcontroller.pin
spi = bitbangio.SPI(p.P_01, MOSI=p.P_02, MISO=p.P_03)
spi.deinit()
# The I2C pin objects can reuse the blocks the SPI freed.
i2c = bitbangio.I2C(p.P_04, p.P_05)
spi = None
# Runs the SPI's finaliser, which calls deinit() again.
gc.collect()
try:
    digitalio.DigitalInOut(p.P_04)
    print("P_04 released")
except ValueError:
    print("P_04 still claimed")
i2c.try_lock()
print("scan", i2c.scan())
i2c.unlock()
i2c.deinit()
i2c.deinit()
print("done")
"""


@pytest.mark.circuitpy_drive({"code.py": DEINIT_THEN_GC_CODE})
def test_bitbangio_finaliser_after_deinit(circuitpython):
    """A finaliser running after deinit() leaves other objects' pins alone."""
    circuitpython.wait_until_done()

    output = circuitpython.serial.all_output
    assert "P_04 still claimed" in output
    assert "scan []" in output
    assert "done" in output
