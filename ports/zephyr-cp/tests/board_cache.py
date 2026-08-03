# SPDX-FileCopyrightText: 2025 Scott Shawcroft for Adafruit Industries
# SPDX-License-Identifier: MIT

"""Board cache: maps board_id → .local hostname for hardware-in-the-loop testing.

CLI usage (run from the *ports/zephyr-cp* directory)::

    python -m tests.board_cache list [--verbose]
    python -m tests.board_cache discover [board_id]
    python -m tests.board_cache check <board_id>
    python -m tests.board_cache add <board_id> <host> [port]
    python -m tests.board_cache remove <board_id>
"""

from __future__ import annotations

import json
import logging
import sys
import time
from pathlib import Path

try:
    from usbip_backend import discover_usbip_servers, get_backend as _get_usbip_backend
except ImportError:  # pragma: no cover
    discover_usbip_servers = None
    _get_usbip_backend = None

logger = logging.getLogger(__name__)

# ===== BOARD CONFIGURATION =====

# Maps board_id to hardware properties for testing.
# Native sim boards (native_*) don't need entries here.
BOARD_CONFIG: dict[str, dict] = {
    "espressif_esp32c61_devkitc_1_n8r2": {
        "chip": "esp32c61",
        "flash_size": "8MB",
        "uf2": False,
        "dut_vid": 0x303A,  # Espressif USB JTAG/Serial
        "dut_pid": 0x1001,
        "flash_vid": 0x10C4,  # CP2102N on-board USB-UART
        "flash_pid": 0xEA60,
    },
}

# Default cache file path (relative to this file's directory).
DEFAULT_CACHE_PATH = Path(__file__).parent / ".board_cache.json"


# ===== BOARD CACHE =====


class BoardCache:
    """Persistent cache mapping board_id to .local hostname.

    Cache file format (JSON)::

        {
          "espressif_esp32c61_devkitc_1_n8r2": {
            "host": "usbip-aabbcc.local",
            "port": 3240,
            "last_seen": 1700000000
          }
        }

    Parameters:
        path: Path to the JSON cache file.
    """

    def __init__(self, path: Path) -> None:
        self._path = path
        self._data: dict[str, dict] = {}
        if self._path.exists():
            try:
                self._data = json.loads(self._path.read_text())
            except (json.JSONDecodeError, OSError):
                self._data = {}

    # -- public read-only helpers ----------------------------------------

    def get(self, board_id: str) -> dict | None:
        """Return cached ``{host, port, last_seen}`` or *None*."""
        return self._data.get(board_id)

    def list_cached(self) -> list[str]:
        """Return *board_ids* present in the cache."""
        return list(self._data.keys())

    @property
    def path(self) -> Path:
        """Path to the backing JSON file."""
        return self._path

    # -- persistence -----------------------------------------------------

    def _save(self) -> None:
        self._path.parent.mkdir(parents=True, exist_ok=True)
        self._path.write_text(json.dumps(self._data, indent=2))

    def _remove(self, board_id: str) -> None:
        if board_id in self._data:
            del self._data[board_id]
            self._save()

    def _find_board_by_host(self, host: str) -> str | None:
        """Return the *board_id* that already uses *host*, or *None*."""
        for bid, entry in self._data.items():
            if entry.get("host") == host:
                return bid
        return None

    def put(self, board_id: str, host: str, port: int = 3240) -> None:
        """Add or update a cache entry, evicting any other entry for *host*."""
        # Only one entry per .local address.
        existing = self._find_board_by_host(host)
        if existing is not None and existing != board_id:
            del self._data[existing]
        self._data[board_id] = {
            "host": host,
            "port": port,
            "last_seen": int(time.time()),
        }
        self._save()

    def remove(self, board_id: str) -> bool:
        """Remove *board_id* from the cache.  Returns *True* if it existed."""
        if board_id in self._data:
            del self._data[board_id]
            self._save()
            return True
        return False

    # -- connectivity ----------------------------------------------------

    def is_accessible(self, board_id: str) -> bool:
        """Quick check: can we list devices on the cached host?"""
        entry = self._data.get(board_id)
        if entry is None:
            return False
        if _get_usbip_backend is None:
            return False
        try:
            backend = _get_usbip_backend(entry["host"], entry.get("port", 3240), timeout=2.0)
            try:
                devices = list(backend.enumerate_devices())
                return len(devices) > 0
            finally:
                backend.close()
        except Exception as e:
            logger.debug("Board %s not reachable at %s: %s", board_id, entry["host"], e)
            return False

    def discover(self, board_id: str) -> dict | None:
        """Run mDNS to find *board_id*; return entry dict or *None*.

        The returned dict has keys ``host`` and ``port``.
        """
        all_found = self.discover_all()
        return all_found.get(board_id)

    def discover_all(self) -> dict[str, dict]:
        """Run a single mDNS scan and find **all** boards on the network.

        Connects to each discovered USB/IP server once, enumerates all
        devices, and matches them against every :data:`BOARD_CONFIG` entry
        by VID/PID.  Every match is automatically cached.

        Returns:
            Dict mapping ``board_id`` → ``{"host": …, "port": …}``.
        """
        if discover_usbip_servers is None or _get_usbip_backend is None:
            logger.debug("usbip_backend not available, skipping discovery.")
            return {}

        # Build reverse lookup: (vid, pid) → board_id
        vid_pid_to_board: dict[tuple[int, int], str] = {}
        for bid, cfg in BOARD_CONFIG.items():
            vid = cfg.get("dut_vid")
            pid = cfg.get("dut_pid")
            if vid is not None:
                vid_pid_to_board[(vid, pid)] = bid

        found: dict[str, dict] = {}
        claimed_hosts: set[str] = set()  # only one entry per .local host

        services = discover_usbip_servers(timeout=3.0)
        for svc in services:
            host = svc["host"].rstrip(".")
            address = svc["address"]
            port = svc["port"]

            hostname = host if host.endswith(".local") else host + ".local"
            if hostname in claimed_hosts:
                continue

            try:
                backend = _get_usbip_backend(address, port, timeout=2.0)
            except Exception:
                continue
            try:
                devices = list(backend.enumerate_devices())
            except Exception:
                backend.close()
                continue
            finally:
                backend.close()

            for dev in devices:
                # Match by VID/PID against all known board configs.
                board_id = vid_pid_to_board.get((dev.id_vendor, dev.id_product))
                if board_id is not None:
                    logger.info(
                        "mDNS: discovered %s at %s (%s:%d)",
                        board_id,
                        hostname,
                        address,
                        port,
                    )
                    entry = {"host": hostname, "port": port}
                    found[board_id] = entry
                    self.put(board_id, hostname, port)
                    claimed_hosts.add(hostname)
                    break  # one board per host

        return found

    def ensure_accessible(self, board_id: str) -> bool:
        """Full accessibility check with mDNS fallback.

        1. If cached and reachable → return *True*.
        2. If cached but unreachable → remove, rediscover.
        3. Run mDNS discovery → cache & return *True* if found.
        4. Return *False* → test should skip this board.
        """
        if self.is_accessible(board_id):
            return True

        if board_id in self._data:
            logger.info("Removing stale cache entry for %s", board_id)
            self._remove(board_id)

        entry = self.discover(board_id)
        if entry is not None:
            self.put(board_id, entry["host"], entry.get("port", 3240))
            return True

        return False


# ===== CLI =====


def _cli_list(cache: BoardCache, verbose: bool = False) -> int:
    """``list`` subcommand."""
    boards = cache.list_cached()
    if not boards:
        print("No boards cached.")
        return 0

    for bid in sorted(boards):
        entry = cache.get(bid)
        assert entry is not None
        host = entry.get("host", "?")
        port = entry.get("port", 3240)
        last = entry.get("last_seen", 0)
        ago = int(time.time() - last)
        reachable = "✓" if cache.is_accessible(bid) else "✗"
        print(f"  {reachable} {bid}")
        print(f"      host: {host}:{port}  (last seen {ago}s ago)")
        if verbose:
            cfg = BOARD_CONFIG.get(bid, {})
            if cfg:
                print(
                    f"      chip: {cfg.get('chip', '?')}  "
                    f"flash: {cfg.get('flash_size', '?')}  "
                    f"uf2: {cfg.get('uf2', False)}"
                )
    return 0


def _cli_discover(cache: BoardCache, board_id: str | None = None) -> int:
    """``discover`` subcommand."""
    if board_id:
        # Single-board targeted discovery.
        print(f"Discovering {board_id} ... ", end="", flush=True)
        entry = cache.discover(board_id)
        if entry is not None:
            cache.put(board_id, entry["host"], entry.get("port", 3240))
            print(f"found at {entry['host']}:{entry.get('port', 3240)}")
            return 0
        else:
            print("not found")
            return 1

    # Discover all boards on the network in a single mDNS scan.
    print("Scanning network for USB/IP servers …")
    all_found = cache.discover_all()
    if not all_found:
        print("No boards discovered.")
        return 1

    for bid, entry in all_found.items():
        print(f"  {bid}: {entry['host']}:{entry.get('port', 3240)}")
    print(f"\n{len(all_found)} board(s) discovered.")
    return 0


def _cli_check(cache: BoardCache, board_id: str) -> int:
    """``check`` subcommand.  Exit code 0 = accessible, 1 = not."""
    print(f"Checking {board_id} ... ", end="", flush=True)
    if cache.ensure_accessible(board_id):
        entry = cache.get(board_id)
        assert entry is not None
        print(f"accessible at {entry['host']}:{entry.get('port', 3240)}")
        return 0
    else:
        print("not reachable")
        return 1


def _cli_add(cache: BoardCache, board_id: str, host: str, port: int = 3240) -> int:
    """``add`` subcommand."""
    cache.put(board_id, host, port)
    print(f"Added {board_id} → {host}:{port}")
    return 0


def _cli_remove(cache: BoardCache, board_id: str) -> int:
    """``remove`` subcommand."""
    if cache.remove(board_id):
        print(f"Removed {board_id} from cache.")
        return 0
    else:
        print(f"{board_id} not in cache.")
        return 1


def main(argv: list[str] | None = None) -> int:
    """Entry point for ``python -m tests.board_cache``."""
    import argparse

    parser = argparse.ArgumentParser(
        description="Manage the board discovery cache.",
    )
    parser.add_argument(
        "--cache",
        default=str(DEFAULT_CACHE_PATH),
        help=f"Path to cache file (default: {DEFAULT_CACHE_PATH})",
    )
    parser.add_argument("-v", "--verbose", action="store_true", help="Verbose output.")

    sub = parser.add_subparsers(dest="command", metavar="COMMAND")
    sub.add_parser("list", help="List cached boards and their status.")

    disc = sub.add_parser("discover", help="mDNS-discover boards and update cache.")
    disc.add_argument("board_id", nargs="?", default=None, help="Specific board_id.")

    chk = sub.add_parser("check", help="Check if a board is accessible.")
    chk.add_argument("board_id", help="Board to check.")

    add = sub.add_parser("add", help="Add a board to the cache manually.")
    add.add_argument("board_id", help="Board id.")
    add.add_argument("host", help=".local hostname or IP address.")
    add.add_argument("port", nargs="?", type=int, default=3240, help="USB/IP port.")

    rem = sub.add_parser("remove", help="Remove a board from the cache.")
    rem.add_argument("board_id", help="Board id.")

    args = parser.parse_args(argv)

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.WARNING,
        format="%(levelname)s: %(message)s",
    )

    cache = BoardCache(Path(args.cache))

    if args.command == "list":
        return _cli_list(cache, verbose=args.verbose)
    elif args.command == "discover":
        return _cli_discover(cache, args.board_id)
    elif args.command == "check":
        return _cli_check(cache, args.board_id)
    elif args.command == "add":
        return _cli_add(cache, args.board_id, args.host, args.port)
    elif args.command == "remove":
        return _cli_remove(cache, args.board_id)
    else:
        parser.print_help()
        return 2


if __name__ == "__main__":
    sys.exit(main())
