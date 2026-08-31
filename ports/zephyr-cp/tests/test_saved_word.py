# SPDX-FileCopyrightText: 2025 Scott Shawcroft for Adafruit Industries
# SPDX-License-Identifier: MIT

"""Test that the safe-mode saved word survives a hard reboot on native_sim/bsim.

native_sim and the bsim boards reboot by re-executing the process (execv),
which wipes RAM (including .noinit). supervisor/port.c works around that by
persisting retained memory to a file across the reboot (the --retained-memory
flag); currently that holds the safe-mode saved word.

The saved word drives safe-mode detection: ``microcontroller.on_next_reset(
RunMode.SAFE_MODE)`` arms the sentinel in the saved word, and a subsequent
``microcontroller.reset()`` reboots. If the word persisted, the next boot reads
the sentinel and enters safe mode (printed as "Running in safe mode!").
"""

import pytest


SAFE_MODE_RESET_CODE = """\
import microcontroller
microcontroller.on_next_reset(microcontroller.RunMode.SAFE_MODE)
print("resetting")
microcontroller.reset()
"""


@pytest.mark.circuitpy_drive({"code.py": SAFE_MODE_RESET_CODE})
@pytest.mark.duration(30)
def test_saved_word_survives_reboot_into_safe_mode(circuitpython):
    """The saved word persists across a hard reboot and triggers safe mode."""
    circuitpython.serial.wait_for("resetting", timeout=20)

    # microcontroller.reset() re-execs the process; the UART PTY is O_CLOEXEC so
    # a new one is opened after reboot. Reconnect to it.
    assert circuitpython.reconnect_serial(timeout=20), "simulator did not reboot"

    # The next boot restores the saved word (the SAFE_MODE sentinel) and enters
    # safe mode instead of running code.py.
    circuitpython.serial.wait_for("Running in safe mode", timeout=20)

    assert "Running in safe mode" in circuitpython.serial.all_output
