# SPDX-FileCopyrightText: 2026 Dan Halbert for Adafruit Industries LLC
# SPDX-License-Identifier: MIT

"""Test touchio functionality on native_sim.

touchio charges the pin, switches it to an input, and counts until it reads
back discharged. gpio_emul has no RC model: an input reads whatever value was
last set, so the count is either 0 or the 10000-tick timeout. A pin held at its
charge level by an input trace stands in for a touch.
"""

import re

import pytest


TOUCH_CODE = """\
import time
import microcontroller
import touchio

touch = touchio.TouchIn(microcontroller.pin.P_01)
print(f"t_abs={time.monotonic():.3f} threshold={touch.threshold}")


def report():
    print(f"t_abs={time.monotonic():.3f} raw={touch.raw_value} value={touch.value}")


report()
# Read only between trace edges, so each edge lands while the pin is an input.
for until in (4.5, 5.5):
    while time.monotonic() < until:
        time.sleep(0.05)
    report()

print("done")
"""

TOUCH_TRACE = {
    "gpio_emul.01": [
        (4_000_000_000, 1),  # 4.0s: touched
        (5_000_000_000, 0),  # 5.0s: released
    ],
}

REPORT_RE = re.compile(r"t_abs=([0-9.]+) raw=(\d+) value=(True|False)")


@pytest.mark.circuitpy_drive({"code.py": TOUCH_CODE})
@pytest.mark.input_trace(TOUCH_TRACE)
def test_touchio_touch_and_release(circuitpython):
    """TouchIn reads touched while a trace holds the pin high."""
    circuitpython.wait_until_done()

    output = circuitpython.serial.all_output
    threshold_match = re.search(r"t_abs=([0-9.]+) threshold=(\d+)", output)
    assert threshold_match is not None
    # The trace only reaches pins that are already inputs.
    assert float(threshold_match.group(1)) < 4.0
    # Untouched raw reading of 0 gives a threshold of 0 * 1.05 + 100.
    assert int(threshold_match.group(2)) == 100

    reports = [(int(raw), value == "True") for _, raw, value in REPORT_RE.findall(output)]
    assert reports == [(0, False), (10000, True), (0, False)]
    assert "done" in output


NO_PULL_CODE = """\
import digitalio
import microcontroller
import touchio

# Nothing pulls P_01 up, so it never charges low.
try:
    touchio.TouchIn(microcontroller.pin.P_01, pull=digitalio.Pull.UP)
except ValueError as e:
    print(f"pull up: {e}")

# Leave P_02 high, as if nothing pulled it down.
with digitalio.DigitalInOut(microcontroller.pin.P_02) as pin:
    pin.switch_to_input(pull=digitalio.Pull.UP)
try:
    touchio.TouchIn(microcontroller.pin.P_02)
except ValueError as e:
    print(f"pull down: {e}")

print("done")
"""


@pytest.mark.circuitpy_drive({"code.py": NO_PULL_CODE})
def test_touchio_missing_pull_resistor(circuitpython):
    """TouchIn raises ValueError when the pin never discharges."""
    circuitpython.wait_until_done()

    output = circuitpython.serial.all_output
    assert "pull up: No pullup on pin; 1Mohm recommended" in output
    assert "pull down: No pulldown on pin; 1Mohm recommended" in output
    assert "done" in output
