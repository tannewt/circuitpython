# SPDX-FileCopyrightText: 2025 Scott Shawcroft for Adafruit Industries
# SPDX-License-Identifier: MIT

"""Pytest fixtures for CircuitPython native_sim and hardware-in-the-loop testing."""

import gc
import logging
import os
import subprocess
from pathlib import Path

import pytest
import serial

from .board_cache import BoardCache, BOARD_CONFIG, DEFAULT_CACHE_PATH
from .perfetto_input_trace import write_input_trace

from perfetto.trace_processor import TraceProcessor

from . import NativeSimProcess
from .esp import (
    EspProcess,
    SumpLogicAnalyzer,
    find_user_fs_partition,
    flash_firmware,
)
from .harness_client import Harness, HarnessSerial

logger = logging.getLogger(__name__)


def pytest_addoption(parser):
    parser.addoption(
        "--update-goldens",
        action="store_true",
        default=False,
        help="Overwrite golden images with captured output instead of comparing.",
    )
    parser.addoption(
        "--board-cache",
        default=None,
        help=(
            "Path to board-cache JSON file. "
            "Maps board_id → mDNS hostname for hardware-in-the-loop testing. "
            "Defaults to <tests_dir>/.board_cache.json"
        ),
    )


def pytest_configure(config):
    # Enable DEBUG logging for USB/IP and pyserial-pyusb to trace
    # endpoint validation and config descriptor issues.
    import logging

    for name in ("usbip_backend", "serial_pyusb", "usb.core"):
        logging.getLogger(name).setLevel(logging.DEBUG)
    config.addinivalue_line(
        "markers", "circuitpy_drive(files): run CircuitPython with files in the flash image"
    )
    config.addinivalue_line(
        "markers", "disable_i2c_devices(*names): disable native_sim I2C emulator devices"
    )
    config.addinivalue_line(
        "markers", "circuitpython_board(board_id): which board id to use in the test"
    )
    config.addinivalue_line(
        "markers",
        "zephyr_sample(sample, board='nrf52_bsim', device_id=1): build and run a Zephyr sample for bsim tests",
    )
    config.addinivalue_line(
        "markers",
        "duration(seconds): native_sim timeout and bsim PHY simulation duration",
    )
    config.addinivalue_line(
        "markers",
        "code_py_runs(count): stop native_sim after count code.py runs (default: 1)",
    )
    config.addinivalue_line(
        "markers",
        "input_trace(trace): inject input signal trace data into native_sim",
    )
    config.addinivalue_line(
        "markers",
        "native_sim_rt: run native_sim in realtime mode (-rt instead of -no-rt)",
    )
    config.addinivalue_line(
        "markers",
        "display(capture_times_ns=None): run test with SDL display; "
        "capture_times_ns is a list of nanosecond timestamps for trace-triggered captures",
    )
    config.addinivalue_line(
        "markers",
        "display_pixel_format(format): override the display pixel format "
        "(e.g. 'RGB_565', 'ARGB_8888')",
    )
    config.addinivalue_line(
        "markers",
        "display_mono_vtiled(value): override the mono vtiled screen_info flag (True or False)",
    )
    config.addinivalue_line(
        "markers",
        "flash_config(erase_block_size=N, total_size=N): override flash simulator parameters",
    )


def pytest_sessionstart(session: pytest.Session) -> None:
    """Discover hardware boards via mDNS and populate the board cache.

    Runs once per session so that :func:`pytest_generate_tests` has
    up-to-date board availability without making network calls during
    collection.
    """
    cache = _get_board_cache(session.config)
    # Single mDNS scan to find all boards on the network.
    cache.discover_all()


def pytest_generate_tests(metafunc: pytest.Metafunc) -> None:
    """Auto-parameterize ``circuitpython`` tests with available boards.

    - Tests that already have an explicit ``circuitpython_board`` marker
      (including those using :func:`pytest.mark.parametrize`) are left
      as-is — the marker drives board selection directly.
    - Tests using native_sim-only markers (``input_trace``, ``display``,
      etc.) run on ``native_native_sim`` only.
    - All other tests are parametrized across ``native_native_sim`` plus
      every cached hardware board (reachability is verified at fixture
      time, not during collection).
    """
    if "circuitpython" not in metafunc.fixturenames:
        return

    # Test already declares its own board(s) — leave it alone.
    if metafunc.definition.get_closest_marker("circuitpython_board") is not None:
        return

    marker_names = {m.name for m in metafunc.definition.iter_markers()}

    if marker_names & NATIVE_SIM_ONLY_MARKERS:
        # Native-sim-only test; no parametrization needed.
        return

    cache = _get_board_cache(metafunc.config)
    # Only look at the cache — no network calls during collection.
    # Actual reachability is checked at fixture time.
    hardware_boards = [b for b in BOARD_CONFIG if cache.get(b) is not None]

    if not hardware_boards:
        return

    params = [
        pytest.param(
            marks=pytest.mark.circuitpython_board("native_native_sim"),
            id="native_native_sim",
        )
    ] + [pytest.param(marks=pytest.mark.circuitpython_board(b), id=b) for b in hardware_boards]
    metafunc.parametrize("", params)


ZEPHYR_CP = Path(__file__).parent.parent
PORTS_DIR = ZEPHYR_CP.parent


def _find_firmware(board_id):
    """Search all port build directories for a board's firmware.bin.

    Returns the Path to firmware.bin, or None if not found.
    """
    for port_dir in sorted(PORTS_DIR.iterdir()):
        if not port_dir.is_dir():
            continue
        if port_dir.name == "__pycache__":
            continue
        candidate = port_dir / f"build-{board_id}" / "firmware.bin"
        if candidate.is_file():
            return candidate
    return None


# Markers that indicate native_sim-only features.
# Tests using any of these markers will only run on native_sim.
NATIVE_SIM_ONLY_MARKERS: set[str] = {
    "display",
    "display_capture",
    "display_pixel_format",
    "display_mono_vtiled",
    "disable_i2c_devices",
    "flash_config",
    "native_sim_rt",
}


def _get_board_cache(config: pytest.Config) -> BoardCache:
    """Return the :class:`BoardCache` for the current session."""
    cache_path = config.getoption("--board-cache", default=None)
    if cache_path is None:
        cache_path = DEFAULT_CACHE_PATH
    else:
        cache_path = Path(cache_path)
    return BoardCache(cache_path)


def _iter_uart_tx_slices(trace_file: Path) -> list[tuple[int, int, str, str]]:
    """Return UART TX slices as (timestamp_ns, duration_ns, text, device_name)."""
    tp = TraceProcessor(file_path=str(trace_file))
    result = tp.query(
        """
        SELECT s.ts, s.dur, s.name, dev.name AS device_name
        FROM slice s
        JOIN track tx ON s.track_id = tx.id
        JOIN track dev ON tx.parent_id = dev.id
        JOIN track uart ON dev.parent_id = uart.id
        WHERE tx.name = "TX" AND uart.name = "UART"
        ORDER BY s.ts
        """
    )
    return [
        (int(row.ts), int(row.dur or 0), row.name or "", row.device_name or "UART")
        for row in result
    ]


def log_uart_trace_output(trace_file: Path) -> None:
    """Log UART TX output from Perfetto trace with timestamps for line starts."""
    if not logger.isEnabledFor(logging.INFO):
        return
    slices = _iter_uart_tx_slices(trace_file)
    if not slices:
        return

    buffers: dict[str, list[str]] = {}
    line_start_ts: dict[str, int | None] = {}

    for ts, dur, text, device in slices:
        if device not in buffers:
            buffers[device] = []
            line_start_ts[device] = None

        if not text:
            continue

        char_step = dur / max(len(text), 1) if dur > 0 else 0.0
        for idx, ch in enumerate(text):
            if line_start_ts[device] is None:
                line_start_ts[device] = int(ts + idx * char_step)
            buffers[device].append(ch)
            if ch == "\n":
                line_text = "".join(buffers[device]).rstrip("\n")
                logger.info(
                    "UART trace %s @%d ns: %s",
                    device,
                    line_start_ts[device],
                    repr(line_text),
                )
                buffers[device] = []
                line_start_ts[device] = None

    for device, buf in buffers.items():
        if buf:
            logger.info(
                "UART trace %s @%d ns (partial): %s",
                device,
                line_start_ts[device] or 0,
                repr("".join(buf)),
            )


@pytest.fixture
def board(request):
    board = request.node.get_closest_marker("circuitpython_board")
    if board is not None:
        board = board.args[0]
    else:
        board = "native_native_sim"
    return board


@pytest.fixture
def native_sim_binary(request, board):
    """Return path to native_sim binary, skip if not built."""
    build_dir = ZEPHYR_CP / f"build-{board}"
    binary = build_dir / "zephyr-cp/zephyr/zephyr.exe"

    if not binary.exists():
        pytest.skip(f"binary not built: {binary}")
    return binary


@pytest.fixture
def native_sim_env() -> dict[str, str]:
    return {}


PIXEL_FORMAT_BITMASK = {
    "RGB_888": 1 << 0,
    "MONO01": 1 << 1,
    "MONO10": 1 << 2,
    "ARGB_8888": 1 << 3,
    "RGB_565": 1 << 4,
    "BGR_565": 1 << 5,
    "L_8": 1 << 6,
    "AL_88": 1 << 7,
}


@pytest.fixture
def pixel_format(request) -> str:
    """Indirect-parametrize fixture: adds display_pixel_format marker."""
    fmt = request.param
    request.node.add_marker(pytest.mark.display_pixel_format(fmt))
    return fmt


@pytest.fixture
def sim_id(request) -> str:
    return request.node.nodeid.replace("/", "_")


def _create_flash_image(tmp_path, drive_marker, flash_total_size, index=0):
    """Create a FAT flash image and populate it with files from the marker.

    Returns (flash_path, files_dict_or_None).
    """
    flash = tmp_path / f"flash-{index}.bin"
    flash.write_bytes(b"\xff" * flash_total_size)
    files = None
    if len(drive_marker.args) == 1:
        files = drive_marker.args[0]
    if files is not None:
        subprocess.run(["mformat", "-i", str(flash), "::"], check=True)
        tmp_drive = tmp_path / f"drive{index}"
        tmp_drive.mkdir(exist_ok=True)

        for name, content in files.items():
            src = tmp_drive / name
            if isinstance(content, bytes):
                src.write_bytes(content)
            else:
                src.write_text(content)
            subprocess.run(["mcopy", "-i", str(flash), str(src), f"::{name}"], check=True)
    return flash


def _harness_url_for(cache: BoardCache, board_id: str | None = None) -> str | None:
    """Return a ``pyusb://`` URL for the harness on a cached board's bridge.

    The harness (VID:PID 0xCAFE:0x4002) exposes an SCPI interface for
    GPIO/bus control and has a built-in logic analyzer.  It lives on the
    same USB/IP bridge as the DUT.

    If *board_id* is given the harness is looked up on that board's
    bridge; otherwise the first cached board is used.
    """
    if board_id is not None:
        entry = cache.get(board_id)
    else:
        for bid in cache.list_cached():
            entry = cache.get(bid)
            if entry is not None:
                board_id = bid
                break
        else:
            entry = None

    if entry is None:
        return None
    return f"pyusb://{entry['host']}/cafe:4002"


def _logic_analyzer_for(
    cache: BoardCache, board_id: str | None = None
) -> SumpLogicAnalyzer | None:
    """Return a :class:`SumpLogicAnalyzer` for the given board's harness.

    The logic analyzer is built into every harness, so if the harness is
    reachable the LA is available at the same URL (the LA speaks SUMP
    over the same serial channel).
    """
    url = _harness_url_for(cache, board_id)
    if url is None:
        return None
    try:
        return SumpLogicAnalyzer(url, baud=115200)
    except Exception:
        return None


@pytest.fixture(scope="session")
def harness(request):
    """Connect to the esp-harness SCPI interface for GPIO/bus control.

    Returns a :class:`HarnessSerial` instance or *None* if no cached
    board with a harness is found.
    """
    cache = _get_board_cache(request.config)
    url = _harness_url_for(cache)
    if url is None:
        return None
    logger.info("Connecting to harness at %s", url)
    h = HarnessSerial(port=url)
    try:
        ident = h.idn()
    except Exception as e:
        logger.warning("Harness at %s did not respond: %s", url, e)
        h.close()
        return None
    logger.info("Harness connected: %s", ident)
    yield h
    h.close()


@pytest.fixture(scope="session")
def harness_board(request):
    """Board-like harness fixture for CircuitPython-compatible digital I/O tests.

    Returns a :class:`Harness` instance (board-like API) or ``None`` if
    the harness is not discovered.

    Usage::

        from harness_client import digitalio

        def test_led_blink(harness_board):
            if harness_board is None:
                pytest.skip("harness not available")
            harness_board.dut_pin("LED", 12)
            dio = digitalio.DigitalInOut(harness_board.LED)
            dio.direction = digitalio.Direction.OUTPUT
            dio.value = True
    """
    cache = _get_board_cache(request.config)
    url = _harness_url_for(cache)
    if url is None:
        return None
    logger.info("Connecting to harness (board) at %s", url)
    h = Harness(port=url)
    try:
        ident = h.idn()
    except Exception as e:
        logger.warning("Harness at %s did not respond: %s", url, e)
        h.close()
        return None
    logger.info("Harness (board) connected: %s", ident)
    yield h
    h.close()


@pytest.fixture
def circuitpython(request, board, sim_id, tmp_path):
    """Run CircuitPython and return a process with serial access.

    The target board is set by ``@pytest.mark.circuitpython_board``
    (or auto-parametrized via :func:`pytest_generate_tests`).

    ``native_native_sim`` and ``native_nrf5340bsim`` use the local
    native simulator.  ``espressif_*`` boards use USB/IP hardware
    discovered via mDNS and cached in the board-cache file.
    """
    if board.startswith("native_"):
        yield from _circuitpython_native(request, board, sim_id, tmp_path)
    elif board.startswith("espressif_"):
        yield from _circuitpython_esp(request, board, tmp_path)
    else:
        pytest.skip(f"Unsupported board: {board}")


def _circuitpython_native(
    request: pytest.FixtureRequest,
    board: str,
    sim_id: str,
    tmp_path: Path,
):
    """Run CircuitPython on a ``native_*`` board (simulator)."""
    native_sim_binary = request.getfixturevalue("native_sim_binary")
    native_sim_env = request.getfixturevalue("native_sim_env")

    instance_count = 1
    if "circuitpython1" in request.fixturenames and "circuitpython2" in request.fixturenames:
        instance_count = 2

    drives = list(request.node.iter_markers_with_node("circuitpy_drive"))
    if len(drives) != instance_count:
        raise RuntimeError(f"not enough drives for {instance_count} instances")

    input_trace_markers = list(request.node.iter_markers_with_node("input_trace"))
    if len(input_trace_markers) > 1:
        raise RuntimeError("expected at most one input_trace marker")

    input_trace = None
    if input_trace_markers and len(input_trace_markers[0][1].args) == 1:
        input_trace = input_trace_markers[0][1].args[0]

    input_trace_file = None
    if input_trace is not None:
        input_trace_file = tmp_path / "input.perfetto"
        write_input_trace(input_trace_file, input_trace)

    marker = request.node.get_closest_marker("duration")
    if marker is None:
        timeout = 10
    else:
        timeout = marker.args[0]

    runs_marker = request.node.get_closest_marker("code_py_runs")
    if runs_marker is None:
        code_py_runs = 1
    else:
        code_py_runs = int(runs_marker.args[0])

    display_marker = request.node.get_closest_marker("display")
    if display_marker is None:
        display_marker = request.node.get_closest_marker("display_capture")

    capture_times_ns = None
    if display_marker is not None:
        capture_times_ns = display_marker.kwargs.get("capture_times_ns", None)

    pixel_format_marker = request.node.get_closest_marker("display_pixel_format")
    pixel_format = None
    if pixel_format_marker is not None and pixel_format_marker.args:
        pixel_format = pixel_format_marker.args[0]

    mono_vtiled_marker = request.node.get_closest_marker("display_mono_vtiled")
    mono_vtiled = None
    if mono_vtiled_marker is not None and mono_vtiled_marker.args:
        mono_vtiled = mono_vtiled_marker.args[0]

    if capture_times_ns is not None:
        if input_trace is None:
            input_trace = {}
        else:
            input_trace = dict(input_trace)
        input_trace["display_capture"] = list(capture_times_ns)
        if input_trace_file is None:
            input_trace_file = tmp_path / "input.perfetto"
        write_input_trace(input_trace_file, input_trace)

    use_realtime = request.node.get_closest_marker("native_sim_rt") is not None

    flash_config_marker = request.node.get_closest_marker("flash_config")
    flash_total_size = 2 * 1024 * 1024
    flash_erase_block_size = None
    flash_write_block_size = None
    if flash_config_marker:
        flash_total_size = flash_config_marker.kwargs.get("total_size", flash_total_size)
        flash_erase_block_size = flash_config_marker.kwargs.get("erase_block_size", None)
        flash_write_block_size = flash_config_marker.kwargs.get("write_block_size", None)

    procs = []
    for i in range(instance_count):
        drives_entry = drives[i][1]
        flash = _create_flash_image(tmp_path, drives_entry, flash_total_size, index=i)

        trace_file = tmp_path / f"trace-{i}.perfetto"

        if "bsim" in board:
            cmd = [str(native_sim_binary), f"--flash_app={flash}"]
            if instance_count > 1:
                cmd.append("-disconnect_on_exit=1")
            cmd.extend(
                (
                    f"-s={sim_id}",
                    f"-d={i}",
                    "-uart0_pty",
                    "-uart0_pty_wait_for_readers",
                    "-uart_pty_wait",
                    f"--vm-runs={code_py_runs + 1}",
                )
            )
        else:
            cmd = [str(native_sim_binary), f"--flash={flash}"]
            realtime_flag = "-rt" if use_realtime else "-no-rt"
            cmd.extend(
                (
                    realtime_flag,
                    "-display_headless",
                    "-i2s_earless",
                    "-wait_uart",
                    f"--vm-runs={code_py_runs + 1}",
                )
            )

        if flash_erase_block_size is not None:
            cmd.append(f"--flash_erase_block_size={flash_erase_block_size}")
        if flash_write_block_size is not None:
            cmd.append(f"--flash_write_block_size={flash_write_block_size}")
        if flash_config_marker and "total_size" in flash_config_marker.kwargs:
            cmd.append(f"--flash_total_size={flash_total_size}")

        if input_trace_file is not None:
            cmd.append(f"--input-trace={input_trace_file}")

        disable_marker = request.node.get_closest_marker("disable_i2c_devices")
        if disable_marker and len(disable_marker.args) > 0:
            for device in disable_marker.args:
                cmd.append(f"--disable-i2c={device}")

        if pixel_format is not None:
            cmd.append(f"--display_pixel_format={PIXEL_FORMAT_BITMASK[pixel_format]}")

        if mono_vtiled is not None:
            cmd.append(f"--display_mono_vtiled={'true' if mono_vtiled else 'false'}")

        env = os.environ.copy()
        env.update(native_sim_env)

        capture_png_pattern = None
        if capture_times_ns is not None:
            if instance_count == 1:
                capture_png_pattern = str(tmp_path / "frame_%d.png")
            else:
                capture_png_pattern = str(tmp_path / f"frame-{i}_%d.png")
            cmd.append(f"--display_capture_png={capture_png_pattern}")

        logger.info("Running: %s", " ".join(cmd))
        proc = NativeSimProcess(cmd, timeout, trace_file, env, flash_file=flash)
        proc.display_dump = None
        proc._capture_png_pattern = capture_png_pattern
        proc._capture_count = len(capture_times_ns) if capture_times_ns is not None else 0
        proc.board_id = board
        procs.append(proc)

    if instance_count == 1:
        yield procs[0]
    else:
        yield procs

    for i, proc in enumerate(procs):
        if instance_count > 1:
            print(f"---------- Instance {i} -----------")
        proc.shutdown()
        print("All serial output:")
        print(proc.serial.all_output)
        print()
        print("All debug serial output:")
        print(proc.debug_serial.all_output)


@pytest.fixture(scope="session", autouse=True)
def _close_usbip_backends(request):
    """Session teardown: force-close every leaked USB/IP backend TCP socket."""
    gc.collect()
    yield
    _close_all_usbip_backends()


@pytest.fixture(scope="session")
def _hw_firmware_flashed(request):
    """Flash full CircuitPython firmware to every cached hardware board.

    Runs once per session.  Each per-test fixture then only refreshes
    the ``user_fs`` partition.

    Returns a set of board IDs that were successfully flashed (or are
    non-espressif boards that don't need flashing).
    """
    cache = _get_board_cache(request.config)
    flashed = set()
    for board_id in cache.list_cached():
        if not board_id.startswith("espressif_"):
            flashed.add(board_id)
            continue
        board_cfg = BOARD_CONFIG.get(board_id, {})
        firmware_path = _find_firmware(board_id)
        if firmware_path is None:
            raise FileNotFoundError(
                f"firmware.bin not found for {board_id} in any port build directory. "
                f"Build it first (e.g. with the espressif port Makefile) "
                f"to ensure the device firmware matches the current build."
            )
        entry = cache.get(board_id)
        assert entry is not None
        host = entry["host"]
        flash_vid = board_cfg.get("flash_vid")
        flash_pid = board_cfg.get("flash_pid")
        if flash_vid and flash_pid:
            flash_port = f"pyusb://{host}/{flash_vid:04x}:{flash_pid:04x}"
        else:
            dut_vid = board_cfg.get("dut_vid", 0x303A)
            dut_pid = board_cfg.get("dut_pid", 0x1001)
            flash_port = f"pyusb://{host}/{dut_vid:04x}:{dut_pid:04x}"
        logger.info("Flashing firmware for %s: %s", board_id, firmware_path)
        flash_firmware(flash_port, firmware_path, chip=board_cfg.get("chip", "auto"))
        flashed.add(board_id)
    return flashed


def _circuitpython_esp(
    request: pytest.FixtureRequest,
    board: str,
    tmp_path: Path,
):
    """ESP32 hardware-in-the-loop variant of the circuitpython fixture.

    Uses the board cache to find the device via USB/IP.  Skips if the
    board is not reachable.
    """
    cache = _get_board_cache(request.config)
    if not cache.ensure_accessible(board):
        pytest.skip(f"Board {board} not reachable via USB/IP")

    # Trigger session-level firmware flash once.
    print("[DEBUG _circuitpython_esp] Getting _hw_firmware_flashed fixture...")
    flashed = request.getfixturevalue("_hw_firmware_flashed")
    if board not in flashed:
        pytest.fail(
            f"firmware.bin for {board} was not flashed. "
            f"Build it first to ensure the device firmware matches the current build."
        )

    entry = cache.get(board)
    assert entry is not None
    board_cfg = BOARD_CONFIG.get(board, {})

    # Build pyusb:// URLs for the DUT and optional flash port.
    host = entry["host"]

    dut_vid = board_cfg.get("dut_vid", 0x303A)
    dut_pid = board_cfg.get("dut_pid", 0x1001)
    esp_port = f"pyusb://{host}/{dut_vid:04x}:{dut_pid:04x}"

    flash_port = esp_port
    flash_vid = board_cfg.get("flash_vid")
    flash_pid = board_cfg.get("flash_pid")
    if flash_vid and flash_pid:
        flash_port = f"pyusb://{host}/{flash_vid:04x}:{flash_pid:04x}"

    drives = list(request.node.iter_markers_with_node("circuitpy_drive"))
    if len(drives) != 1:
        pytest.skip("ESP hardware backend only supports single-instance tests")

    # Skip tests that require native_sim-only features.
    if request.node.get_closest_marker("input_trace") is not None:
        pytest.skip("input_trace not supported on ESP hardware")
    if request.node.get_closest_marker("display") is not None:
        pytest.skip("display capture not supported on ESP hardware")
    if request.node.get_closest_marker("disable_i2c_devices") is not None:
        pytest.skip("disable_i2c_devices not supported on ESP hardware")
    if request.node.get_closest_marker("flash_config") is not None:
        pytest.skip("flash_config not supported on ESP hardware")

    flash_size = board_cfg.get("flash_size", "4MB")
    uf2 = board_cfg.get("uf2", False)
    chip = board_cfg.get("chip", "auto")

    user_fs_offset, user_fs_size = find_user_fs_partition(flash_size, uf2)

    marker = request.node.get_closest_marker("duration")
    timeout = marker.args[0] if marker is not None else 30

    runs_marker = request.node.get_closest_marker("code_py_runs")
    code_py_runs = int(runs_marker.args[0]) if runs_marker is not None else 1

    flash = _create_flash_image(tmp_path, drives[0][1], user_fs_size, index=0)

    # Logic analyzer is built into the harness on the same bridge.
    logic_analyzer = _logic_analyzer_for(cache, board)
    trace_file = tmp_path / "trace-0.perfetto" if logic_analyzer is not None else None

    proc = EspProcess(
        port=esp_port,
        flash_file=flash,
        user_fs_offset=user_fs_offset,
        user_fs_size=user_fs_size,
        chip=chip,
        timeout=timeout,
        logic_analyzer=logic_analyzer,
        trace_file=trace_file,
        flash_port=flash_port,
    )
    proc._expected_runs = code_py_runs
    proc.board_id = board

    try:
        yield proc
    finally:
        try:
            print("All serial output:")
            if proc.serial is not None:
                print(proc.serial.all_output)
            else:
                print("(closed)")
        except Exception:
            pass
        proc.shutdown()
        if logic_analyzer is not None:
            try:
                logic_analyzer.close()
            except Exception:
                pass


def _close_all_usbip_backends():
    """Find every USBIPBackend still alive in the heap and close it.

    pyserial-pyusb Serial ↔ USBIPBackend ↔ _UsbipConnection form a
    reference cycle that the garbage collector must break before the
    backend's ``__del__`` fires.  Between tests the collector may not
    run, so callers should ``gc.collect()`` first, then this function.
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
        logger.info("Closed %d leaked USBIPBackend instance(s)", closed)
