# SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
# SPDX-License-Identifier: MIT

"""busio.UART on native_sim, through board.UART() on the second PTY UART."""

import pytest

TIMEOUT_CODE = """\
import board

uart = board.UART()
for t in (0.5, 2, 0):
    uart.timeout = t
    print("timeout", uart.timeout)
print("done")
"""


@pytest.mark.circuitpy_drive({"code.py": TIMEOUT_CODE})
def test_uart_timeout_reads_back_in_seconds(circuitpython):
    """timeout reads back the seconds it was set to, not raw kernel ticks."""
    circuitpython.wait_until_done()

    output = circuitpython.serial.all_output
    assert "timeout 0.5" in output
    assert "timeout 2.0" in output
    assert "timeout 0.0" in output
    assert "done" in output
