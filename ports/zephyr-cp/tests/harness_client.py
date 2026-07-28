# SPDX-FileCopyrightText: 2026 Scott Shawcroft for Adafruit Industries
# SPDX-License-Identifier: MIT

"""SCPI client for the esp-harness over a serial port.

Communicates with the virtual harness device exposed by esp-usbip-bridge
(VID:PID 0xCAFE:0x4002) using the SCPI-1999 protocol over CDC-ACM serial.

Transport classes:
* :class:`HarnessSerial` — pyserial-based SCPI transport (low-level).
* :class:`Harness` — board-like wrapper that acts like ``board``
  and provides pin objects for use with :class:`digitalio.DigitalInOut`.
* :class:`HarnessVISA` — PyVISA-based SCPI transport for VISA tooling.

Usage::

    # Board-like API (recommended)
    from harness_client import Harness, digitalio

    with Harness(port="pyusb://bridge/CAFE:4002") as h:
        h.dut_pin("LED", 12)
        h.dut_pin("BTN", 9)

        dio = digitalio.DigitalInOut(h.LED)
        dio.direction = digitalio.Direction.OUTPUT
        dio.value = True
        print(dio.value)

    # Low-level SCPI API
    from harness_client import HarnessSerial

    with HarnessSerial(port="pyusb://bridge/CAFE:4002") as h:
        h.dut_pin("LED", 12)
        h.gpio_dir("LED", "OUT")
        h.gpio_write(12, 1)
        print(h.gpio_read(12))
"""

from __future__ import annotations

import re
from typing import Optional


# ---------------------------------------------------------------------------
# IEEE 488.2 arbitrary-block helpers.
# ---------------------------------------------------------------------------


def encode_block(data: bytes) -> bytes:
    """Encode ``data`` as an IEEE 488.2 definite-length arbitrary block."""
    n = str(len(data))
    return f"#{len(n)}{n}".encode("ascii") + data


def decode_block(reply: bytes) -> bytes:
    """Decode the first definite-length arbitrary block in ``reply``."""
    m = re.match(rb"\s*#(\d)", reply)
    if not m:
        raise ValueError(f"not an arbitrary block: {reply!r}")
    ndigits = int(m.group(1))
    hdr_end = m.end()
    length = int(reply[hdr_end : hdr_end + ndigits])
    start = hdr_end + ndigits
    return bytes(reply[start : start + length])


# ---------------------------------------------------------------------------
# Transport-agnostic command surface (mixin).
# ---------------------------------------------------------------------------


class _HarnessCommands:
    """Mixin: turns raw write/query into typed harness commands.

    Subclasses must supply ``write(cmd: str)``, ``query(cmd: str) -> str``,
    and ``query_block(cmd: str) -> bytes``.
    """

    # ---- IEEE 488.2 mandatory ----
    def idn(self) -> str:
        """Return the 4-field instrument identity string."""
        return self.query("*IDN?")

    def reset(self) -> None:
        """*RST — tear down all live buses and release claimed GPIOs."""
        self.write("*RST")

    def cls(self) -> None:
        """*CLS — clear the error queue and event status registers."""
        self.write("*CLS")

    def self_test(self) -> int:
        """*TST? — always returns 0 on this device (boot success)."""
        return int(self.query("*TST?"))

    def opc_wait(self) -> int:
        """*OPC? — barrier; always returns 1 on this device."""
        return int(self.query("*OPC?"))

    # ---- SYSTem (SCPI-required + vendor) ----
    def error_next(self) -> tuple[int, str]:
        """Pop the next error from the queue. Returns ``(0, "No error")`` if empty."""
        reply = self.query("SYST:ERR?")
        code, _, msg = reply.partition(",")
        return int(code), msg.strip().strip('"')

    def error_count(self) -> int:
        """Number of errors still queued."""
        return int(self.query("SYST:ERR:COUN?"))

    def chip(self) -> str:
        """Chip identifier (e.g. ``"ESP32-P4"``)."""
        return self.query("SYST:CHIP?")

    def serial(self) -> str:
        """MAC-derived serial string (12 hex chars)."""
        return self.query("SYST:SER?")

    def free(self) -> tuple[int, int]:
        """Free heap, as ``(internal_bytes, psram_bytes)``."""
        internal, psram = self.query("SYST:FREE?").split(",")
        return int(internal), int(psram)

    def uptime_ms(self) -> int:
        """Milliseconds since boot."""
        return int(self.query("SYST:UPT?"))

    def reboot(self) -> None:
        """Reboot the device. No reply is sent."""
        self.write("SYST:REB")

    # ---- DUT metadata (NVS-backed) ----
    def dut_name(self, name: Optional[str] = None) -> Optional[str]:
        """Get or set the DUT board name."""
        if name is None:
            r = self.query("DUT:NAME?")
            return r.strip().strip('"') or None
        self.write(f'DUT:NAME "{name}"')
        return None

    def dut_note(self, text: Optional[str] = None) -> Optional[str]:
        """Get or set the free-form DUT note."""
        if text is None:
            r = self.query("DUT:NOTE?")
            return r.strip().strip('"') or None
        self.write(f'DUT:NOTE "{text}"')
        return None

    def dut_pin(self, label: str, gpio: Optional[int] = None) -> Optional[int]:
        """Get or set a DUT pin label → GPIO binding.

        ``gpio=-1`` records a logical pin with no GPIO (VCC, GND, NC).
        """
        if gpio is None:
            return int(self.query(f'DUT:PIN? "{label}"'))
        self.write(f'DUT:PIN "{label}",{gpio}')
        return None

    def dut_pin_del(self, label: str) -> None:
        """Remove a pin label."""
        self.write(f'DUT:PIN:DEL "{label}"')

    def dut_pin_list(self) -> dict[str, int]:
        """Return ``{label: gpio}`` for every recorded DUT pin."""
        r = self.query("DUT:PIN:LIST?").strip()
        if not r:
            return {}
        toks = _split_csv(r)
        out: dict[str, int] = {}
        for label, gpio in zip(toks[0::2], toks[1::2]):
            out[label.strip('"')] = int(gpio)
        return out

    def dut_wire(self, dut_label: str, host_label: str) -> None:
        """Record (or update) a wire entry mapping ``dut_label`` ↔ ``host_label``."""
        self.write(f'DUT:WIRE "{dut_label}","{host_label}"')

    def dut_wire_del(self, dut_label: str) -> None:
        """Remove a wire entry by its DUT-side label."""
        self.write(f'DUT:WIRE:DEL "{dut_label}"')

    def dut_wire_list(self) -> list[tuple[str, str]]:
        """Return ``[(dut_label, host_label), ...]`` for every recorded wire."""
        r = self.query("DUT:WIRE:LIST?").strip()
        if not r:
            return []
        toks = _split_csv(r)
        return [(toks[i].strip('"'), toks[i + 1].strip('"')) for i in range(0, len(toks), 2)]

    def dut_clear(self) -> None:
        """Wipe all DUT metadata from NVS."""
        self.write("DUT:CLE")

    # ---- GPIO ----
    def gpio_dir(self, pin, direction: str) -> None:
        """Set pin direction: ``IN`` / ``OUT`` / ``INOUT`` / ``OFF``.

        ``pin`` may be an integer GPIO number or a quoted DUT label string.
        """
        self.write(f"GPIO:DIR {_pin(pin)},{direction}")

    def gpio_dir_query(self, pin) -> str:
        """Return ``"OWNED"`` if the harness configured this pin, else ``"OFF"``."""
        return self.query(f"GPIO:DIR? {_pin(pin)}").strip()

    def gpio_pull(self, pin, pull: str) -> None:
        """Set pull resistors: ``NONE`` / ``UP`` / ``DOWN`` / ``UPDOWN``."""
        self.write(f"GPIO:PULL {_pin(pin)},{pull}")

    def gpio_write(self, pin, level: int) -> None:
        """Drive ``pin`` to ``level`` (0 or 1)."""
        self.write(f"GPIO:WRIT {_pin(pin)},{int(bool(level))}")

    def gpio_read(self, pin) -> int:
        """Read the level of ``pin``, returning 0 or 1."""
        return int(self.query(f"GPIO:READ? {_pin(pin)}"))

    def gpio_toggle(self, pin) -> None:
        """Invert current output level of ``pin``."""
        self.write(f"GPIO:TOGG {_pin(pin)}")

    def gpio_pulse(self, pin, microseconds: int) -> None:
        """Drive ``pin`` high then low for ``microseconds``."""
        self.write(f"GPIO:PUL {_pin(pin)},{int(microseconds)}")

    # ---- I2C controller ----
    def i2c_cont_init(self, sda: int, scl: int, hz: int) -> None:
        self.write(f"BUS:I2C:CONT:INIT {sda},{scl},{hz}")

    def i2c_cont_deinit(self) -> None:
        self.write("BUS:I2C:CONT:DEIN")

    def i2c_cont_scan(self) -> list[int]:
        r = self.query("BUS:I2C:CONT:SCAN?").strip()
        return [int(x) for x in r.split(",")] if r else []

    def i2c_cont_write(self, addr: int, data: bytes) -> None:
        self.write(f"BUS:I2C:CONT:WRIT {addr},{_block_arg(data)}")

    def i2c_cont_read(self, addr: int, n: int) -> bytes:
        return self.query_block(f"BUS:I2C:CONT:READ? {addr},{n}")

    def i2c_cont_xfer(self, addr: int, write: bytes, read_n: int) -> bytes:
        return self.query_block(f"BUS:I2C:CONT:XFER? {addr},{_block_arg(write)},{read_n}")

    # ---- I2C target ----
    def i2c_targ_init(self, sda: int, scl: int, addr: int) -> None:
        self.write(f"BUS:I2C:TARG:INIT {sda},{scl},{addr}")

    def i2c_targ_deinit(self) -> None:
        self.write("BUS:I2C:TARG:DEIN")

    def i2c_targ_write(self, data: bytes) -> None:
        self.write(f"BUS:I2C:TARG:WRIT {_block_arg(data)}")

    def i2c_targ_read(self, n: int, timeout_ms: int = 100) -> bytes:
        return self.query_block(f"BUS:I2C:TARG:READ? {n},{timeout_ms}")

    def i2c_targ_state(self) -> tuple[int, int]:
        tx, rx = self.query("BUS:I2C:TARG:STAT?").split(",")
        return int(tx), int(rx)

    # ---- SPI controller ----
    def spi_cont_init(self, sck: int, mosi: int, miso: int, cs: int, hz: int, mode: int) -> None:
        self.write(f"BUS:SPI:CONT:INIT {sck},{mosi},{miso},{cs},{hz},{mode}")

    def spi_cont_deinit(self) -> None:
        self.write("BUS:SPI:CONT:DEIN")

    def spi_cont_xfer(self, data: bytes) -> bytes:
        return self.query_block(f"BUS:SPI:CONT:XFER? {_block_arg(data)}")

    def spi_cont_write(self, data: bytes) -> None:
        self.write(f"BUS:SPI:CONT:WRIT {_block_arg(data)}")

    def spi_cont_cs(self, level: int) -> None:
        self.write(f"BUS:SPI:CONT:CS {int(bool(level))}")

    # ---- SPI target ----
    def spi_targ_init(self, sck: int, mosi: int, miso: int, cs: int, mode: int) -> None:
        self.write(f"BUS:SPI:TARG:INIT {sck},{mosi},{miso},{cs},{mode}")

    def spi_targ_deinit(self) -> None:
        self.write("BUS:SPI:TARG:DEIN")

    def spi_targ_xfer(self, data: bytes, timeout_ms: int = 1000) -> bytes:
        return self.query_block(f"BUS:SPI:TARG:XFER? {_block_arg(data)},{timeout_ms}")

    # ---- UART ----
    def uart_init(
        self, tx: int, rx: int, baud: int, databits: int = 8, parity: str = "NONE", stop: int = 1
    ) -> None:
        self.write(f"BUS:UART:INIT {tx},{rx},{baud},{databits},{parity},{stop}")

    def uart_deinit(self) -> None:
        self.write("BUS:UART:DEIN")

    def uart_write(self, data: bytes) -> None:
        self.write(f"BUS:UART:WRIT {_block_arg(data)}")

    def uart_read(self, n: int, timeout_ms: int = 100) -> bytes:
        return self.query_block(f"BUS:UART:READ? {n},{timeout_ms}")

    def uart_drain(self) -> None:
        self.write("BUS:UART:DRA")


# ---------------------------------------------------------------------------
# pyserial-backed transport.
# ---------------------------------------------------------------------------


class HarnessSerial(_HarnessCommands):
    """SCPI client over a raw pyserial port.

    No PyVISA dependency. Opens the SCPI port of an esp-harness device
    (0xCAFE:0x4002) and exposes typed commands via the :class:`_HarnessCommands`
    mixin.

    Args:
        port: Serial port path or ``pyusb://`` URL.
        baud_rate: Baud rate (ignored for CDC-ACM, declare for compat).
        timeout_s: Read timeout in seconds.

    Usage::

        with HarnessSerial("pyusb://bridge/CAFE:4002") as h:
            print(h.idn())
            h.gpio_dir(12, "OUT")
            h.gpio_write(12, 1)
    """

    def __init__(
        self, port: str = "/dev/ttyACM0", *, baud_rate: int = 115200, timeout_s: float = 2.0
    ):
        import serial as _serial

        self._ser = _serial.Serial(port, baudrate=baud_rate, timeout=timeout_s)

    def write(self, cmd: str) -> None:
        """Send a SCPI command (no reply expected)."""
        self._ser.write(cmd.encode("ascii") + b"\n")

    def query(self, cmd: str) -> str:
        """Send a SCPI query and read the reply line."""
        self.write(cmd)
        line = self._ser.readline()
        return line.rstrip(b"\r\n").decode("latin-1")

    def query_block(self, cmd: str) -> bytes:
        """Send a query that returns an IEEE 488.2 arbitrary block.

        Parses the ``#<n><len>`` header and reads exactly the announced
        number of bytes, then consumes the trailing newline.
        """
        self.write(cmd)
        header = self._ser.read(2)
        if not header.startswith(b"#"):
            extra = self._ser.read_until(b"\n")
            raise ValueError(f"not a block reply: {(header + extra)!r}")
        ndigits = int(chr(header[1]))
        length = int(self._ser.read(ndigits))
        payload = self._ser.read(length)
        # consume the trailing newline
        self._ser.read_until(b"\n")
        return payload

    def close(self) -> None:
        """Close the serial port."""
        if self._ser:
            self._ser.close()
            self._ser = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


# ---------------------------------------------------------------------------
# HarnessPin — a pin descriptor for use with digitalio.DigitalInOut.
# ---------------------------------------------------------------------------


class HarnessPin:
    """A GPIO pin on the DUT, accessible through a :class:`Harness`.

    These are typically obtained as attributes of a :class:`Harness`
    (e.g. ``harness.LED``) and passed to :class:`digitalio.DigitalInOut`.

    Attributes:
        gpio: The GPIO number on the DUT.
        label: The human-readable pin label (e.g. ``"LED"``, ``"D12"``).
    """

    def __init__(self, harness: _HarnessCommands, gpio: int, label: str | None = None):
        self._harness = harness
        self._gpio = gpio
        self._label = label

    @property
    def gpio(self) -> int:
        """The GPIO number on the DUT."""
        return self._gpio

    @property
    def label(self) -> str | None:
        """The human-readable pin label, or ``None`` if unnamed."""
        return self._label

    def __repr__(self) -> str:
        if self._label:
            return f"HarnessPin({self._label!r}, gpio={self._gpio})"
        return f"HarnessPin(gpio={self._gpio})"


# ---------------------------------------------------------------------------
# Harness — board-like wrapper.
# ---------------------------------------------------------------------------


class Harness:
    """Board-like harness for CircuitPython-compatible hardware testing.

    Acts like the CircuitPython ``board`` module: pin labels registered
    via :meth:`dut_pin` become accessible as attributes. Pins returned
    by attribute access are :class:`HarnessPin` objects, which can be
    passed to :class:`digitalio.DigitalInOut`.

    The :attr:`board_id` property reports the configured DUT board name.

    All low-level SCPI methods from :class:`_HarnessCommands` are
    available through the :attr:`_scpi` attribute for advanced use.

    Usage::

        from harness_client import Harness, digitalio

        with Harness(port="pyusb://bridge/CAFE:4002") as h:
            # Register pin labels (persisted in NVS on the harness)
            h.dut_pin("LED", 12)
            h.dut_pin("BTN", 9)

            # Access like the board module
            dio = digitalio.DigitalInOut(h.LED)
            dio.direction = digitalio.Direction.OUTPUT
            dio.value = True

            # Numeric pins also work
            dio2 = digitalio.DigitalInOut(h.D4)
            dio2.direction = digitalio.Direction.INPUT
            print(dio2.value)

            # Board identity
            print(h.board_id)
    """

    def __init__(
        self,
        port: str = "pyusb://bridge/CAFE:4002",
        *,
        baud_rate: int = 115200,
        timeout_s: float = 2.0,
    ):
        self._ser = HarnessSerial(port=port, baud_rate=baud_rate, timeout_s=timeout_s)
        self._pin_cache: dict[int, HarnessPin] = {}
        self._board_id: str | None = None

    # -- Board identification --

    @property
    def board_id(self) -> str:
        """The configured DUT board name, or ``"unknown"`` if not set."""
        if self._board_id is None:
            name = self._ser.dut_name()
            self._board_id = name or "unknown"
        return self._board_id

    # -- Pin attribute access (board-like) --

    def __getattr__(self, name: str):
        if name.startswith("_"):
            raise AttributeError(name)

        # Look up as a registered DUT pin label
        try:
            gpio = self._ser.dut_pin(name)
            if gpio >= 0:
                return self._get_pin(gpio, name)
        except (ValueError, RuntimeError):
            pass

        # Fall back to numeric convention: D12 -> GPIO 12
        if name.startswith("D") and name[1:].isdigit():
            gpio = int(name[1:])
            return self._get_pin(gpio, name)

        raise AttributeError(f"'{type(self).__name__}' object has no pin '{name}'")

    def __dir__(self) -> list[str]:
        """List known pin labels from the harness plus standard names."""
        std = ["board_id", "_scpi"]
        try:
            pins = self._ser.dut_pin_list()
        except Exception:
            pins = {}
        return std + list(pins.keys())

    def _get_pin(self, gpio: int, label: str) -> HarnessPin:
        if gpio not in self._pin_cache:
            self._pin_cache[gpio] = HarnessPin(self._ser, gpio, label)
        return self._pin_cache[gpio]

    # -- Low-level SCPI access --

    @property
    def _scpi(self) -> HarnessSerial:
        """Access the underlying SCPI transport for advanced commands."""
        return self._ser

    # -- Forwarded convenience methods --

    def dut_pin(self, label: str, gpio: int | None = None) -> int | None:
        """Get or set a DUT pin label mapping."""
        return self._ser.dut_pin(label, gpio)

    def dut_name(self, name: str | None = None) -> str | None:
        """Get or set the DUT board name."""
        return self._ser.dut_name(name)

    def dut_note(self, text: str | None = None) -> str | None:
        """Get or set a free-form DUT note."""
        return self._ser.dut_note(text)

    def dut_pin_list(self) -> dict[str, int]:
        """Return ``{label: gpio}`` for every recorded DUT pin."""
        return self._ser.dut_pin_list()

    def idn(self) -> str:
        """Return the instrument identity string."""
        return self._ser.idn()

    def close(self) -> None:
        """Close the serial connection to the harness."""
        self._ser.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


# ---------------------------------------------------------------------------
# digitalio — CircuitPython-compatible digital I/O for the harness.
# ---------------------------------------------------------------------------

import enum as _enum


class _Direction(_enum.Enum):
    INPUT = 0
    OUTPUT = 1


class _Pull(_enum.Enum):
    NONE = 0
    UP = 1
    DOWN = 2


class _DriveMode(_enum.Enum):
    PUSH_PULL = 0
    OPEN_DRAIN = 1


class _DigitalInOut:
    """CircuitPython-compatible DigitalInOut backed by the harness SCPI GPIO.

    Usage::

        from harness_client import Harness, digitalio

        h = Harness(port="pyusb://bridge/CAFE:4002")
        h.dut_pin("LED", 12)

        dio = digitalio.DigitalInOut(h.LED)
        dio.direction = digitalio.Direction.OUTPUT
        dio.value = True
    """

    def __init__(self, pin: HarnessPin):
        if not isinstance(pin, HarnessPin):
            raise TypeError(
                f"expected a HarnessPin, got {type(pin).__name__}. "
                "Use Harness attribute access (e.g. harness.LED) to get a pin."
            )
        self._harness = pin._harness
        self._pin = pin
        self._direction: _Direction | None = None
        self._pull: _Pull = _Pull.NONE
        self._drive_mode: _DriveMode = _DriveMode.PUSH_PULL

    # -- Direction --

    @property
    def direction(self) -> _Direction | None:
        """The pin direction, or ``None`` if not yet configured."""
        return self._direction

    @direction.setter
    def direction(self, value: _Direction) -> None:
        if not isinstance(value, _Direction):
            raise TypeError(f"expected Direction, got {type(value).__name__}")
        self._direction = value
        if value == _Direction.OUTPUT:
            self._harness.gpio_dir(self._pin.gpio, "OUT")
        else:
            self._harness.gpio_dir(self._pin.gpio, "IN")

    # -- Value --

    @property
    def value(self) -> bool:
        """The digital logic level of the pin (``True`` = high, ``False`` = low)."""
        return bool(self._harness.gpio_read(self._pin.gpio))

    @value.setter
    def value(self, val: bool) -> None:
        self._harness.gpio_write(self._pin.gpio, 1 if val else 0)

    # -- Pull --

    @property
    def pull(self) -> _Pull:
        """The pull resistor configuration."""
        return self._pull

    @pull.setter
    def pull(self, value: _Pull) -> None:
        if not isinstance(value, _Pull):
            raise TypeError(f"expected Pull, got {type(value).__name__}")
        self._pull = value
        _PULL_MAP = {_Pull.NONE: "NONE", _Pull.UP: "UP", _Pull.DOWN: "DOWN"}
        self._harness.gpio_pull(self._pin.gpio, _PULL_MAP[value])

    # -- Drive mode --

    @property
    def drive_mode(self) -> _DriveMode:
        """The output drive mode."""
        return self._drive_mode

    @drive_mode.setter
    def drive_mode(self, value: _DriveMode) -> None:
        if not isinstance(value, _DriveMode):
            raise TypeError(f"expected DriveMode, got {type(value).__name__}")
        self._drive_mode = value
        # The harness SCPI interface doesn't have a direct drive mode command.
        # For OPEN_DRAIN, reconfigure as INOUT; for PUSH_PULL, use OUT.
        if value == _DriveMode.OPEN_DRAIN:
            self._harness.gpio_dir(self._pin.gpio, "INOUT")
        else:
            if self._direction == _Direction.OUTPUT:
                self._harness.gpio_dir(self._pin.gpio, "OUT")

    def __repr__(self) -> str:
        return f"DigitalInOut({self._pin!r}, direction={self._direction}, value={self.value})"

    def deinit(self) -> None:
        """Release the pin by setting direction to OFF."""
        self._harness.gpio_dir(self._pin.gpio, "OFF")
        self._direction = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.deinit()


class _DigitalIO:
    """Namespace that mirrors the CircuitPython ``digitalio`` module.

    Attributes:
        DigitalInOut: Class for digital I/O on a pin.
        Direction: Enum with ``INPUT`` and ``OUTPUT``.
        Pull: Enum with ``NONE``, ``UP``, and ``DOWN``.
        DriveMode: Enum with ``PUSH_PULL`` and ``OPEN_DRAIN``.
    """

    Direction = _Direction
    Pull = _Pull
    DriveMode = _DriveMode
    DigitalInOut = _DigitalInOut


digitalio = _DigitalIO()


# ---------------------------------------------------------------------------
# pyvisa-backed transport (optional, for VISA tooling compat).
# ---------------------------------------------------------------------------


class HarnessVISA(_HarnessCommands):
    """SCPI client over PyVISA.

    Requires ``pyvisa`` and ``pyvisa-py``. Open the harness by VISA resource
    string.

    Usage::

        with HarnessVISA("ASRL/dev/ttyACM0::INSTR") as h:
            print(h.idn())
    """

    def __init__(
        self,
        resource: str = "ASRL/dev/ttyACM0::INSTR",
        *,
        timeout_ms: int = 2000,
        baud_rate: int = 115200,
    ):
        import pyvisa

        rm = pyvisa.ResourceManager("@py")
        self._inst = rm.open_resource(
            resource,
            baud_rate=baud_rate,
            read_termination="\n",
            write_termination="\n",
        )
        self._inst.timeout = timeout_ms

    def write(self, cmd: str) -> None:
        self._inst.write(cmd)

    def query(self, cmd: str) -> str:
        return self._inst.query(cmd)

    def query_block(self, cmd: str) -> bytes:
        return decode_block(self._inst.query(cmd).encode("latin-1"))

    def close(self) -> None:
        self._inst.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


# ---------------------------------------------------------------------------
# Helpers.
# ---------------------------------------------------------------------------


def _pin(p) -> str:
    """Render a pin argument as either an int or a quoted DUT label."""
    if isinstance(p, int):
        return str(p)
    return f'"{p}"'


def _block_arg(data: bytes) -> str:
    """Encode bytes as an IEEE 488.2 block literal for embedding in a command."""
    return encode_block(data).decode("latin-1")


def _split_csv(s: str) -> list[str]:
    """Split a comma-separated list, respecting ``"quoted,strings"``."""
    out: list[str] = []
    buf = []
    in_q = False
    for ch in s:
        if ch == '"':
            in_q = not in_q
            buf.append(ch)
        elif ch == "," and not in_q:
            out.append("".join(buf).strip())
            buf = []
        else:
            buf.append(ch)
    if buf:
        out.append("".join(buf).strip())
    return out
