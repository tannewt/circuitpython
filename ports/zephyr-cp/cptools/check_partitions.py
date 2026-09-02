#!/usr/bin/env python3
"""Check the flash partition layout a board's devicetree actually resolved to.

Board overlays in ``boards/`` routinely rebuild the ``partitions`` node so the
board gets a CIRCUITPY filesystem. Rebuilding it is easy to get subtly wrong in
a way nothing reports: drop the ``ranges;`` the board DTS declared and
devicetree stops translating partition addresses into the SoC's address space,
so every partition resolves to a bare offset instead. The build still succeeds.
What changes is anything keyed off the address -- on RP2040,
``RP2_REQUIRES_SECOND_STAGE_BOOT`` matches the code partition against
0x10000100, so a partition left at 0x100 silently turns it off and the UF2 is
built for the wrong address with no second stage bootloader linked in.

This reads the ``edt.pickle`` a build already produced, so it costs no build
time -- point it at build directories after building, or run it with no
arguments to check every build directory in the port.

    python cptools/check_partitions.py                       # every build-* dir
    python cptools/check_partitions.py build-raspberrypi_rpi_pico_zephyr

Exits non-zero when a layout has problems.
"""

import argparse
import pathlib
import pickle
import sys

PORT_DIR = pathlib.Path(__file__).resolve().parent.parent
EDT_MODULE = PORT_DIR / "zephyr" / "scripts" / "dts" / "python-devicetree" / "src"


def load_edt(build_dir):
    """Load the pickled devicetree a build produced, or None if there is none."""
    edt_path = build_dir / "zephyr-cp" / "zephyr" / "edt.pickle"
    if not edt_path.is_file():
        edt_path = build_dir / "zephyr" / "edt.pickle"
    if not edt_path.is_file():
        return None
    sys.path.insert(0, str(EDT_MODULE))
    with open(edt_path, "rb") as f:
        return pickle.load(f)


def partition_device(node):
    """Walk up to the NVM device owning a partition.

    The device is the first ancestor carrying ``reg``; the ``partitions``
    grouping node in between has none.
    """
    parent = getattr(node, "parent", None)
    while parent is not None:
        if parent.props.get("reg") is not None:
            return parent
        parent = getattr(parent, "parent", None)
    return None


def device_size(node):
    """Total size of an NVM device.

    External SPI/QSPI NOR carries its capacity in the ``size`` property (in
    bits) and uses ``reg`` for the chip select, so reading ``reg`` alone gives
    0 for exactly the devices CIRCUITPY usually lives on.
    """
    size = node.props.get("size")
    if size:
        return size.val // 8
    reg = node.props.get("reg")
    if reg and len(reg.val) >= 2:
        return reg.val[1]
    return 0


def is_partition_child(node):
    """True for any node under a ``partitions`` grouping node.

    Membership is positional rather than by ``compatible``: an overlay may add
    a partition carrying neither ``zephyr,mapped-partition`` nor a
    ``fixed-partitions`` parent, and it still occupies the space and still is
    what the layout means to describe.
    """
    parent = getattr(node, "parent", None)
    return parent is not None and getattr(parent, "name", "") == "partitions"


def iter_partitions(edt):
    """Yield ``(node, device, offset, size, mapped)`` for every partition."""
    for node in edt.nodes:
        mapped = "zephyr,mapped-partition" in getattr(node, "compats", [])
        if not mapped and not is_partition_child(node):
            continue
        reg = node.props.get("reg")
        if not reg or len(reg.val) < 2:
            continue
        dev = partition_device(node)
        if dev is None:
            continue
        if mapped and (not getattr(node, "regs", None) or not getattr(dev, "regs", None)):
            continue
        yield node, dev, reg.val[0], reg.val[1], mapped


def check_layout(edt):
    """Return a list of problems with the resolved layout.

    Mapped partitions are checked for address translation; every partition is
    checked for overlap and for running past the end of its device. Erase-page
    alignment is not checked: RP2040 deliberately puts its code partition at
    0x100, directly behind the 256-byte second stage bootloader.
    """
    problems = []
    by_device = {}
    for node, dev, offset, size, mapped in iter_partitions(edt):
        label = node.labels[0] if node.labels else node.name
        dev_label = dev.labels[0] if dev.labels else dev.name
        total = device_size(dev)
        if mapped:
            # Test the resolved address against the device's own window rather
            # than against base + reg. Both forms are in use: a partition reg
            # is usually an offset, but some overlays write the absolute
            # address and leave the partitions node without ranges, which
            # resolves to the same correct address. What is never right is a
            # partition resolving outside the device it lives in -- which is
            # exactly what a rebuilt partitions node missing ranges; produces,
            # since the offset is then left untranslated.
            base = dev.regs[0].addr
            actual = node.regs[0].addr
            if total and not (base <= actual < base + total):
                problems.append(
                    f"{label}: resolves to 0x{actual:x}, outside {dev_label} "
                    f"(0x{base:x}-0x{base + total:x}) -- address translation is "
                    f"broken; does the partitions node declare ranges;?"
                )
            # Geometry below is compared in offsets from the device base, so a
            # partition declared either way lands in the same space. When the
            # translation is broken the difference is meaningless (and often
            # negative), so keep the declared offset rather than running the
            # geometry checks on nonsense.
            translated = actual - base
            if 0 <= translated and (not total or translated < total):
                offset = translated
        by_device.setdefault(dev_label, (dev, total, []))[2].append((label, offset, size))

    for dev_label, (dev, total, parts) in sorted(by_device.items()):
        ordered = sorted(parts, key=lambda p: p[1])
        for label, offset, size in ordered:
            if total and offset + size > total:
                problems.append(
                    f"{label}: ends at 0x{offset + size:x}, past the end of "
                    f"{dev_label} (0x{total:x})"
                )
        # A running high-water mark, not neighbouring pairs: a partition
        # spanning several later ones only overlaps its immediate successor in
        # a pairwise walk.
        high_label, high_end = None, 0
        for label, offset, size in ordered:
            if offset < high_end:
                problems.append(
                    f"{high_label} (ends 0x{high_end:x}) overlaps "
                    f"{label} (starts 0x{offset:x}) on {dev_label}"
                )
            if offset + size > high_end:
                high_label, high_end = label, offset + size
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "build_dirs",
        nargs="*",
        help="Build directories to check (default: every build-* in the port)",
    )
    args = parser.parse_args()

    dirs = [pathlib.Path(d) for d in args.build_dirs]
    if not dirs:
        dirs = sorted(p for p in PORT_DIR.glob("build-*") if p.is_dir())
    if not dirs:
        print("No build directories found; build a board first.", file=sys.stderr)
        return 1

    failed = []
    checked = 0
    for build_dir in dirs:
        edt = load_edt(build_dir)
        if edt is None:
            continue
        checked += 1
        n_parts = sum(1 for _ in iter_partitions(edt))
        problems = check_layout(edt)
        if problems:
            failed.append(build_dir.name)
            print(f"{build_dir.name}:")
            for problem in problems:
                print(f"  FAIL  {problem}")
        else:
            # Say how much was inspected: a board whose overlay defines no
            # partitions at all would otherwise be indistinguishable from a
            # verified-good one.
            print(f"{build_dir.name}: ok ({n_parts} partitions checked)")

    if not checked:
        print("No build directory held an edt.pickle; build a board first.", file=sys.stderr)
        return 1
    if failed:
        print(f"\n{len(failed)} board(s) with layout problems: {', '.join(failed)}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
