# SPDX-FileCopyrightText: 2025 Scott Shawcroft for Adafruit Industries
# SPDX-License-Identifier: MIT

"""Test the asyncexample native async module."""

from pathlib import Path

import pytest

# Load the asyncio Python package from the frozen directory so it can be
# placed on the virtual flash alongside code.py.
_FROZEN = Path(__file__).parent.parent.parent.parent / "frozen"
_ASYNCIO_DIR = _FROZEN / "Adafruit_CircuitPython_asyncio/asyncio"
_TICKS_FILE = _FROZEN / "Adafruit_CircuitPython_Ticks/adafruit_ticks.py"


def _asyncio_files():
    """Return a dict of {path: content} for the asyncio package + dependencies."""
    files = {}
    for f in sorted(_ASYNCIO_DIR.glob("*.py")):
        files[f"lib/asyncio/{f.name}"] = f.read_text()
    files["lib/adafruit_ticks.py"] = _TICKS_FILE.read_text()
    return files


def _drive(code):
    """Build a circuitpy_drive dict with code.py + asyncio package."""
    return {**_asyncio_files(), "code.py": code}


# --- Tests using asyncio.run() ---


ASYNCIO_BASIC_CODE = """\
import asyncio
import asyncexample

async def main():
    result = await asyncexample.delay(0)
    print(f"result={result}")
    print("done")

asyncio.run(main())
"""


@pytest.mark.circuitpy_drive(_drive(ASYNCIO_BASIC_CODE))
@pytest.mark.duration(30)
def test_asyncio_basic(circuitpython):
    """Test delay(0) with asyncio.run()."""
    circuitpython.wait_until_done()
    output = circuitpython.serial.all_output
    assert "result=0" in output
    assert "done" in output


ASYNCIO_GATHER_CODE = """\
import asyncio
import asyncexample

async def worker(name):
    result = await asyncexample.delay(0)
    print(f"{name}={result}")

async def main():
    await asyncio.gather(worker("a"), worker("b"))
    print("done")

asyncio.run(main())
"""


@pytest.mark.circuitpy_drive(_drive(ASYNCIO_GATHER_CODE))
@pytest.mark.duration(30)
def test_asyncio_gather(circuitpython):
    """Test concurrent tasks with asyncio.gather()."""
    circuitpython.wait_until_done()
    output = circuitpython.serial.all_output
    assert "a=0" in output
    assert "b=0" in output
    assert "done" in output


# --- Tests using manual coroutine driver (for timer-based delays) ---


TIMER_CODE = """\
import asyncexample
import time

async def main():
    result = await asyncexample.delay(10)
    print(f"result={result}")
    print("done")

coro = main()
val = None
while True:
    try:
        val = coro.send(val)
        time.sleep(0.005)
    except StopIteration:
        break
"""


@pytest.mark.circuitpy_drive({"code.py": TIMER_CODE})
@pytest.mark.native_sim_rt
def test_timer_delay(circuitpython):
    """Test delay(10ms) with a manual coroutine driver and Zephyr timer."""
    circuitpython.wait_until_done()
    output = circuitpython.serial.all_output
    assert "result=10" in output
    assert "done" in output


# --- Protocol tests ---


DIRECT_PROTOCOL_CODE = """\
import asyncexample

awaitable = asyncexample.delay(0)
print(f"type={type(awaitable).__name__}")

it = awaitable.__await__()
print(f"is_self={it is awaitable}")

try:
    awaitable.send(None)
    print("ERROR: should have raised StopIteration")
except StopIteration as e:
    print(f"value={e.value}")

print("done")
"""


@pytest.mark.circuitpy_drive({"code.py": DIRECT_PROTOCOL_CODE})
def test_direct_protocol(circuitpython):
    """Test the awaitable protocol directly."""
    circuitpython.wait_until_done()
    output = circuitpython.serial.all_output
    assert "type=Awaitable" in output
    assert "is_self=True" in output
    assert "value=0" in output
    assert "done" in output


NEVER_AWAITED_CODE = """\
import gc
import asyncexample

asyncexample.delay(0)
gc.collect()
print("done")
"""


@pytest.mark.circuitpy_drive({"code.py": NEVER_AWAITED_CODE})
def test_never_awaited_warning(circuitpython):
    """Test that GC'ing an un-awaited awaitable prints a warning."""
    circuitpython.wait_until_done()
    output = circuitpython.serial.all_output
    debug_output = circuitpython.debug_serial.all_output
    assert "awaitable was never awaited" in output or "awaitable was never awaited" in debug_output
    assert "done" in output
