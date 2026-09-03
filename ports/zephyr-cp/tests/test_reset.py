# SPDX-FileCopyrightText: 2025 Scott Shawcroft for Adafruit Industries
# SPDX-License-Identifier: MIT

"""Test that a plain microcontroller.reset() reboots back into CircuitPython.

This is like the safe-mode test (test_saved_word.py) but without changing the
run mode: no sentinel is armed, so the reboot should be a normal boot that
runs code.py again instead of entering safe mode.
"""

import pytest


RESET_CODE = """\
import microcontroller
print("resetting")
microcontroller.reset()
"""


@pytest.mark.circuitpy_drive({"code.py": RESET_CODE})
@pytest.mark.duration(30)
def test_plain_reset_reboots_into_circuitpython(circuitpython):
    """microcontroller.reset() without a run mode set reboots into code.py."""
    circuitpython.serial.wait_for("resetting", timeout=30)

    # microcontroller.reset() re-execs the process; the UART PTY is O_CLOEXEC
    # so a new one is opened after reboot. Reconnect to it.
    assert circuitpython.reconnect_serial(timeout=30), "simulator did not reboot"

    # The next boot is a normal boot: code.py runs again and prints the same
    # marker. (The second reset exhausts the --vm-runs budget and exits.)
    circuitpython.serial.wait_for("resetting", timeout=30)

    all_output = circuitpython.serial.all_output
    assert "resetting" in all_output
    # It must not have entered safe mode.
    assert "Running in safe mode" not in all_output
