#!/usr/bin/env python3
"""Visualize nonvolatile memory layout and partition schemes for CircuitPython Zephyr boards.

Runs a cmake-only west build to generate the resolved devicetree, then parses
the edt.pickle for flash devices, erase page sizes, and partition layouts.

Usage:
    python3 cptools/bootloaderify.py <board_id>          # show layout
    python3 cptools/bootloaderify.py --fix <board_id>    # write aligned partitions dtsi
"""

import argparse
import pathlib
import pickle
import subprocess
import sys
import tomllib

KB = 1024
MB = 1024 * 1024

BAR_WIDTH = 72

PORT_DIR = pathlib.Path(__file__).resolve().parent.parent
EDT_MODULE = PORT_DIR / "zephyr" / "scripts" / "dts" / "python-devicetree" / "src"

FILL = {
    "mcuboot": ("B", "mcuboot"),
    "image-0": ("0", "image-0 (app slot)"),
    "image-1": ("1", "image-1 (update slot)"),
    "storage": ("S", "storage (settings)"),
    "nvm": ("N", "nvm (non-volatile memory)"),
    "circuitpy": ("#", "circuitpy (filesystem)"),
    "free": (".", "free / unallocated"),
}

ZEPHYR_BASE = PORT_DIR / "zephyr"


def discover_boards():
    """Find all boards with circuitpython.toml that have adaboot enabled.

    Returns sorted list of board_ids. Boards with adaboot = false are excluded.
    """
    boards = []
    for toml_path in (PORT_DIR / "boards").glob("*/*/circuitpython.toml"):
        parts = toml_path.relative_to(PORT_DIR / "boards").parts
        if len(parts) == 3:
            with open(toml_path, "rb") as f:
                toml_data = tomllib.load(f)
            if toml_data.get("adaboot", True):
                boards.append(f"{parts[0]}_{parts[1]}")
    return sorted(boards)


def load_board_toml(board_id):
    """Load circuitpython.toml for a board_id (e.g. 'nordic_nrf54lm20dk')."""
    boards_dir = PORT_DIR / "boards"
    next_underscore = board_id.find("_")
    while next_underscore != -1:
        vendor = board_id[:next_underscore]
        board = board_id[next_underscore + 1 :]
        toml_path = boards_dir / vendor / board / "circuitpython.toml"
        if toml_path.exists():
            with open(toml_path, "rb") as f:
                return tomllib.load(f)
        next_underscore = board_id.find("_", next_underscore + 1)
    return {}


def cmake_only_build(board_id, build_dir):
    """Run west build --cmake-only to generate the devicetree."""
    build_dir.mkdir(parents=True, exist_ok=True)

    subprocess.run(
        [sys.executable, str(PORT_DIR / "cptools" / "pre_zephyr_build_prep.py"), board_id],
        cwd=PORT_DIR,
        check=True,
        capture_output=True,
    )

    cmd = [
        "west",
        "build",
        "-b",
        board_id,
        "-d",
        str(build_dir),
        "--cmake-only",
        "--",
        f"-DZEPHYR_BOARD_ALIASES={PORT_DIR / 'boards' / 'board_aliases.cmake'}",
    ]
    result = subprocess.run(cmd, cwd=PORT_DIR, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"Build failed:\n{result.stderr}", file=sys.stderr)
        sys.exit(1)


def load_edt(build_dir):
    """Load the pickled EDT from a build directory."""
    sys.path.insert(0, str(EDT_MODULE))
    edt_path = build_dir / "zephyr" / "edt.pickle"
    if not edt_path.exists():
        print(f"edt.pickle not found at {edt_path}", file=sys.stderr)
        sys.exit(1)
    with open(edt_path, "rb") as f:
        return pickle.load(f)


def get_erase_size(node):
    """Get erase block size from a flash device node."""
    erase_size_prop = node.props.get("erase-block-size")
    if erase_size_prop:
        return erase_size_prop.val
    if "pages_layout" in node.children:
        erase_size = 0
        pl = node.children["pages_layout"]
        for layout_name, layout_node in pl.children.items():
            ps = layout_node.props.get("pages-size")
            if ps and ps.val > erase_size:
                erase_size = ps.val
        if erase_size > 0:
            return erase_size
    return 4096


def get_total_size(node):
    """Get total flash size from a device node."""
    size_prop = node.props.get("size")
    if size_prop:
        return size_prop.val // 8
    reg = node.props.get("reg")
    if reg and len(reg.val) >= 2:
        return reg.val[1]
    return 0


def is_internal_flash(node):
    """Check if a flash node is internal (SoC-defined) flash.

    SoC flash is defined in zephyr/dts/ (SoC dtsi files).
    Board flash is defined in zephyr/boards/ (board dts files).
    """
    filename = getattr(node, "filename", "")
    if not filename:
        return False
    path = pathlib.Path(filename)
    try:
        rel = path.relative_to(ZEPHYR_BASE)
        # SoC dtsi files live under dts/, board files under boards/
        return rel.parts[0] == "dts"
    except ValueError:
        return False


# Partition tuple: (label, node_label, offset, size)
# Device tuple: (dev_label, total_size, erase_size, [partitions], is_internal)


def extract_flash_devices(edt, flash_overrides=None):
    """Extract all flash devices from the EDT, with their partitions (if any)."""
    # Build a map of partitions keyed by device label
    partitions_by_label = {}
    for node in edt.nodes:
        if not hasattr(node, "children") or "partitions" not in node.children:
            continue
        partitions_node = node.children["partitions"]
        if "fixed-partitions" not in getattr(partitions_node, "compats", []):
            continue
        dev_label = node.labels[0] if node.labels else node.name
        parts = []
        for pname, pnode in partitions_node.children.items():
            label_prop = pnode.props.get("label")
            reg_prop = pnode.props.get("reg")
            if label_prop and reg_prop:
                node_label = pnode.labels[0] if pnode.labels else pname
                parts.append((label_prop.val, node_label, reg_prop.val[0], reg_prop.val[1]))
        partitions_by_label[dev_label] = parts

    # Return all flash devices, attaching partitions where they exist
    devices = []
    for dev_label, total_size, erase_size, internal in discover_all_flash(edt, flash_overrides):
        parts = partitions_by_label.get(dev_label, [])
        devices.append((dev_label, total_size, erase_size, parts, internal))
    return devices


def discover_all_flash(edt, flash_overrides=None):
    """Find all flash device nodes in the EDT, even those without partitions.

    flash_overrides is an optional dict mapping device labels to
    {"erase_block_size": int} from circuitpython.toml [external_flash].

    Returns list of (dev_label, total_size, erase_size, is_internal).
    """
    if flash_overrides is None:
        flash_overrides = {}
    override_labels = set(flash_overrides.keys())

    devices = []
    for node in edt.nodes:
        dev_label = node.labels[0] if node.labels else node.name
        has_erase = node.props.get("erase-block-size") or "pages_layout" in getattr(
            node, "children", {}
        )
        has_override = dev_label in override_labels
        if not has_erase and not has_override:
            continue
        # Must have a size
        total_size = get_total_size(node)
        if total_size == 0:
            continue
        # Skip flash controllers (they wrap the actual flash node)
        if any("controller" in c for c in node.compats):
            continue
        if has_override:
            erase_size = flash_overrides[dev_label]["erase_block_size"]
        else:
            erase_size = get_erase_size(node)
        internal = is_internal_flash(node)
        devices.append((dev_label, total_size, erase_size, internal))
    return devices


# ── Rendering ────────────────────────────────────────────────────────────


def make_partitions_with_gaps(parts, total_size):
    parts = sorted(parts, key=lambda p: p[2])  # sort by offset
    result = []
    cursor = 0
    for label, node_label, offset, size in parts:
        if offset > cursor:
            result.append(("free", cursor, offset - cursor))
        result.append((label, offset, size))
        cursor = offset + size
    if cursor < total_size:
        result.append(("free", cursor, total_size - cursor))
    return result


def format_size(size_bytes):
    if size_bytes >= MB:
        val = size_bytes / MB
        return f"{val:.1f} MB" if val != int(val) else f"{int(val)} MB"
    val = size_bytes / KB
    return f"{val:.0f} KB" if val == int(val) else f"{val:.1f} KB"


def render_bar(parts, total_size):
    full = make_partitions_with_gaps(parts, total_size)
    bar = []
    for label, offset, size in full:
        char = FILL.get(label, ("?", ""))[0]
        cols = max(1, round(size / total_size * BAR_WIDTH))
        bar.append(char * cols)
    line = "".join(bar)
    return line[:BAR_WIDTH].ljust(BAR_WIDTH)


def render_detail_lines(parts, total_size, erase_size):
    full = make_partitions_with_gaps(parts, total_size)
    lines = []
    for label, offset, size in full:
        if label == "free" and size < 1024:
            continue
        char = FILL.get(label, ("?", ""))[0]
        end = offset + size - 1
        pages = size / erase_size
        pages_str = f"{int(pages)} pgs" if pages == int(pages) else f"{pages:.1f} pgs"
        warn = ""
        if offset % erase_size != 0:
            warn += " !offset"
        if size % erase_size != 0:
            warn += " !size"
        lines.append(
            f"    [{char}] 0x{offset:08X}..0x{end:08X}  {format_size(size):>10s}  {pages_str:>8s}  {label}{warn}"
        )
    return lines


def show_layout(devices):
    print("  Legend:")
    for key, (char, desc) in FILL.items():
        print(f"    {char}  {desc}")
    print()

    for dev_label, total_size, erase_size, parts, internal in devices:
        kind = "internal" if internal else "external"
        name = f"{dev_label} ({format_size(total_size)}, {kind})"
        print(f"  {name}  erase page: {format_size(erase_size)}")
        bar = render_bar(parts, total_size)
        print(f"  |{bar}|")
        for line in render_detail_lines(parts, total_size, erase_size):
            print(f"  {line}")
        print()


# ── Fix mode ─────────────────────────────────────────────────────────────


def align_up(val, alignment):
    return ((val + alignment - 1) // alignment) * alignment


def align_down(val, alignment):
    return (val // alignment) * alignment


def format_dt_size(size_bytes):
    """Format a size as a DT_SIZE_K() or DT_SIZE_M() macro call."""
    if size_bytes % MB == 0:
        return f"DT_SIZE_M({size_bytes // MB})"
    if size_bytes % KB == 0:
        return f"DT_SIZE_K({size_bytes // KB})"
    return f"0x{size_bytes:x}"


def _has_predefined_mcuboot(edt, flash_overrides=None):
    """Check if any flash device already has mcuboot partitions defined upstream.

    Returns the existing partitions by device label if mcuboot is found, else None.
    """
    existing = extract_flash_devices(edt, flash_overrides)
    for dev_label, total_size, erase_size, parts, internal in existing:
        labels = {p[0] for p in parts}
        if "mcuboot" in labels and "image-0" in labels:
            return existing
    return None


CP_PARTITION_LABELS = {"circuitpy", "nvm"}


def plan_partitions_predefined(edt, flash_overrides=None):
    """Plan partitions for boards with a predefined mcuboot layout.

    When external flash is available, slot1 is moved to external flash and
    slot0 is grown to fill the freed internal space.  Layout:
        Internal: mcuboot + slot0 (grown) + storage
        External: nvm + slot1 (slot0 size + 1 max-erase sector) + circuitpy

    When no external flash is available, upstream partitions are kept and
    nvm/circuitpy are added to the remaining free space on internal flash.

    Returns list of (dev_label, total_size, erase_size, [(label, node_label, offset, size)],
    predefined_labels) where predefined_labels is the set of partition labels from the
    upstream DTS (so the dtsi generator can skip them).
    """
    devices = extract_flash_devices(edt, flash_overrides)
    all_flash = discover_all_flash(edt, flash_overrides)
    data_flash = [(l, s, e) for l, s, e, i in all_flash if i and s < 64 * KB]
    external = [(l, s, e) for l, s, e, i in all_flash if not i and s >= 1 * MB]

    result = []
    for dev_label, total_size, erase_size, parts, internal in devices:
        existing_labels = {p[0] for p in parts}

        # If this device has no mcuboot partitions, pass it through unchanged
        # (or skip if empty).
        if "mcuboot" not in existing_labels:
            if parts:
                upstream = [p for p in parts if p[0] not in CP_PARTITION_LABELS]
                predefined = {p[0] for p in upstream}
                result.append((dev_label, total_size, erase_size, list(upstream), predefined))
            continue

        if external:
            # ── Move slot1 to external flash, grow slot0 ──
            ext_label, ext_size, ext_erase = external[0]
            max_erase = max(erase_size, ext_erase)

            # Find the predefined mcuboot partition and ensure it's at
            # least MCUBOOT_SIZE since we're regenerating all partitions.
            boot = [p for p in parts if p[0] == "mcuboot"][0]
            boot_size = max(boot[3], align_up(128 * KB, erase_size))
            boot_end = boot[2] + boot_size

            # Find the predefined storage partition (if any).
            storage_parts = [p for p in parts if p[0] == "storage"]
            storage_size = storage_parts[0][3] if storage_parts else erase_size

            # Place NVM on internal flash if internal is RRAM (good
            # endurance, small erase pages) or if data flash exists.
            nvm_on_internal = "rram" in dev_label or data_flash
            nvm_size = erase_size if nvm_on_internal and not data_flash else 0

            # Place storage + nvm at the end of internal flash, then grow
            # slot0 to fill the remaining space.
            tail_size = storage_size + nvm_size
            tail_start = align_down(total_size - tail_size, erase_size)
            slot0_offset = align_up(boot_end, erase_size)
            slot0_size = align_down(tail_start - slot0_offset, max_erase)

            int_parts = [
                ("mcuboot", "boot_partition", boot[2], boot_size),
                ("image-0", "slot0_partition", slot0_offset, slot0_size),
            ]

            cursor = slot0_offset + slot0_size
            if nvm_size > 0:
                nvm_offset = align_up(cursor, erase_size)
                int_parts.append(("nvm", "nvm_partition", nvm_offset, nvm_size))
                cursor = nvm_offset + nvm_size

            storage_offset = align_up(cursor, erase_size)
            int_parts.append(("storage", "storage_partition", storage_offset, storage_size))

            # All partitions must be regenerated since the upstream partitions
            # node is deleted to allow slot0 to grow.
            result.append((dev_label, total_size, erase_size, int_parts, set()))

            # External: slot1 + circuitpy
            # slot1 needs slot0_size + 1 max-erase sector for mcuboot
            # swap-using-offset scratch area.
            slot1_size = slot0_size + max_erase
            ext_parts = []
            cursor = 0

            if not nvm_on_internal and not data_flash:
                nvm_ext_size = ext_erase
                ext_parts.append(("nvm", "nvm_partition", cursor, nvm_ext_size))
                cursor = align_up(cursor + nvm_ext_size, ext_erase)

            slot1_offset = align_up(cursor, ext_erase)
            ext_parts.append(("image-1", "slot1_partition", slot1_offset, slot1_size))
            cursor = align_up(slot1_offset + slot1_size, ext_erase)

            circuitpy_size = align_down(ext_size - cursor, ext_erase)
            if circuitpy_size > 0:
                ext_parts.append(("circuitpy", "circuitpy_partition", cursor, circuitpy_size))

            result.append((ext_label, ext_size, ext_erase, ext_parts, set()))
        else:
            # ── No external flash: keep upstream layout, add CP partitions ──
            upstream = [p for p in parts if p[0] not in CP_PARTITION_LABELS]
            predefined = {p[0] for p in upstream}
            kept = sorted(upstream, key=lambda p: p[2])

            # Find the end of the last upstream partition.
            last_end = max(p[2] + p[3] for p in kept)

            # Add nvm + circuitpy in the free space after the last partition.
            cursor = align_up(last_end, erase_size)

            if not data_flash:
                nvm_size = erase_size
                if cursor + nvm_size <= total_size:
                    kept.append(("nvm", "nvm_partition", cursor, nvm_size))
                    cursor = align_up(cursor + nvm_size, erase_size)

            circuitpy_size = align_down(total_size - cursor, erase_size)
            if circuitpy_size > 0:
                kept.append(("circuitpy", "circuitpy_partition", cursor, circuitpy_size))

            result.append((dev_label, total_size, erase_size, kept, predefined))

    # If data flash exists, use it entirely for NVM.
    result_labels = {d[0] for d in result}
    if data_flash:
        for df_label, df_size, df_erase in data_flash:
            if df_label in result_labels:
                continue
            nvm_size = align_down(df_size, df_erase)
            if nvm_size > 0:
                result.append(
                    (df_label, df_size, df_erase, [("nvm", "nvm_partition", 0, nvm_size)], set())
                )

    return result


def plan_partitions(edt, flash_overrides=None):
    """Determine the partition layout based on available flash devices.

    If the board already has a predefined mcuboot layout (from the upstream
    board DTS), the existing partitions are kept and nvm/circuitpy are added
    to the free space.

    Otherwise, partitions are planned from scratch:
    - If internal flash exists and there is also external flash:
        Internal: mcuboot + slot0 (fills remaining internal flash)
        External: storage (1 erase page) + slot1 (same size as slot0) + circuitpy (rest)
    - If internal flash exists with no external flash:
        Internal: mcuboot + slot0 + storage (1 erase page) + circuitpy (rest)
        No slot1 (no OTA update support).
    - If only external flash (XIP, e.g. FlexSPI):
        External: mcuboot + slot0 + slot1 (same size as slot0) + circuitpy (rest)
        No storage partition.

    NVM partition placement:
    - If small internal data flash exists (< 64 KB, e.g. RA6/RA8 data flash),
      NVM uses the entire data flash device (ideal: small erase pages).
    - Otherwise, NVM gets 1 erase page on the same device as storage.

    Returns list of (dev_label, total_size, erase_size, [(label, node_label, offset, size)]).
    """
    # Check for predefined mcuboot layout first.
    if _has_predefined_mcuboot(edt, flash_overrides):
        return plan_partitions_predefined(edt, flash_overrides)

    all_flash = discover_all_flash(edt, flash_overrides)
    if not all_flash:
        return []

    # Separate small internal data flash (< 64 KB) from main internal flash.
    # Data flash (e.g. RA6/RA8 flash1) has small erase pages ideal for NVM.
    data_flash = [(l, s, e) for l, s, e, i in all_flash if i and s < 64 * KB]
    internal = [(l, s, e) for l, s, e, i in all_flash if i and s >= 64 * KB]
    # Filter out tiny external flash regions
    external = [(l, s, e) for l, s, e, i in all_flash if not i and s >= 1 * MB]

    MCUBOOT_SIZE = 128 * KB
    result = []

    if internal and external:
        # ── Internal + External ──
        int_label, int_size, int_erase = internal[0]
        ext_label, ext_size, ext_erase = external[0]

        # Both slots must be multiples of the largest erase size so
        # mcuboot's swap algorithm works across the two flash devices.
        max_erase = max(int_erase, ext_erase)

        # Internal: mcuboot + slot0
        boot_size = align_up(MCUBOOT_SIZE, int_erase)
        slot0_offset = boot_size
        slot0_size = align_down(int_size - slot0_offset, max_erase)

        int_parts = [
            ("mcuboot", "boot_partition", 0, boot_size),
            ("image-0", "slot0_partition", slot0_offset, slot0_size),
        ]

        # If there's leftover internal space after slot0 (due to max_erase
        # rounding), use it for storage instead of wasting external flash.
        int_leftover = int_size - (slot0_offset + slot0_size)
        storage_on_internal = int_leftover >= int_erase

        if storage_on_internal:
            storage_offset = slot0_offset + slot0_size
            storage_size = align_down(int_leftover, int_erase)
            int_parts.append(("storage", "storage_partition", storage_offset, storage_size))

        result.append((int_label, int_size, int_erase, int_parts, set()))

        # External: slot1 + circuitpy (and storage/nvm if they didn't fit internally)
        # slot1 is slot0 + 1 sector of the largest erase size (mcuboot
        # swap-using-offset needs the extra sector as a scratch area).
        slot1_size = slot0_size + max_erase
        nvm_on_ext = not data_flash
        nvm_ext_size = ext_erase if nvm_on_ext else 0

        if storage_on_internal:
            nvm_ext_offset = 0
            slot1_offset = nvm_ext_size
        else:
            storage_size = ext_erase  # minimum: 1 erase page
            nvm_ext_offset = storage_size
            slot1_offset = nvm_ext_offset + nvm_ext_size

        slot1_offset = align_up(slot1_offset, ext_erase)
        circuitpy_offset = align_up(slot1_offset + slot1_size, ext_erase)
        circuitpy_size = align_down(ext_size - circuitpy_offset, ext_erase)

        ext_parts = []
        if not storage_on_internal:
            ext_parts.append(("storage", "storage_partition", 0, storage_size))
        if nvm_on_ext:
            ext_parts.append(("nvm", "nvm_partition", nvm_ext_offset, nvm_ext_size))
        ext_parts += [
            ("image-1", "slot1_partition", slot1_offset, slot1_size),
            ("circuitpy", "circuitpy_partition", circuitpy_offset, circuitpy_size),
        ]
        result.append((ext_label, ext_size, ext_erase, ext_parts, set()))

    elif internal:
        # ── Internal only ──
        int_label, int_size, int_erase = internal[0]

        boot_size = align_up(MCUBOOT_SIZE, int_erase)
        slot0_offset = boot_size
        # Reserve space at the end for storage + nvm + circuitpy.
        # Storage and NVM are each 1 erase page.
        storage_size = int_erase
        nvm_size = int_erase if not data_flash else 0
        # Give slot0 roughly half the remaining space.
        remaining = int_size - boot_size - storage_size - nvm_size
        slot0_size = align_down(remaining // 2, int_erase)
        storage_offset = slot0_offset + slot0_size
        nvm_offset = storage_offset + storage_size
        circuitpy_offset = nvm_offset + nvm_size
        circuitpy_size = align_down(int_size - circuitpy_offset, int_erase)

        int_parts = [
            ("mcuboot", "boot_partition", 0, boot_size),
            ("image-0", "slot0_partition", slot0_offset, slot0_size),
            ("storage", "storage_partition", storage_offset, storage_size),
        ]
        if nvm_size > 0:
            int_parts.append(("nvm", "nvm_partition", nvm_offset, nvm_size))
        int_parts.append(("circuitpy", "circuitpy_partition", circuitpy_offset, circuitpy_size))
        result.append((int_label, int_size, int_erase, int_parts, set()))

    elif external:
        # ── External only (XIP) ──
        ext_label, ext_size, ext_erase = external[0]

        boot_size = align_up(MCUBOOT_SIZE * 2, ext_erase)  # 128K for XIP boards
        slot0_offset = boot_size
        # slot0 and slot1 each get a portion; circuitpy gets the rest.
        # Use roughly 1/8 of flash for each slot, rest for circuitpy.
        slot_size = align_down(ext_size // 8, ext_erase)
        slot0_size = slot_size
        slot1_offset = slot0_offset + slot0_size
        slot1_size = slot_size
        storage_offset = slot1_offset + slot1_size
        storage_size = ext_erase  # minimum: 1 erase page
        nvm_offset = storage_offset + storage_size
        nvm_size = ext_erase
        circuitpy_offset = nvm_offset + nvm_size
        circuitpy_size = align_down(ext_size - circuitpy_offset, ext_erase)

        ext_parts = [
            ("mcuboot", "boot_partition", 0, boot_size),
            ("image-0", "slot0_partition", slot0_offset, slot0_size),
            ("image-1", "slot1_partition", slot1_offset, slot1_size),
            ("storage", "storage_partition", storage_offset, storage_size),
            ("nvm", "nvm_partition", nvm_offset, nvm_size),
            ("circuitpy", "circuitpy_partition", circuitpy_offset, circuitpy_size),
        ]
        result.append((ext_label, ext_size, ext_erase, ext_parts, set()))

    # If data flash exists, use it entirely for NVM.
    if data_flash:
        df_label, df_size, df_erase = data_flash[0]
        nvm_size = align_down(df_size, df_erase)
        if nvm_size > 0:
            result.append(
                (df_label, df_size, df_erase, [("nvm", "nvm_partition", 0, nvm_size)], set())
            )

    return result


def generate_partitions_dtsi(planned_devices):
    """Generate the contents of a partitions .dtsi file.

    Each entry in planned_devices is:
        (dev_label, total_size, erase_size, parts, predefined_labels)
    where predefined_labels is a set of partition labels already defined in the
    upstream DTS. Those partitions are skipped in the generated output.
    """
    lines = []

    for dev_label, total_size, erase_size, parts, *rest in planned_devices:
        predefined = rest[0] if rest else set()
        new_parts = [(l, nl, o, s) for l, nl, o, s in parts if l not in predefined]
        if not new_parts:
            continue

        lines.append(f"&{dev_label} {{")
        lines.append("\tpartitions {")
        # Only emit compatible/cells if the device has no predefined partitions
        # (i.e. the partitions node doesn't exist yet).
        if not predefined:
            lines.append('\t\tcompatible = "fixed-partitions";')
            lines.append("\t\t#address-cells = <1>;")
            lines.append("\t\t#size-cells = <1>;")

        for label, node_label, offset, size in new_parts:
            lines.append("")
            lines.append(f"\t\t{node_label}: partition@{offset:x} {{")
            lines.append(f'\t\t\tlabel = "{label}";')
            lines.append(f"\t\t\treg = <0x{offset:x} {format_dt_size(size)}>;")
            lines.append("\t\t};")

        lines.append("\t};")
        lines.append("};")
        lines.append("")

    return "\n".join(lines)


def get_partitions_dtsi_name(board_id):
    """Derive the partitions dtsi filename from the board_id.

    E.g. renesas_ek_ra6m5 -> ek_ra6m5 (strip vendor prefix).
    """
    boards_dir = PORT_DIR / "boards"
    next_underscore = board_id.find("_")
    while next_underscore != -1:
        vendor = board_id[:next_underscore]
        board = board_id[next_underscore + 1 :]
        if (boards_dir / vendor / board).is_dir():
            return board
        next_underscore = board_id.find("_", next_underscore + 1)
    return board_id


def fix_alignment(board_id, edt, flash_overrides=None):
    """Plan and write partition layout to boards/partitions/generated/<board>.dtsi."""
    dtsi_name = get_partitions_dtsi_name(board_id)
    dtsi_path = PORT_DIR / "boards" / "partitions" / "generated" / f"{dtsi_name}.dtsi"

    planned = plan_partitions(edt, flash_overrides)
    if not planned:
        print("  No flash devices found to plan partitions for.")
        return

    # Show the planned layout
    for dev_label, total_size, erase_size, parts, *rest in planned:
        predefined = rest[0] if rest else set()
        kind_parts = [
            (l, s, e, i)
            for l, s, e, i in discover_all_flash(edt, flash_overrides)
            if l == dev_label
        ]
        kind = "internal" if kind_parts and kind_parts[0][3] else "external"
        print(
            f"  {dev_label} ({format_size(total_size)}, {kind})"
            f"  erase page: {format_size(erase_size)}"
        )
        for label, node_label, offset, size in parts:
            marker = " (predefined)" if label in predefined else ""
            print(
                f"    {label}: 0x{offset:x} + {format_dt_size(size)} ({format_size(size)}){marker}"
            )
        print()

    content = generate_partitions_dtsi(planned)

    dtsi_path.parent.mkdir(parents=True, exist_ok=True)
    dtsi_path.write_text(content)
    print(f"  Wrote {dtsi_path.relative_to(PORT_DIR)}")

    # Check if the overlay already includes this dtsi
    overlay_name = _find_overlay(board_id)
    if overlay_name:
        overlay_path = PORT_DIR / "boards" / overlay_name
        overlay_text = overlay_path.read_text()
        include_line = f'#include "partitions/generated/{dtsi_name}.dtsi"'
        if include_line not in overlay_text:
            print(f"\n  NOTE: Add this to the top of boards/{overlay_name}:")
            print(f"    {include_line}")


def _find_overlay(board_id):
    """Find the overlay filename for a board_id."""
    boards_dir = PORT_DIR / "boards"
    for candidate in boards_dir.glob("*.overlay"):
        stem = candidate.stem
        if board_id.endswith(stem) or stem.startswith(board_id.split("_", 1)[-1]):
            return candidate.name
    return None


# ── Main ─────────────────────────────────────────────────────────────────


def main():
    boards = discover_boards()

    parser = argparse.ArgumentParser(
        description="Visualize or fix NVM partition layouts for CircuitPython Zephyr boards."
    )
    parser.add_argument("board_id", nargs="?", help="Board identifier")
    parser.add_argument(
        "--fix",
        action="store_true",
        help="Write aligned partitions to boards/partitions/<board>.dtsi",
    )
    parser.add_argument("--list", action="store_true", help="List available boards")
    args = parser.parse_args()

    if args.list or not args.board_id:
        print("Available boards:")
        for name in boards:
            print(f"  {name}")
        sys.exit(0 if args.list else 1)

    if args.board_id not in boards:
        print(f"Unknown board: {args.board_id}", file=sys.stderr)
        print("\nAvailable boards:")
        for name in boards:
            print(f"  {name}")
        sys.exit(1)

    board_toml = load_board_toml(args.board_id)
    if not board_toml.get("adaboot", True):
        print(f"Error: {args.board_id} has adaboot = false", file=sys.stderr)
        sys.exit(1)

    build_dir = PORT_DIR / f"build-partitions-{args.board_id}"

    # Load flash overrides from circuitpython.toml (e.g. erase_block_size
    # for SPI NOR flash that doesn't expose it in the DTS binding).
    flash_overrides = {}
    if "external_flash" in board_toml:
        ef = board_toml["external_flash"]
        flash_overrides[ef["label"]] = {"erase_block_size": ef["erase_block_size"]}

    print(f"Running cmake-only build for {args.board_id}...")
    cmake_only_build(args.board_id, build_dir)

    edt = load_edt(build_dir)

    print(f"  {args.board_id} — NVM Partition Layout")
    print()

    if args.fix:
        fix_alignment(args.board_id, edt, flash_overrides)
    else:
        devices = extract_flash_devices(edt, flash_overrides)
        if not devices:
            print(f"No flash devices with partitions found for {args.board_id}")
            sys.exit(1)
        show_layout(devices)


if __name__ == "__main__":
    main()
