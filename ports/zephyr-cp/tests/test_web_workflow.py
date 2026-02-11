# SPDX-FileCopyrightText: 2026 Scott Shawcroft for Adafruit Industries
# SPDX-License-Identifier: MIT

"""Tests for web workflow on native_sim."""

from __future__ import annotations

import base64
import os
import select
import socket
import subprocess
import time
from pathlib import Path

import pytest

from conftest import UART_PTY_RE, USB_PTY_RE


WEB_WORKFLOW_CODE = """\
import time

# Keep the VM alive while the web workflow starts.
time.sleep(3)
"""

WEB_WORKFLOW_UPDATED_CODE = """\
print("updated")
"""

WEB_WORKFLOW_SETTINGS = """\
CIRCUITPY_WEB_API_PASSWORD="testpass"
"""

WEB_WORKFLOW_BOOT = """\
import storage

storage.remount("/", readonly=False)
"""


def _get_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def _start_native_sim(
    binary: Path,
    flash: Path,
    timeout: float,
    trace_file: Path,
    env: dict,
    *,
    wait_for_usb: bool = False,
) -> tuple[subprocess.Popen, int, int | None]:
    cmd = [
        str(binary),
        f"--flash={flash}",
        "--flash_rm",
        "-no-rt",
        "-wait_uart",
        f"-stop_at={timeout}",
        f"--trace-file={trace_file}",
    ]

    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=env,
    )

    uart_path = None
    usb_path = None
    start_time = time.time()
    while time.time() - start_time < timeout + 5:
        if proc.poll() is not None:
            break
        ready, _, _ = select.select([proc.stdout], [], [], 0.1)
        if not ready:
            continue
        line = proc.stdout.readline()
        if not line:
            break
        uart_match = UART_PTY_RE.search(line)
        if uart_match:
            uart_path = uart_match.group("path")
        usb_match = USB_PTY_RE.search(line)
        if usb_match:
            usb_path = usb_match.group("path")
        if uart_path and (usb_path or not wait_for_usb):
            break

    if uart_path is None or (wait_for_usb and usb_path is None):
        proc.terminate()
        missing = "UART"
        if wait_for_usb:
            missing = "UART/USB"
        pytest.fail(f"Failed to find {missing} PTY path in native_sim output")

    uart_fd = os.open(uart_path, os.O_RDWR | os.O_NONBLOCK)
    usb_fd = os.open(usb_path, os.O_RDWR | os.O_NONBLOCK) if usb_path else None
    return proc, uart_fd, usb_fd


def _read_http_response(sock: socket.socket) -> tuple[str, bytes]:
    sock.settimeout(1.0)
    response = b""
    while b"\r\n\r\n" not in response:
        chunk = sock.recv(1024)
        if not chunk:
            break
        response += chunk

    if b"\r\n\r\n" not in response:
        return response.decode("utf-8", errors="replace"), b""

    header_bytes, body = response.split(b"\r\n\r\n", 1)
    header_text = header_bytes.decode("utf-8", errors="replace")
    content_length = None
    for line in header_text.split("\r\n"):
        if line.lower().startswith("content-length:"):
            content_length = int(line.split(":", 1)[1].strip())
            break

    if content_length is not None:
        while len(body) < content_length:
            chunk = sock.recv(1024)
            if not chunk:
                break
            body += chunk
    return header_text, body


def _send_request(port: int, request: bytes, deadline: float) -> tuple[str, bytes] | None:
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5) as sock:
                sock.sendall(request)
                return _read_http_response(sock)
        except (ConnectionRefusedError, OSError, socket.timeout):
            time.sleep(0.1)
    return None


@pytest.mark.usefixtures("native_sim_binary", "create_flash_image")
def test_web_workflow_hostnetwork(native_sim_binary, create_flash_image, tmp_path):
    """Ensure web workflow responds over hostnetwork."""
    flash = create_flash_image({"code.py": WEB_WORKFLOW_CODE})
    trace_file = tmp_path / "trace-web-workflow.perfetto"
    port = _get_free_port()

    env = os.environ.copy()
    env["CIRCUITPY_WEB_API_PASSWORD"] = "testpass"
    env["CIRCUITPY_WEB_API_PORT"] = str(port)

    proc, pty_fd, _ = _start_native_sim(native_sim_binary, flash, 4.0, trace_file, env)
    try:
        response = None
        deadline = time.time() + 3.0
        while time.time() < deadline:
            if proc.poll() is not None:
                break
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=0.5) as sock:
                    sock.sendall(b"GET /edit/ HTTP/1.1\r\nHost: localhost\r\n\r\n")
                    response = sock.recv(1024).decode("utf-8", errors="replace")
                    break
            except (ConnectionRefusedError, OSError):
                time.sleep(0.1)

        assert response is not None, "Web workflow did not accept a connection"
        assert "401 Unauthorized" in response
    finally:
        os.close(pty_fd)
        proc.terminate()
        proc.wait(timeout=1)


@pytest.mark.usefixtures("native_sim_binary", "create_flash_image")
def test_web_workflow_write_code_py_conflict(native_sim_binary, create_flash_image, tmp_path):
    """Ensure web workflow reports a conflict when USB has the drive."""
    flash = create_flash_image(
        {
            "code.py": WEB_WORKFLOW_CODE,
            "settings.toml": WEB_WORKFLOW_SETTINGS,
        }
    )
    trace_file = tmp_path / "trace-web-workflow-conflict.perfetto"
    port = _get_free_port()

    env = os.environ.copy()
    env["CIRCUITPY_WEB_API_PORT"] = str(port)

    proc, pty_fd, _ = _start_native_sim(native_sim_binary, flash, 6.0, trace_file, env)
    try:
        token = base64.b64encode(b":testpass").decode("utf-8")
        auth_header = f"Authorization: Basic {token}\r\n"
        delete_request = (
            f"DELETE /fs/code.py HTTP/1.1\r\nHost: localhost\r\n{auth_header}\r\n"
        ).encode("utf-8")

        response = _send_request(port, delete_request, time.time() + 3.0)
        assert response is not None, "Web workflow did not accept DELETE request"
        header, _ = response
        assert "409 Conflict" in header
    finally:
        os.close(pty_fd)
        proc.terminate()
        proc.wait(timeout=1)


@pytest.mark.usefixtures("native_sim_binary", "create_flash_image")
def test_web_workflow_write_code_py_remount(native_sim_binary, create_flash_image, tmp_path):
    """Ensure web workflow can update code.py after remounting."""
    flash = create_flash_image(
        {
            "boot.py": WEB_WORKFLOW_BOOT,
            "code.py": WEB_WORKFLOW_CODE,
            "settings.toml": WEB_WORKFLOW_SETTINGS,
        }
    )
    trace_file = tmp_path / "trace-web-workflow-write.perfetto"
    port = _get_free_port()

    env = os.environ.copy()
    env["CIRCUITPY_WEB_API_PORT"] = str(port)

    proc, pty_fd, _ = _start_native_sim(native_sim_binary, flash, 6.0, trace_file, env)
    try:
        token = base64.b64encode(b":testpass").decode("utf-8")
        auth_header = f"Authorization: Basic {token}\r\n"
        body = WEB_WORKFLOW_UPDATED_CODE.encode("utf-8")
        put_request = (
            "PUT /fs/code.py HTTP/1.1\r\n"
            "Host: localhost\r\n"
            f"{auth_header}"
            f"Content-Length: {len(body)}\r\n"
            "\r\n"
        ).encode("utf-8") + body

        response = _send_request(port, put_request, time.time() + 3.0)
        assert response is not None, "Web workflow did not accept PUT request"
        header, _ = response
        assert "204 No Content" in header or "201 Created" in header

        get_request = (f"GET /fs/code.py HTTP/1.1\r\nHost: localhost\r\n{auth_header}\r\n").encode(
            "utf-8"
        )
        response = _send_request(port, get_request, time.time() + 2.0)
        assert response is not None, "Web workflow did not respond to GET request"
        header, body = response
        assert "200 OK" in header
        assert WEB_WORKFLOW_UPDATED_CODE in body.decode("utf-8", errors="replace")
    finally:
        os.close(pty_fd)
        proc.terminate()
        proc.wait(timeout=1)
