# SPDX-FileCopyrightText: 2025 Scott Shawcroft for Adafruit Industries
# SPDX-License-Identifier: MIT

"""Tests for cptools/bootloaderify.py partition planning and rendering."""

import sys
import pathlib
from types import SimpleNamespace

import pytest

# Add cptools to path (same pattern as test_zephyr2cp.py)
PORT_DIR = pathlib.Path(__file__).parent.parent.parent
sys.path.insert(0, str(PORT_DIR))

from cptools.bootloaderify import (
    KB,
    MB,
    align_up,
    align_down,
    format_size,
    format_dt_size,
    make_partitions_with_gaps,
    render_bar,
    render_detail_lines,
    generate_partitions_dtsi,
    plan_partitions,
    _has_predefined_mcuboot,
    discover_all_flash,
    get_erase_size,
    get_total_size,
    ZEPHYR_BASE,
)


# ── Helpers to build fake EDT nodes ──────────────────────────────────────


def _prop(val):
    return SimpleNamespace(val=val)


def _flash_node(
    label,
    size_bits=None,
    reg=None,
    erase_block_size=4096,
    compats=None,
    filename=None,
    children=None,
    labels=None,
):
    """Build a minimal fake flash node for testing."""
    props = {}
    if erase_block_size is not None:
        props["erase-block-size"] = _prop(erase_block_size)
    if size_bits is not None:
        props["size"] = _prop(size_bits)
    if reg is not None:
        props["reg"] = _prop(reg)
    return SimpleNamespace(
        name=label,
        labels=labels or [label],
        props=props,
        compats=compats or ["soc-nv-flash"],
        children=children or {},
        filename=filename or "",
    )


def _partition_node(label, node_label, offset, size):
    """Build a fake partition child node."""
    return SimpleNamespace(
        name=node_label,
        labels=[node_label],
        props={
            "label": _prop(label),
            "reg": _prop([offset, size]),
        },
    )


def _flash_with_partitions(label, size, erase, partitions, filename=None):
    """Build a flash node with predefined partition children.

    partitions: list of (label, node_label, offset, size) tuples.
    """
    part_children = {}
    for plabel, pnode_label, poffset, psize in partitions:
        part_children[pnode_label] = _partition_node(plabel, pnode_label, poffset, psize)
    partitions_node = SimpleNamespace(
        compats=["fixed-partitions"],
        children=part_children,
    )
    return _flash_node(
        label,
        size_bits=8 * size,
        erase_block_size=erase,
        filename=filename or str(ZEPHYR_BASE / "boards" / "vendor" / "board.dts"),
        children={"partitions": partitions_node},
    )


def _make_edt(*nodes):
    """Build a minimal fake EDT with the given nodes."""
    return SimpleNamespace(nodes=list(nodes))


# ── Unit tests: align_up / align_down ────────────────────────────────────


class TestAlign:
    def test_align_up_already_aligned(self):
        assert align_up(4096, 4096) == 4096

    def test_align_up_not_aligned(self):
        assert align_up(4097, 4096) == 8192

    def test_align_up_zero(self):
        assert align_up(0, 4096) == 0

    def test_align_down_already_aligned(self):
        assert align_down(8192, 4096) == 8192

    def test_align_down_not_aligned(self):
        assert align_down(8193, 4096) == 8192

    def test_align_down_zero(self):
        assert align_down(0, 4096) == 0


# ── Unit tests: format_size ──────────────────────────────────────────────


class TestFormatSize:
    def test_megabytes_exact(self):
        assert format_size(1 * MB) == "1 MB"
        assert format_size(2 * MB) == "2 MB"

    def test_megabytes_fractional(self):
        assert format_size(int(1.5 * MB)) == "1.5 MB"

    def test_kilobytes_exact(self):
        assert format_size(64 * KB) == "64 KB"
        assert format_size(256 * KB) == "256 KB"

    def test_kilobytes_fractional(self):
        assert format_size(int(4.5 * KB)) == "4.5 KB"

    def test_small(self):
        assert format_size(512) == "0.5 KB"


# ── Unit tests: format_dt_size ───────────────────────────────────────────


class TestFormatDtSize:
    def test_megabytes(self):
        assert format_dt_size(1 * MB) == "DT_SIZE_M(1)"
        assert format_dt_size(4 * MB) == "DT_SIZE_M(4)"

    def test_kilobytes(self):
        assert format_dt_size(64 * KB) == "DT_SIZE_K(64)"
        assert format_dt_size(256 * KB) == "DT_SIZE_K(256)"

    def test_raw_hex(self):
        assert format_dt_size(4097) == "0x1001"


# ── Unit tests: make_partitions_with_gaps ────────────────────────────────


class TestMakePartitionsWithGaps:
    def test_no_partitions(self):
        result = make_partitions_with_gaps([], 1 * MB)
        assert result == [("free", 0, 1 * MB)]

    def test_single_partition_at_start(self):
        parts = [("mcuboot", "boot_partition", 0, 64 * KB)]
        result = make_partitions_with_gaps(parts, 1 * MB)
        assert result[0] == ("mcuboot", 0, 64 * KB)
        assert result[1] == ("free", 64 * KB, 1 * MB - 64 * KB)

    def test_gap_between_partitions(self):
        parts = [
            ("mcuboot", "boot_partition", 0, 64 * KB),
            ("image-0", "slot0_partition", 128 * KB, 256 * KB),
        ]
        result = make_partitions_with_gaps(parts, 1 * MB)
        assert result[0] == ("mcuboot", 0, 64 * KB)
        assert result[1] == ("free", 64 * KB, 64 * KB)
        assert result[2] == ("image-0", 128 * KB, 256 * KB)
        assert result[3] == ("free", 384 * KB, 1 * MB - 384 * KB)

    def test_contiguous_partitions(self):
        parts = [
            ("mcuboot", "boot_partition", 0, 64 * KB),
            ("image-0", "slot0_partition", 64 * KB, 448 * KB),
        ]
        result = make_partitions_with_gaps(parts, 512 * KB)
        assert len(result) == 2
        assert result[0] == ("mcuboot", 0, 64 * KB)
        assert result[1] == ("image-0", 64 * KB, 448 * KB)

    def test_fills_entire_flash(self):
        parts = [("image-0", "slot0_partition", 0, 1 * MB)]
        result = make_partitions_with_gaps(parts, 1 * MB)
        assert len(result) == 1


# ── Unit tests: render_bar ───────────────────────────────────────────────


class TestRenderBar:
    def test_single_partition(self):
        parts = [("mcuboot", "boot_partition", 0, 1 * MB)]
        bar = render_bar(parts, 1 * MB)
        assert len(bar) == 72
        assert "B" in bar

    def test_mixed_partitions(self):
        parts = [
            ("mcuboot", "boot_partition", 0, 64 * KB),
            ("image-0", "slot0_partition", 64 * KB, 448 * KB),
            ("circuitpy", "circuitpy_partition", 512 * KB, 512 * KB),
        ]
        bar = render_bar(parts, 1 * MB)
        assert len(bar) == 72
        assert "B" in bar
        assert "0" in bar
        assert "#" in bar


# ── Unit tests: render_detail_lines ──────────────────────────────────────


class TestRenderDetailLines:
    def test_alignment_warnings(self):
        erase = 4096
        parts = [("mcuboot", "boot_partition", 100, 5000)]  # misaligned offset and size
        lines = render_detail_lines(parts, 1 * MB, erase)
        mcuboot_line = [l for l in lines if "mcuboot" in l][0]
        assert "!offset" in mcuboot_line
        assert "!size" in mcuboot_line

    def test_no_warnings_when_aligned(self):
        erase = 4096
        parts = [("mcuboot", "boot_partition", 0, 64 * KB)]
        lines = render_detail_lines(parts, 1 * MB, erase)
        assert "!offset" not in lines[0]
        assert "!size" not in lines[0]

    def test_small_free_gaps_hidden(self):
        erase = 4096
        parts = [
            ("mcuboot", "boot_partition", 0, 64 * KB),
            ("image-0", "slot0_partition", 64 * KB + 512, 256 * KB),
        ]
        lines = render_detail_lines(parts, 1 * MB, erase)
        # The 512-byte gap should be hidden (< 1024)
        labels = [l.strip().split()[-1] for l in lines]
        assert "free" not in labels


# ── Unit tests: get_erase_size ───────────────────────────────────────────


class TestGetEraseSize:
    def test_from_erase_block_size_prop(self):
        node = _flash_node("flash0", erase_block_size=8192)
        assert get_erase_size(node) == 8192

    def test_from_pages_layout(self):
        layout_child = SimpleNamespace(
            props={"pages-size": _prop(32768)},
            children={},
        )
        pages_layout = SimpleNamespace(children={"layout0": layout_child})
        node = _flash_node(
            "flash0", erase_block_size=None, children={"pages_layout": pages_layout}
        )
        assert get_erase_size(node) == 32768

    def test_default_4096(self):
        node = _flash_node("flash0", erase_block_size=None)
        assert get_erase_size(node) == 4096


# ── Unit tests: get_total_size ───────────────────────────────────────────


class TestGetTotalSize:
    def test_from_size_prop_bits(self):
        node = _flash_node("flash0", size_bits=8 * 1 * MB)
        assert get_total_size(node) == 1 * MB

    def test_from_reg_prop(self):
        node = _flash_node("flash0", reg=[0x0, 512 * KB])
        assert get_total_size(node) == 512 * KB

    def test_zero_when_no_size(self):
        node = _flash_node("flash0")
        assert get_total_size(node) == 0


# ── Unit tests: discover_all_flash ───────────────────────────────────────


class TestDiscoverAllFlash:
    def test_finds_flash_node(self):
        node = _flash_node("flash0", size_bits=8 * MB * 8, erase_block_size=4096)
        edt = _make_edt(node)
        devices = discover_all_flash(edt)
        assert len(devices) == 1
        assert devices[0][0] == "flash0"  # dev_label
        assert devices[0][1] == 8 * MB  # total_size
        assert devices[0][2] == 4096  # erase_size

    def test_skips_controllers(self):
        node = _flash_node(
            "flash_controller",
            size_bits=8 * MB * 8,
            erase_block_size=4096,
            compats=["nrf-flash-controller"],
        )
        edt = _make_edt(node)
        devices = discover_all_flash(edt)
        assert len(devices) == 0

    def test_skips_zero_size(self):
        node = _flash_node("flash0", erase_block_size=4096)
        edt = _make_edt(node)
        devices = discover_all_flash(edt)
        assert len(devices) == 0

    def test_internal_detection(self):
        internal_path = str(ZEPHYR_BASE / "dts" / "arm" / "some_soc.dtsi")
        node = _flash_node("flash0", size_bits=8 * MB * 8, filename=internal_path)
        edt = _make_edt(node)
        devices = discover_all_flash(edt)
        assert devices[0][3] is True  # is_internal

    def test_external_detection(self):
        external_path = str(ZEPHYR_BASE / "boards" / "vendor" / "board.dts")
        node = _flash_node("mx25r", size_bits=8 * 16 * MB, filename=external_path)
        edt = _make_edt(node)
        devices = discover_all_flash(edt)
        assert devices[0][3] is False  # is_internal


# ── Unit tests: plan_partitions ──────────────────────────────────────────


def _internal_flash(label="flash0", size=2 * MB, erase=4096):
    return _flash_node(
        label,
        size_bits=8 * size,
        erase_block_size=erase,
        filename=str(ZEPHYR_BASE / "dts" / "arm" / "soc.dtsi"),
    )


def _external_flash(label="mx25r", size=16 * MB, erase=4096):
    return _flash_node(
        label,
        size_bits=8 * size,
        erase_block_size=erase,
        filename=str(ZEPHYR_BASE / "boards" / "vendor" / "board.dts"),
    )


class TestPlanPartitions:
    def test_empty_edt(self):
        edt = _make_edt()
        assert plan_partitions(edt) == []

    def test_internal_only(self):
        edt = _make_edt(_internal_flash())
        result = plan_partitions(edt)
        assert len(result) == 1
        dev_label, total_size, erase_size, parts, _predefined = result[0]
        assert dev_label == "flash0"
        labels = [p[0] for p in parts]
        assert labels == ["mcuboot", "image-0", "storage", "nvm", "circuitpy"]
        # No slot1 when internal-only
        assert "image-1" not in labels

    def test_internal_only_alignment(self):
        edt = _make_edt(_internal_flash())
        result = plan_partitions(edt)
        _, _, erase_size, parts, _ = result[0]
        for label, node_label, offset, size in parts:
            assert offset % erase_size == 0, f"{label} offset 0x{offset:x} not aligned"
            assert size % erase_size == 0, f"{label} size 0x{size:x} not aligned"

    def test_internal_only_no_overlap(self):
        edt = _make_edt(_internal_flash())
        result = plan_partitions(edt)
        _, total_size, _, parts, _ = result[0]
        for i in range(len(parts) - 1):
            end_i = parts[i][2] + parts[i][3]
            start_next = parts[i + 1][2]
            assert end_i <= start_next, (
                f"{parts[i][0]} ends at 0x{end_i:x} but {parts[i + 1][0]} starts at 0x{start_next:x}"
            )
        # Last partition should not exceed flash
        last_end = parts[-1][2] + parts[-1][3]
        assert last_end <= total_size

    def test_internal_plus_external(self):
        edt = _make_edt(_internal_flash(), _external_flash())
        result = plan_partitions(edt)
        assert len(result) == 2

        int_label, _, _, int_parts, _ = result[0]
        ext_label, _, _, ext_parts, _ = result[1]
        assert int_label == "flash0"
        assert ext_label == "mx25r"

        int_labels = [p[0] for p in int_parts]
        ext_labels = [p[0] for p in ext_parts]

        assert "mcuboot" in int_labels
        assert "image-0" in int_labels
        assert "image-1" in ext_labels
        assert "circuitpy" in ext_labels
        # NVM should be on external when no data flash
        assert "nvm" in ext_labels

    def test_internal_plus_external_alignment(self):
        edt = _make_edt(
            _internal_flash(erase=4096),
            _external_flash(erase=65536),
        )
        result = plan_partitions(edt)
        for dev_label, total_size, erase_size, parts, _ in result:
            for label, node_label, offset, size in parts:
                assert offset % erase_size == 0, (
                    f"{dev_label}/{label} offset 0x{offset:x} not aligned to {erase_size}"
                )
                assert size % erase_size == 0, (
                    f"{dev_label}/{label} size 0x{size:x} not aligned to {erase_size}"
                )

    def test_internal_plus_external_slot_sizes(self):
        """slot1 should be slot0 + max_erase for swap-using-offset."""
        edt = _make_edt(
            _internal_flash(erase=4096),
            _external_flash(erase=4096),
        )
        result = plan_partitions(edt)
        int_parts = {p[0]: p for p in result[0][3]}
        ext_parts = {p[0]: p for p in result[1][3]}
        slot0_size = int_parts["image-0"][3]
        slot1_size = ext_parts["image-1"][3]
        max_erase = max(result[0][2], result[1][2])
        assert slot1_size == slot0_size + max_erase

    def test_ra6_internal_32k_external_4k(self):
        """RA6-like: 2MB internal @ 32K erase, 16MB external @ 4K erase.

        slot0 should be 62 * 32K pages (filling internal after mcuboot).
        slot1 must be 63 * 32K pages — one extra max_erase sector for
        mcuboot swap-using-offset scratch, even though external is 4K pages.
        """
        edt = _make_edt(
            _internal_flash(size=2 * MB, erase=32 * KB),
            _external_flash(size=16 * MB, erase=4096),
        )
        result = plan_partitions(edt)
        assert len(result) == 2

        int_parts = {p[0]: p for p in result[0][3]}
        ext_parts = {p[0]: p for p in result[1][3]}

        # slot0 fills internal after 64K mcuboot, aligned to 32K
        assert int_parts["mcuboot"][3] == 64 * KB
        assert int_parts["image-0"][3] == 62 * 32 * KB

        # slot1 = slot0 + one 32K sector (max_erase), NOT one 4K sector
        assert ext_parts["image-1"][3] == 63 * 32 * KB

        # All partitions on each device must be aligned to that device's erase
        for dev_label, total_size, erase_size, parts, _ in result:
            for label, node_label, offset, size in parts:
                assert offset % erase_size == 0, (
                    f"{dev_label}/{label} offset 0x{offset:x} not aligned to {erase_size}"
                )
                assert size % erase_size == 0, (
                    f"{dev_label}/{label} size 0x{size:x} not aligned to {erase_size}"
                )

    def test_external_only(self):
        edt = _make_edt(_external_flash(size=16 * MB, erase=4096))
        result = plan_partitions(edt)
        assert len(result) == 1
        _, _, _, parts, _ = result[0]
        labels = [p[0] for p in parts]
        assert labels == ["mcuboot", "image-0", "image-1", "storage", "nvm", "circuitpy"]

    def test_external_only_alignment(self):
        edt = _make_edt(_external_flash(size=16 * MB, erase=65536))
        result = plan_partitions(edt)
        _, _, erase_size, parts, _ = result[0]
        for label, node_label, offset, size in parts:
            assert offset % erase_size == 0, f"{label} offset not aligned"
            assert size % erase_size == 0, f"{label} size not aligned"

    def test_small_external_ignored(self):
        """External flash < 1MB should be ignored (e.g. small SPI flash)."""
        edt = _make_edt(
            _internal_flash(),
            _external_flash(label="spi_flash", size=512 * KB),
        )
        result = plan_partitions(edt)
        # Should behave as internal-only (small external ignored)
        assert len(result) == 1
        assert result[0][0] == "flash0"

    def test_mcuboot_at_least_64k(self):
        edt = _make_edt(_internal_flash(erase=4096))
        result = plan_partitions(edt)
        _, _, _, parts, _ = result[0]
        boot = [p for p in parts if p[0] == "mcuboot"][0]
        assert boot[3] >= 64 * KB

    def test_data_flash_nvm(self):
        """Small internal data flash (like RA6 flash1) should be used for NVM."""
        edt = _make_edt(
            _internal_flash(label="flash0", size=2 * MB, erase=4096),
            _internal_flash(label="flash1", size=8 * KB, erase=64),
            _external_flash(size=16 * MB, erase=4096),
        )
        result = plan_partitions(edt)
        # Should have: internal (flash0), external (mx25r), data flash (flash1)
        dev_labels = [r[0] for r in result]
        assert "flash1" in dev_labels

        # flash1 should contain only an NVM partition
        flash1 = [r for r in result if r[0] == "flash1"][0]
        _, _, _, parts, _ = flash1
        assert len(parts) == 1
        assert parts[0][0] == "nvm"
        assert parts[0][1] == "nvm_partition"
        # NVM should use the whole data flash (aligned to erase size)
        assert parts[0][3] == 8 * KB

        # External should NOT have NVM (data flash handles it)
        ext = [r for r in result if r[0] == "mx25r"][0]
        ext_labels = [p[0] for p in ext[3]]
        assert "nvm" not in ext_labels

    def test_data_flash_nvm_internal_only(self):
        """Data flash + internal only: NVM on data flash, not on main internal."""
        edt = _make_edt(
            _internal_flash(label="flash0", size=2 * MB, erase=4096),
            _internal_flash(label="flash1", size=8 * KB, erase=64),
        )
        result = plan_partitions(edt)
        dev_labels = [r[0] for r in result]
        assert "flash1" in dev_labels

        # Main internal should not have NVM
        flash0 = [r for r in result if r[0] == "flash0"][0]
        flash0_labels = [p[0] for p in flash0[3]]
        assert "nvm" not in flash0_labels

        # Data flash should have NVM
        flash1 = [r for r in result if r[0] == "flash1"][0]
        flash1_labels = [p[0] for p in flash1[3]]
        assert "nvm" in flash1_labels

    def test_nvm_on_external_when_no_data_flash(self):
        """Without data flash, NVM goes on external (1 erase page)."""
        edt = _make_edt(
            _internal_flash(erase=4096),
            _external_flash(erase=4096),
        )
        result = plan_partitions(edt)
        ext = [r for r in result if r[0] == "mx25r"][0]
        ext_parts = {p[0]: p for p in ext[3]}
        assert "nvm" in ext_parts
        # NVM should be 1 erase page
        assert ext_parts["nvm"][3] == 4096

    def test_nvm_on_internal_when_internal_only(self):
        """Internal-only boards get NVM on main flash (1 erase page)."""
        edt = _make_edt(_internal_flash(erase=4096))
        result = plan_partitions(edt)
        _, _, _, parts, _ = result[0]
        nvm = [p for p in parts if p[0] == "nvm"]
        assert len(nvm) == 1
        assert nvm[0][3] == 4096


# ── Unit tests: generate_partitions_dtsi ─────────────────────────────────


class TestGeneratePartitionsDtsi:
    def test_basic_output(self):
        planned = [
            (
                "flash0",
                512 * KB,
                4096,
                [
                    ("mcuboot", "boot_partition", 0, 64 * KB),
                    ("image-0", "slot0_partition", 64 * KB, 192 * KB),
                    ("storage", "storage_partition", 256 * KB, 4096),
                    ("circuitpy", "circuitpy_partition", 260 * KB, 252 * KB),
                ],
            )
        ]
        dtsi = generate_partitions_dtsi(planned)
        assert "&flash0 {" in dtsi
        assert 'compatible = "fixed-partitions"' in dtsi
        assert "boot_partition: partition@0" in dtsi
        assert 'label = "mcuboot"' in dtsi
        assert "DT_SIZE_K(64)" in dtsi
        assert "slot0_partition: partition@10000" in dtsi

    def test_empty_parts_skipped(self):
        planned = [("flash0", 512 * KB, 4096, [])]
        dtsi = generate_partitions_dtsi(planned)
        assert dtsi == ""

    def test_two_devices(self):
        planned = [
            (
                "flash0",
                512 * KB,
                4096,
                [("mcuboot", "boot_partition", 0, 64 * KB)],
            ),
            (
                "mx25r",
                16 * MB,
                4096,
                [("circuitpy", "circuitpy_partition", 0, 16 * MB)],
            ),
        ]
        dtsi = generate_partitions_dtsi(planned)
        assert "&flash0 {" in dtsi
        assert "&mx25r {" in dtsi

    def test_predefined_partitions_skipped(self):
        """Predefined partitions should not appear in the generated dtsi."""
        predefined = {"mcuboot", "image-0", "image-1", "storage"}
        planned = [
            (
                "flash0",
                4 * MB,
                4096,
                [
                    ("mcuboot", "boot_partition", 0x2400, 0xDC00),
                    ("image-0", "slot0_partition", 0x10000, 512 * KB),
                    ("image-1", "slot1_partition", 0x90000, 512 * KB),
                    ("storage", "storage_partition", 0x110000, 32 * KB),
                    ("nvm", "nvm_partition", 0x118000, 4096),
                    ("circuitpy", "circuitpy_partition", 0x119000, 4 * MB - 0x119000),
                ],
                predefined,
            )
        ]
        dtsi = generate_partitions_dtsi(planned)
        # Only nvm and circuitpy should appear
        assert 'label = "nvm"' in dtsi
        assert 'label = "circuitpy"' in dtsi
        assert 'label = "mcuboot"' not in dtsi
        assert 'label = "image-0"' not in dtsi
        assert 'label = "image-1"' not in dtsi
        assert 'label = "storage"' not in dtsi
        # Should NOT emit compatible since partitions node already exists
        assert 'compatible = "fixed-partitions"' not in dtsi

    def test_predefined_all_skipped_means_no_output(self):
        """If all partitions are predefined, nothing should be generated."""
        predefined = {"mcuboot", "image-0"}
        planned = [
            (
                "flash0",
                1 * MB,
                4096,
                [
                    ("mcuboot", "boot_partition", 0, 64 * KB),
                    ("image-0", "slot0_partition", 64 * KB, 512 * KB),
                ],
                predefined,
            )
        ]
        dtsi = generate_partitions_dtsi(planned)
        assert dtsi == ""


# ── Unit tests: predefined mcuboot layout ────────────────────────────────


# DA14695-like: 4MB flash, mcuboot+slots+storage already defined upstream
_DA14695_PARTITIONS = [
    ("mcuboot", "boot_partition", 0x2400, 0xDC00),
    ("image-0", "slot0_partition", 0x10000, 512 * KB),
    ("image-1", "slot1_partition", 0x90000, 512 * KB),
    ("storage", "storage_partition", 0x110000, 32 * KB),
]


class TestPredefinedMcuboot:
    def test_has_predefined_mcuboot_detected(self):
        """A board with existing mcuboot partitions should be detected."""
        flash = _flash_with_partitions("flash0", 4 * MB, 4096, _DA14695_PARTITIONS)
        edt = _make_edt(flash)
        assert _has_predefined_mcuboot(edt) is not None

    def test_no_predefined_mcuboot(self):
        """A board without existing partitions should not be detected."""
        edt = _make_edt(_internal_flash())
        assert _has_predefined_mcuboot(edt) is None

    def test_predefined_adds_nvm_and_circuitpy(self):
        """Predefined layout should get nvm and circuitpy added in free space."""
        flash = _flash_with_partitions("flash0", 4 * MB, 4096, _DA14695_PARTITIONS)
        edt = _make_edt(flash)
        result = plan_partitions(edt)
        assert len(result) == 1
        _, _, _, parts, predefined = result[0]
        labels = [p[0] for p in parts]
        # Original partitions kept
        assert "mcuboot" in labels
        assert "image-0" in labels
        assert "image-1" in labels
        assert "storage" in labels
        # New partitions added
        assert "nvm" in labels
        assert "circuitpy" in labels
        # Predefined set should contain the original labels
        assert predefined == {"mcuboot", "image-0", "image-1", "storage"}

    def test_predefined_preserves_original_offsets(self):
        """Original partition offsets and sizes should be unchanged."""
        flash = _flash_with_partitions("flash0", 4 * MB, 4096, _DA14695_PARTITIONS)
        edt = _make_edt(flash)
        result = plan_partitions(edt)
        _, _, _, parts, _ = result[0]
        parts_by_label = {p[0]: p for p in parts}
        for label, node_label, offset, size in _DA14695_PARTITIONS:
            assert parts_by_label[label][2] == offset
            assert parts_by_label[label][3] == size

    def test_predefined_new_partitions_dont_overlap(self):
        """New partitions should not overlap with existing ones."""
        flash = _flash_with_partitions("flash0", 4 * MB, 4096, _DA14695_PARTITIONS)
        edt = _make_edt(flash)
        result = plan_partitions(edt)
        _, total_size, _, parts, _ = result[0]
        sorted_parts = sorted(parts, key=lambda p: p[2])
        for i in range(len(sorted_parts) - 1):
            end_i = sorted_parts[i][2] + sorted_parts[i][3]
            start_next = sorted_parts[i + 1][2]
            assert end_i <= start_next, (
                f"{sorted_parts[i][0]} ends at 0x{end_i:x} but "
                f"{sorted_parts[i + 1][0]} starts at 0x{start_next:x}"
            )
        last_end = sorted_parts[-1][2] + sorted_parts[-1][3]
        assert last_end <= total_size

    def test_predefined_new_partitions_aligned(self):
        """New partitions should be aligned to the erase size."""
        flash = _flash_with_partitions("flash0", 4 * MB, 4096, _DA14695_PARTITIONS)
        edt = _make_edt(flash)
        result = plan_partitions(edt)
        _, _, erase_size, parts, predefined = result[0]
        for label, node_label, offset, size in parts:
            if label not in predefined:
                assert offset % erase_size == 0, f"{label} offset not aligned"
                assert size % erase_size == 0, f"{label} size not aligned"

    def test_predefined_circuitpy_fills_remaining(self):
        """circuitpy should use all remaining flash after nvm."""
        flash = _flash_with_partitions("flash0", 4 * MB, 4096, _DA14695_PARTITIONS)
        edt = _make_edt(flash)
        result = plan_partitions(edt)
        _, total_size, _, parts, _ = result[0]
        circuitpy = [p for p in parts if p[0] == "circuitpy"][0]
        # circuitpy should end at the flash boundary
        assert circuitpy[2] + circuitpy[3] == total_size

    def test_predefined_dtsi_only_has_new_partitions(self):
        """Generated dtsi should only contain new (non-predefined) partitions."""
        flash = _flash_with_partitions("flash0", 4 * MB, 4096, _DA14695_PARTITIONS)
        edt = _make_edt(flash)
        result = plan_partitions(edt)
        dtsi = generate_partitions_dtsi(result)
        assert 'label = "nvm"' in dtsi
        assert 'label = "circuitpy"' in dtsi
        assert 'label = "mcuboot"' not in dtsi
        assert 'label = "image-0"' not in dtsi

    def test_existing_cp_partitions_replaced(self):
        """Existing circuitpy/nvm partitions from a prior overlay should be replaced."""
        # Simulate da14695 with its overlay-added circuitpy partition
        parts_with_cp = _DA14695_PARTITIONS + [
            ("circuitpy", "circuitpy_partition", 0x118000, 4 * MB - 0x118000),
        ]
        flash = _flash_with_partitions("flash0", 4 * MB, 4096, parts_with_cp)
        edt = _make_edt(flash)
        result = plan_partitions(edt)
        _, _, _, parts, predefined = result[0]
        labels = [p[0] for p in parts]

        # circuitpy and nvm should both be present (regenerated)
        assert "circuitpy" in labels
        assert "nvm" in labels

        # They should NOT be in the predefined set
        assert "circuitpy" not in predefined
        assert "nvm" not in predefined

        # The circuitpy offset should differ (nvm now takes a page before it)
        circuitpy = [p for p in parts if p[0] == "circuitpy"][0]
        assert circuitpy[2] != 0x118000  # not the old overlay offset
