# SPDX-FileCopyrightText: 2026 Scott Shawcroft for Adafruit Industries
# SPDX-License-Identifier: MIT

"""ESP32 hardware-in-the-loop test backend.

Uses esptool.py to flash the CIRCUITPY FAT image to the user_fs partition
and communicates over serial (local or via pyserial-pyusb over USB/IP).

Optionally captures Perfetto traces from an esp-perfetto-logic device
running alongside the DUT (Device Under Test).
"""

import csv
import gc
import logging
import os
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

import serial
import serial.tools.list_ports

from . import SerialSaver

logger = logging.getLogger(__name__)


def _ensure_pyusb_handler():
    """Register pyserial-pyusb URL handler if available.

    Bumps the reconnect timeout and disables auto-reconnect so that USB
    read/write errors propagate to the caller (esptool / our own reset
    helper) instead of silently churning fresh TCP connections.
    """
    try:
        import serial_pyusb  # noqa: F401 — registers pyusb:// on import
        from serial_pyusb.serial import Serial as _PyusbSerial

        _PyusbSerial._RECONNECT_TIMEOUT = max(_PyusbSerial._RECONNECT_TIMEOUT, 30.0)
        _PyusbSerial._AUTO_RECONNECT = False
    except ImportError:
        pass


# DTR/RTS <-> EN/BOOT mapping on Espressif dev kits (cross-coupled
# auto-reset circuit).  wValue is the CP210x SET_MHS payload: high byte
# is the pin mask (0x01=DTR, 0x02=RTS, 0x03=both), low byte the state.
#   (DTR=0,RTS=1) 0x0302 -> EN=LOW  (chip in reset)
#   (DTR=1,RTS=0) 0x0301 -> EN=HIGH, BOOT=LOW (ROM bootloader)
#   (DTR=0,RTS=0) 0x0300 -> EN=HIGH, BOOT=HIGH (run)
#   (DTR=1,RTS=1) 0x0303 -> EN=HIGH, BOOT=HIGH (run, both off)
# The reset->bootloader transition (0x0302 -> 0x0301) MUST happen in a
# single SET_MHS so EN rises at the same instant BOOT falls; EN's RC
# delay then guarantees BOOT is low before the strapping sample.  We use
# the pyusb Serial.setDTRandRTS() atomic combined write for this.
_RESET_DELAY = 0.05


def _reset_to_bootloader(flash_port, baud=115200):
    """Drive the DUT into the ROM bootloader over the CP210x UART bridge.

    We perform the reset ourselves (esptool is invoked with
    ``--before=no-reset`` afterwards) because esptool's own reset
    strategies can't run on ``pyusb://`` ports: ``UnixTightReset`` needs
    a real file descriptor (``ioctl(TIOCMSET)``) and ``ClassicReset``
    sets DTR/RTS in separate transfers, which on the cross-coupled
    auto-reset circuit releases EN with BOOT still high and the chip
    boots normally instead of entering the bootloader.

    The sequence below mirrors esptool's ``UnixTightReset`` using a
    single atomic ``setDTRandRTS`` per step, which is exactly what the
    Linux kernel cp210x driver emits via ``TIOCMSET`` (one ``SET_MHS``
    with mask 0x03).  This is the proven-working sequence from the
    native usbip bridge logs.
    """
    if not flash_port.startswith("pyusb://"):
        # Real tty: let esptool handle the reset (default-reset).
        return
    print(
        f"[DEBUG _reset_to_bootloader] [{time.monotonic():.3f}] Resetting {flash_port} into bootloader"
    )
    _ensure_pyusb_handler()
    ser = _open_serial(flash_port, baudrate=baud, timeout=0.5, write_timeout=1)
    try:
        t0 = time.monotonic()
        # Idle / ensure chip is running first.
        ser.setDTRandRTS(False, False)  # 0x0300
        t1 = time.monotonic()
        ser.setDTRandRTS(True, True)  # 0x0303
        t2 = time.monotonic()
        # Hold reset (EN low).
        ser.setDTRandRTS(False, True)  # 0x0302
        t3 = time.monotonic()
        time.sleep(0.1)
        # Release reset with BOOT low -> ROM bootloader.
        ser.setDTRandRTS(True, False)  # 0x0301
        t4 = time.monotonic()
        time.sleep(_RESET_DELAY)
        # Release BOOT, keep EN high (running the bootloader).
        ser.setDTRandRTS(False, False)  # 0x0300
        t5 = time.monotonic()
        # Drain any stale bootloader output (boot message, etc.) that the
        # ROM prints after entering download mode.  This prevents stale
        # data from contaminating esptool's subsequent sync handshake.
        time.sleep(0.1)
        t6 = time.monotonic()
        print(
            f"[DEBUG _reset_to_bootloader] [{t0:.3f}] DTR/RTS timings: "
            f"step1={t1 - t0:.3f}s step2={t2 - t1:.3f}s step3={t3 - t2:.3f}s "
            f"step4={t4 - t3:.3f}s step5={t5 - t4:.3f}s pre-drain-sleep={t6 - t5:.3f}s"
        )
        drained = b""
        read_count = 0
        t_drain_start = time.monotonic()
        # Only drain data that's already waiting — don't block on a
        # read timeout because the USB/IP backend ignores the serial
        # timeout and uses a 5 s socket timeout instead.
        while True:
            waiting = ser.in_waiting
            if not waiting:
                break
            t_before = time.monotonic()
            chunk = ser.read(min(waiting, 1024))
            t_after = time.monotonic()
            read_count += 1
            print(
                f"[DEBUG _reset_to_bootloader] [{t_after:.3f}] drain read#{read_count}: {len(chunk)} bytes in {t_after - t_before:.3f}s"
            )
            drained += chunk
        if drained:
            print(
                f"[DEBUG _reset_to_bootloader] [{time.monotonic():.3f}] Drained {len(drained)} bytes of stale data in {time.monotonic() - t_drain_start:.3f}s: {drained[:200]!r}"
            )
    finally:
        try:
            ser.close()
        except Exception:
            pass
        # Free the TCP connection to the bridge so esptool can open a
        # fresh one without racing a stale socket.
        gc.collect()
        _close_leaked_usbip_backends()
    # Give the ROM bootloader a moment to be ready before esptool syncs.
    time.sleep(0.05)
    print(f"[DEBUG _reset_to_bootloader] [{time.monotonic():.3f}] done")


def _open_serial(port, **kwargs):
    """Open a serial port, supporting both device paths and pyusb:// URLs."""
    if port.startswith("pyusb://"):
        _ensure_pyusb_handler()
        try:
            result = serial.serial_for_url(port, **kwargs)
            print(f"[DEBUG _open_serial] [{time.monotonic():.3f}] pyusb serial opened: {result}")
            return result
        except Exception as e:
            print(
                f"[DEBUG _open_serial] [{time.monotonic():.3f}] pyusb serial open FAILED: {type(e).__name__}: {e}"
            )
            raise
    result = serial.Serial(port, **kwargs)
    print(f"[DEBUG _open_serial] [{time.monotonic():.3f}] local serial opened: {result}")
    return result


def _close_leaked_usbip_backends():
    """Close every USBIPBackend still alive in the heap.

    Reference cycles between pyserial-pyusb Serial ↔ USBIPBackend ↔
    _UsbipConnection prevent timely GC, leaking TCP connections on the
    bridge. Call this between retries to force-close stale sockets.
    """
    try:
        from usbip_backend.backend import USBIPBackend
    except ImportError:
        return
    closed = 0
    for obj in gc.get_objects():
        if isinstance(obj, USBIPBackend):
            try:
                obj.close()
                closed += 1
            except Exception:
                pass
    if closed:
        print(f"[DEBUG _close_leaked_usbip_backends] Closed {closed} leaked backend(s)")


# Sentinel printed by main.c after every code.py run.
CODE_PY_DONE = "Press any key to enter the REPL"


def _esptool_before_arg(port):
    """Pick the right --before reset strategy for the given port.

    For ``pyusb://`` ports we drive the chip into the bootloader
    ourselves via :func:`_reset_to_bootloader` and tell esptool not to
    reset (``--before=no-reset``) — esptool's own reset strategies can't
    run over pyusb (no ``fileno()`` for the ioctl, and separate
    setDTR/setRTS transfers defeat the auto-reset circuit).  For real
    tty ports, let esptool do its standard reset.
    """
    return "no-reset" if port.startswith("pyusb://") else "default-reset"


def flash_firmware(port, firmware_bin, chip="auto", baud=921600, offset=0x0):
    """Flash a combined CircuitPython firmware image to an ESP device.

    Done once at session start (e.g. via a session-scoped pytest fixture)
    so that subsequent per-test fixtures only need to refresh the user_fs
    partition. `firmware_bin` is the combined image (bootloader +
    partition table + app) produced by the espressif Makefile, written at
    flash offset 0x0.
    """
    before = _esptool_before_arg(port)
    if port.startswith("pyusb://"):
        _reset_to_bootloader(port, baud=baud)
    args = [
        "--chip",
        chip,
        "-p",
        port,
        "--baud",
        str(baud),
        f"--before={before}",
        "--after=no-reset-stub",
        "write-flash",
        hex(offset),
        str(firmware_bin),
    ]
    if port.startswith("pyusb://"):
        _ensure_pyusb_handler()
        import esptool
        import esptool.loader
        from serial_pyusb.serial import Serial as _PyusbSerial

        # Increase timeouts for USB/IP connections.  The default 0.1 s sync
        # timeout and 10 s write timeout are often too short when the bridge
        # adds TCP latency and CP210x buffer NAK delays.
        esptool.loader.SYNC_TIMEOUT = max(esptool.loader.SYNC_TIMEOUT, 3.0)
        esptool.loader.DEFAULT_SERIAL_WRITE_TIMEOUT = max(
            esptool.loader.DEFAULT_SERIAL_WRITE_TIMEOUT, 30.0
        )
        # Read up to 512 bytes per USB bulk transfer to drain the CP210x
        # FIFO before it overflows at high baud rates.
        _PyusbSerial._USB_READ_SIZE = max(_PyusbSerial._USB_READ_SIZE, 512)
        _PyusbSerial._RECONNECT_TIMEOUT = max(_PyusbSerial._RECONNECT_TIMEOUT, 30.0)
        _PyusbSerial._AUTO_RECONNECT = False
        print(f"[DEBUG flash_firmware] SYNC_TIMEOUT={esptool.loader.SYNC_TIMEOUT}")
        print(
            f"[DEBUG flash_firmware] DEFAULT_SERIAL_WRITE_TIMEOUT={esptool.loader.DEFAULT_SERIAL_WRITE_TIMEOUT}"
        )
        print(f"[DEBUG flash_firmware] _USB_READ_SIZE={_PyusbSerial._USB_READ_SIZE}")

        logger.info("flash_firmware (python): %s", " ".join(args))
        print("[DEBUG flash_firmware] Running esptool.main()...")
        try:
            esptool.main(args)
            print("[DEBUG flash_firmware] esptool.main() completed successfully")
        except Exception as e:
            print(f"[DEBUG flash_firmware] esptool.main() FAILED: {type(e).__name__}: {e}")
            raise
        finally:
            # esptool normally calls esp._port.close() on its happy path,
            # which now also tears down the underlying USB/IP backend's
            # TCP socket. Force a GC pass and aggressively close any
            # backends that survived (e.g. when USBError escapes esptool's
            # FatalError/SerialException/OSError handler without closing).
            print(
                "[DEBUG flash_firmware] Running gc.collect() + _close_leaked_usbip_backends()..."
            )
            gc.collect()
            _close_leaked_usbip_backends()
            print("[DEBUG flash_firmware] gc.collect() done")
        return
    cmd = [sys.executable, "-m", "esptool"] + args
    logger.info("flash_firmware: %s", " ".join(cmd))
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"flash_firmware failed: {result.stderr}")


# Partition table CSV files, keyed by (flash_size, uf2).
# Paths are relative to ports/espressif/.
ESPRESSIF_PORT = Path(__file__).parent.parent.parent / "espressif"
PARTITION_CSVS = {
    ("2MB", False): "esp-idf-config/partitions-2MB-no-ota-no-uf2.csv",
    ("4MB", False): "esp-idf-config/partitions-4MB-no-uf2.csv",
    ("4MB", True): "esp-idf-config/partitions-4MB.csv",
    ("8MB", False): "esp-idf-config/partitions-8MB-no-uf2.csv",
    ("8MB", True): "esp-idf-config/partitions-8MB.csv",
    ("16MB", False): "esp-idf-config/partitions-16MB-no-uf2.csv",
    ("16MB", True): "esp-idf-config/partitions-16MB.csv",
    ("32MB", True): "esp-idf-config/partitions-32MB.csv",
}


def _parse_size(s):
    """Parse a size string like '1216K' or '4MB' into bytes."""
    s = s.strip()
    if s.upper().endswith("K"):
        return int(s[:-1]) * 1024
    if s.upper().endswith("MB"):
        return int(s[:-2]) * 1024 * 1024
    if s.upper().endswith("M"):
        return int(s[:-1]) * 1024 * 1024
    return int(s, 0)


def find_user_fs_partition(flash_size, uf2=False):
    """Return (offset, size) in bytes for the user_fs partition.

    Parses the appropriate partition CSV for the given flash configuration.
    """
    key = (flash_size, uf2)
    csv_rel = PARTITION_CSVS.get(key)
    if csv_rel is None:
        raise ValueError(
            f"No partition table for flash_size={flash_size}, uf2={uf2}. "
            f"Known configs: {list(PARTITION_CSVS.keys())}"
        )
    csv_path = ESPRESSIF_PORT / csv_rel
    if not csv_path.exists():
        raise FileNotFoundError(f"Partition CSV not found: {csv_path}")

    with open(csv_path) as f:
        for row in csv.reader(f):
            # Skip comments and empty lines.
            if not row or row[0].strip().startswith("#"):
                continue
            name = row[0].strip()
            if name == "user_fs":
                offset = int(row[3].strip(), 0)
                size = _parse_size(row[4])
                return offset, size

    raise ValueError(f"user_fs partition not found in {csv_path}")


# ---------------------------------------------------------------------------
# SUMP protocol constants for esp-perfetto-logic
# ---------------------------------------------------------------------------

SUMP_RESET = b"\x00"
SUMP_RUN = b"\x01"
SUMP_ID = b"\x02"
SUMP_METADATA = b"\x04"
SUMP_SET_FORMAT = b"\x05"
SUMP_FORMAT_PERFETTO = b"\x01"
SUMP_SET_DIVIDER = b"\x80"
SUMP_SET_READ_DELAY = b"\x81"
SUMP_SET_FLAGS = b"\x82"
SUMP_SET_PIN_MAP = b"\x83"
SUMP_SET_ENABLE_PIN = b"\x84"
SUMP_SET_EXT_READ_COUNT = b"\x85"

# Perfetto trace framing
TRACE_PACKET_TAG = 0x0A
END_SENTINEL = bytes([TRACE_PACKET_TAG, 0x00])


def _sump_long_cmd(cmd_byte, data_u32):
    """Build a 5-byte SUMP long command."""
    return cmd_byte + struct.pack("<I", data_u32)


class SumpLogicAnalyzer:
    """Driver for an esp-perfetto-logic device over serial.

    Sends SUMP commands to configure the logic analyzer, triggers a capture
    in Perfetto streaming mode, and collects the trace in a background thread.
    """

    def __init__(self, port, baud=115200, timeout=1):
        self._port_name = port
        self._baud = baud
        self._ser = _open_serial(port, baudrate=baud, timeout=timeout)
        self._capture_thread = None
        self._trace_data = b""
        self._capture_done = threading.Event()
        self._reset()

    def _reset(self):
        """Send 5x reset to ensure known state."""
        for _ in range(5):
            self._ser.write(SUMP_RESET)
        time.sleep(0.1)
        self._ser.reset_input_buffer()

    def set_pin_mapping(self, channel_gpio_map):
        """Map logical channels to GPIOs.

        channel_gpio_map: dict of {channel_num: gpio_num}
        """
        for channel, gpio in channel_gpio_map.items():
            self._ser.write(_sump_long_cmd(SUMP_SET_PIN_MAP, channel | (gpio << 8)))

    def set_sample_rate(self, clock_hz, rate_hz):
        """Set the sample rate via the divider register."""
        divider = max(0, (clock_hz // rate_hz) - 1)
        self._ser.write(_sump_long_cmd(SUMP_SET_DIVIDER, divider))

    def set_sample_count(self, count):
        """Set the number of samples to capture (0 = streaming/unlimited)."""
        self._ser.write(_sump_long_cmd(SUMP_SET_EXT_READ_COUNT, count))

    def start_perfetto_capture(self, trace_file):
        """Configure Perfetto output and start capture.

        The capture runs in a background thread. Call stop_capture() or
        wait for the device to send the end sentinel.
        """
        self._trace_file = Path(trace_file)
        self._trace_data = b""
        self._capture_done.clear()

        # Select Perfetto output format.
        self._ser.write(SUMP_SET_FORMAT + SUMP_FORMAT_PERFETTO)
        time.sleep(0.05)

        # Arm and run.
        self._ser.write(SUMP_RUN)

        self._capture_thread = threading.Thread(target=self._capture_loop, daemon=True)
        self._capture_thread.start()

    def _capture_loop(self):
        """Background thread: read Perfetto trace from serial until sentinel."""
        buf = b""
        # Skip any boot log text and find the trace start.
        while not self._capture_done.is_set():
            chunk = self._ser.read(self._ser.in_waiting or 1)
            if not chunk:
                continue
            buf += chunk
            # Look for the Perfetto trace start (0x0A followed by non-text).
            while len(buf) >= 2:
                idx = buf.find(bytes([TRACE_PACKET_TAG]))
                if idx == -1:
                    buf = buf[-1:]
                    break
                next_byte = buf[idx + 1]
                if next_byte > 0x7F or (0 < next_byte < 0x20 and next_byte not in (0x0A, 0x0D)):
                    # Found trace start.
                    buf = buf[idx:]
                    self._stream_trace(buf)
                    return
                buf = buf[idx + 1 :]
            if len(buf) > 4096:
                buf = buf[-256:]

    def _stream_trace(self, leading):
        """Read trace data until the end sentinel or capture is stopped."""
        data = bytearray(leading)
        tail = leading[-1:] if leading else b""

        while not self._capture_done.is_set():
            chunk = self._ser.read(self._ser.in_waiting or 1)
            if not chunk:
                continue

            combined = tail + chunk
            sentinel_pos = combined.find(END_SENTINEL)
            if sentinel_pos != -1:
                keep = sentinel_pos - len(tail)
                if keep > 0:
                    data.extend(chunk[:keep])
                break

            data.extend(chunk)
            tail = combined[-len(END_SENTINEL) + 1 :]

        self._trace_data = bytes(data)
        self._trace_file.write_bytes(self._trace_data)
        self._capture_done.set()
        logger.info(
            "Logic analyzer: captured %d bytes to %s",
            len(self._trace_data),
            self._trace_file,
        )

    def stop_capture(self, timeout=10):
        """Wait for capture to finish and return the trace file path."""
        if self._capture_thread is None:
            return self._trace_file
        self._capture_done.wait(timeout=timeout)
        if not self._capture_done.is_set():
            # Force stop if the device didn't send the sentinel.
            self._capture_done.set()
            logger.warning("Logic analyzer capture timed out")
        self._capture_thread.join(timeout=2)
        self._capture_thread = None
        return self._trace_file

    def close(self):
        """Stop any in-progress capture and close the serial port."""
        self._capture_done.set()
        if self._capture_thread is not None:
            self._capture_thread.join(timeout=2)
            self._capture_thread = None
        self._reset()
        self._ser.close()


class EspProcess:
    """Hardware-in-the-loop backend for ESP32 boards.

    Provides the same interface as NativeSimProcess:
      .serial      - SerialSaver wrapping the board's serial port
      .flash_file  - path to the local FAT image (for mcopy readback)
      .wait_until_done()
      .shutdown()
    """

    def __init__(
        self,
        port,
        flash_file,
        user_fs_offset,
        user_fs_size,
        chip="auto",
        timeout=10,
        baud=921600,
        serial_baud=115200,
        logic_analyzer=None,
        trace_file=None,
        flash_port=None,
    ):
        self.port = port
        # Port that esptool talks to. For boards like the ESP32-C6-DevKitM
        # the on-board CP2102N's DTR/RTS are wired to GPIO9 (BOOT) and EN,
        # so flashing must go through it; the chip's native USB CDC
        # (`port`) is reserved for monitoring CircuitPython's REPL output.
        # If unspecified, flash and monitor share the same port.
        self.flash_port = flash_port or port
        self.flash_file = flash_file
        self.trace_file = trace_file
        self._user_fs_offset = user_fs_offset
        self._user_fs_size = user_fs_size
        self._chip = chip
        self._timeout = timeout
        self._baud = baud
        self._serial_baud = serial_baud
        self.serial = None
        self._logic_analyzer = logic_analyzer
        self._code_py_runs_seen = 0
        self._expected_runs = 1

        self._flash_user_fs()
        self._reset_and_connect()

    def _esptool(self, *args):
        """Run esptool.py as a Python module against self.flash_port.

        Using ``python -m esptool`` ensures pyserial-pyusb's URL handler
        is available when the port is a ``pyusb://`` URL.  For pyusb ports
        we drive the chip into the ROM bootloader ourselves first (esptool
        is then invoked with ``--before=no-reset``).
        """
        if self.flash_port.startswith("pyusb://"):
            _reset_to_bootloader(self.flash_port, baud=self._baud)
        cmd = [
            sys.executable,
            "-m",
            "esptool",
            "--chip",
            self._chip,
            "-p",
            self.flash_port,
            "--baud",
            str(self._baud),
        ]
        cmd.extend(args)
        logger.info("esptool: %s", " ".join(cmd))

        env = os.environ.copy()
        if self.flash_port.startswith("pyusb://"):
            _ensure_pyusb_handler()
            self._esptool_python(*args)
            return ""

        result = subprocess.run(cmd, capture_output=True, text=True, env=env)
        if result.returncode != 0:
            raise RuntimeError(f"esptool failed: {result.stderr}")
        return result.stdout

    def _esptool_python(self, *args):
        """Run esptool commands via its Python API (for pyusb:// ports)."""
        _ensure_pyusb_handler()
        import esptool
        import esptool.loader
        from serial_pyusb.serial import Serial as _PyusbSerial

        # Increase timeouts for USB/IP connections.
        esptool.loader.SYNC_TIMEOUT = max(esptool.loader.SYNC_TIMEOUT, 3.0)
        esptool.loader.DEFAULT_SERIAL_WRITE_TIMEOUT = max(
            esptool.loader.DEFAULT_SERIAL_WRITE_TIMEOUT, 30.0
        )
        _PyusbSerial._USB_READ_SIZE = max(_PyusbSerial._USB_READ_SIZE, 512)
        _PyusbSerial._RECONNECT_TIMEOUT = max(_PyusbSerial._RECONNECT_TIMEOUT, 30.0)
        _PyusbSerial._AUTO_RECONNECT = False
        print(f"[DEBUG _esptool_python] SYNC_TIMEOUT={esptool.loader.SYNC_TIMEOUT}")
        print(
            f"[DEBUG _esptool_python] DEFAULT_SERIAL_WRITE_TIMEOUT={esptool.loader.DEFAULT_SERIAL_WRITE_TIMEOUT}"
        )
        print(f"[DEBUG _esptool_python] _USB_READ_SIZE={_PyusbSerial._USB_READ_SIZE}")

        argv = [
            "--chip",
            self._chip,
            "-p",
            self.flash_port,
            "--baud",
            str(self._baud),
        ]
        argv.extend(args)
        logger.info("esptool (python): %s", " ".join(argv))
        print(f"[DEBUG _esptool_python] Running: {' '.join(argv)}")
        try:
            esptool.main(argv)
            print("[DEBUG _esptool_python] esptool.main() succeeded")
        except Exception as e:
            print(f"[DEBUG _esptool_python] esptool.main() FAILED: {type(e).__name__}: {e}")
            raise
        finally:
            # See flash_firmware: drop any backend cycle that survived
            # esptool's port-close path so the bridge sees the TCP
            # socket close before the next per-test invocation.
            print(
                "[DEBUG _esptool_python] Running gc.collect() + _close_leaked_usbip_backends()..."
            )
            gc.collect()
            _close_leaked_usbip_backends()
            print("[DEBUG _esptool_python] gc.collect() done")

    def _flash_user_fs(self):
        """Write the FAT image to the user_fs partition."""
        size = Path(self.flash_file).stat().st_size
        sample = Path(self.flash_file).read_bytes()[:512]
        all_ff = all(b == 0xFF for b in sample)
        logger.info(
            "Flashing user_fs image: %s (%d bytes, offset=%s, first-512-bytes=%s)",
            self.flash_file,
            size,
            hex(self._user_fs_offset),
            "all 0xFF (blank/erased)" if all_ff else f"FAT-like ({sample[:16].hex()}...)",
        )
        before = _esptool_before_arg(self.flash_port)
        self._esptool(
            f"--before={before}",
            "--after=no-reset",
            "write-flash",
            hex(self._user_fs_offset),
            str(self.flash_file),
        )

    def _reset_and_connect(self):
        """Hard-reset the board and open serial for test communication."""
        # Start logic analyzer capture before reset so we catch boot signals.
        if self._logic_analyzer is not None and self.trace_file is not None:
            self._logic_analyzer.start_perfetto_capture(self.trace_file)

        before = _esptool_before_arg(self.flash_port)
        self._esptool(f"--before={before}", "--after=hard-reset", "read-mac")

        # Give the board a moment to boot.
        time.sleep(0.5)

        ser = _open_serial(self.port, baudrate=self._serial_baud, timeout=0.05, write_timeout=1)
        self.serial = SerialSaver(ser, name="esp-serial")

    def wait_until_done(self, runs=None):
        """Wait for code.py to finish by watching for the REPL prompt."""
        if runs is None:
            runs = self._expected_runs
        target_runs = self._code_py_runs_seen + runs
        print(
            f"[DEBUG wait_until_done] Waiting for {runs} run(s) (target={target_runs}, seen={self._code_py_runs_seen})"
        )
        while self._code_py_runs_seen < target_runs:
            print(
                f"[DEBUG wait_until_done] Calling serial.wait_for({CODE_PY_DONE!r}, timeout={self._timeout})..."
            )
            try:
                self.serial.wait_for(CODE_PY_DONE, timeout=self._timeout)
                self._code_py_runs_seen += 1
                print(
                    f"[DEBUG wait_until_done] Got CODE_PY_DONE, seen now {self._code_py_runs_seen}/{target_runs}"
                )
            except TimeoutError:
                print(
                    f"[DEBUG wait_until_done] TIMEOUT after {self._timeout}s. Output so far: {self.serial.all_output[-500:]!r}"
                )
                raise

    def read_back_flash(self):
        """Read the user_fs partition back from hardware into flash_file.

        Call this before using mcopy to inspect the on-device filesystem.
        """
        self.shutdown()
        readback = str(self.flash_file) + ".readback"
        before = _esptool_before_arg(self.flash_port)
        self._esptool(
            f"--before={before}",
            "--after=no-reset",
            "read-flash",
            hex(self._user_fs_offset),
            hex(self._user_fs_size),
            readback,
        )
        # Overwrite local flash_file so mcopy-based assertions work unchanged.
        Path(readback).rename(self.flash_file)

    def shutdown(self):
        """Stop logic analyzer capture and close the serial connection."""
        if self._logic_analyzer is not None:
            try:
                self._logic_analyzer.stop_capture(timeout=self._timeout)
            except Exception as e:
                logger.warning("Logic analyzer stop_capture failed: %s", e)
        if self.serial is not None:
            try:
                self.serial.close()
            except Exception as e:
                logger.warning("Serial close failed: %s", e)
            finally:
                self.serial = None
