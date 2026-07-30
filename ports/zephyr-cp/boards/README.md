# Board Configuration

Each board requires several files to integrate with the CircuitPython Zephyr
build system. This document describes the file layout, partitioning scheme, and
MCUboot (Adaboot) bootloader requirements.

## File Layout

For a board named `<board>` with Zephyr qualifier `<board>_<soc>_<cpu>`:

```
boards/
├── <vendor>/<board>/
│   ├── circuitpython.toml          # Build config (extensions, USB IDs)
│   └── autogen_board_info.toml     # Auto-generated module info (committed, not edited)
├── <board>_<soc>_<cpu>.conf        # Kconfig settings (BLE, WiFi, etc.)
├── <board>_<soc>_<cpu>.overlay     # Device tree overlay (code-partition, USB, board tweaks)
├── mcuboot_<board>.conf            # Board-specific MCUboot config (optional)
└── board_aliases.cmake             # Maps friendly names to Zephyr board specs
```

## Partition Structure

The flash partition layout is **owned by the mcuboot fork**, not by CircuitPython,
so that CircuitPython, Arduino, and Wippersnapper share one memory map. Each
board's layout lives in the fork as a single self-contained devicetree overlay:

```
bootloader/mcuboot/dts/<vendor>/<board>.dtsi
```

That overlay is applied to **both** the MCUboot image and the application image
by `sysbuild/CMakeLists.txt` (see below); the application never `#include`s it
and does not keep its own copy. Applications reference partitions by their node
labels (e.g. `FIXED_PARTITION_ID(filesystem_partition)`), so they automatically
follow any geometry change in the fork.

### Partition roles

These labels are the shared vocabulary across all apps that boot via this fork:

| label        | node label             | used by                       |
|--------------|------------------------|-------------------------------|
| `mcuboot`    | `boot_partition`       | bootloader                    |
| `image-0`    | `slot0_partition`      | application slot (links here) |
| `image-1`    | `slot1_partition`      | OTA update slot               |
| `image-2`    | `netcore_partition`    | net/radio core firmware       |
| `storage`    | `storage_partition`    | Zephyr settings / BT bonding  |
| `nvm`        | `nvm_partition`        | raw non-volatile byte access  |
| `filesystem` | `filesystem_partition` | user filesystem (CIRCUITPY)  |

Typical layout with internal + external flash:

| Partition  | Location | Purpose |
|------------|----------|---------|
| `mcuboot`  | Internal | Adaboot bootloader (128 KB) |
| `image-0`  | Internal | Application slot (fills remaining internal flash) |
| `storage`  | Internal | Zephyr settings storage |
| `nvm`      | Internal or External | Non-volatile memory |
| `image-1`  | External | OTA update slot (same size as image-0 + 1 erase page) |
| `image-2`  | Internal or flash1 | Net/radio core firmware (multi-core boards only) |
| `filesystem`| External | CircuitPython filesystem (remainder of external flash) |

For internal-only boards, all partitions share the single flash device.

### What goes in the fork's `<board>.dtsi

Each `<board>.dtsi` is a complete overlay:

1. **Delete Zephyr-default partitions** it replaces:
   ```dts
   &flash0 { /delete-node/ partitions; };
   ```
2. **Enable external flash** if disabled by default: `&mx25r64 { status = "okay"; };`
3. **Set up retention memory** for MCUboot double-tap reset (SoCs with `gpregret`).
4. **Define the `mcuboot-button0` alias** and any `chosen` the bootloader needs.
5. **Define the partition table** (the `&<dev> { partitions { ... } }` blocks).

The partition *numbers* are planned by `bootloader/mcuboot/tools/partition_layout.py`
(see `bootloader/mcuboot/dts/README.md`); the board-specific glue
around them (device enables, retention, chosen/aliases, `/delete-node/`) is
hand-maintained in the same file.

## Board Overlay (`<board>_<soc>_<cpu>.overlay`)

The overlay no longer includes partitions. It only adds application/board
configuration on top of the fork's layout:

```dts
/ {
    chosen {
        zephyr,code-partition = &slot0_partition;
    };
};

#include "../app.overlay"    /* Only if the board has USB */
```

Board-specific tweaks that sit on top of the fork layout also live here, e.g.
nRF54H20 shrinks slot0 and drops slot1 because CircuitPython doesn't use OTA:

```dts
&slot0_partition { reg = <0x40000 DT_SIZE_K(656)>; };
/delete-node/ &slot1_partition;
```

## MCUboot (Adaboot) Integration

The mcuboot fork is a **sysbuild module**, so it configures itself — nothing in
this port decides whether a board uses MCUboot. For a board it owns a layout for
(`bootloader/mcuboot/dts/<vendor>/<board>.dtsi`) it:

- applies that overlay to both the application and MCUboot images
  (`bootloader/mcuboot/dts/sysbuild.cmake`)
- defaults `SB_CONFIG_BOOTLOADER_MCUBOOT`, the MCUboot mode, and
  `SB_CONFIG_BOOT_SIGNATURE_TYPE_NONE` for it
  (`bootloader/mcuboot/dts/Kconfig.sysbuild`, generated from `tools/boards.toml`)

What `sysbuild/CMakeLists.txt` still does is CircuitPython's own bootloader UI
config for the MCUboot image:

- `sysbuild/mcuboot.conf` — UF2 drag-and-drop, GPIO button entrance, double-tap reset
- `boards/mcuboot_<board>.conf` — optional board-specific overrides

plus an escape hatch: `boards/sysbuild_<board>.conf` is applied as
`SB_EXTRA_CONF_FILE` if it exists, for overriding a Zephyr board default the
fork's Kconfig defaults can't reach (board Kconfig is parsed before module
Kconfig), e.g. `SB_CONFIG_BOOTLOADER_NONE=y`.

### Board-specific MCUboot config

Create `boards/mcuboot_<board>.conf` when the board needs:

- **Retention subsystem** (`CONFIG_RETAINED_MEM=y`, `CONFIG_RETENTION=y`,
  `CONFIG_RETENTION_BOOT_MODE=y`) — required for double-tap reset on boards
  with `gpregret`
- **USB driver quirks** (e.g. `CONFIG_MULTITHREADING=y` for DWC2 USB controllers)
- **UF2 disabled** (`CONFIG_MCUBOOT_UF2=n`) — for boards without USB
- **Double-tap disabled** (`CONFIG_MCUBOOT_UF2_ENTRANCE_DOUBLE_TAP=n`) — for
  boards without `gpregret` retention (e.g. nRF54H20)

## Multi-Image Updates

Boards with multiple cores can update all core firmware from a single UF2
drag-and-drop operation. The UF2 handler in MCUboot (`uf2_disk.c`) supports
multiple flash targets, routing UF2 blocks by `target_addr` to the correct
flash region.

### nRF5340 (cpuapp + cpunet, separate flash per core)

The nRF5340 has a dedicated 256 KB flash for the network core (`flash1` at
0x01000000), separate from the 1 MB app core flash (`flash0`).

The partition dtsi defines the net core flash controller
(`nordic,nrf53-flash-controller` at 0x41080000) so MCUboot running on the app
core can write to it. A `netcore_partition` covering the full 256 KB is
registered as a secondary UF2 target.

UF2 blocks targeting 0x00000000-range go to flash0 (app core), blocks
targeting 0x01000000-range go to flash1 (net core). The build system produces
separate UF2 files per core, concatenated into one file for the user.

| Device | Partition | Size | Purpose |
|--------|-----------|------|---------|
| flash0 | mcuboot | 128 KB | Bootloader |
| flash0 | image-0 | ~864 KB | App core firmware |
| flash0 | storage | 32 KB | Settings |
| flash1 | image-2 | 256 KB | Net core firmware (entire flash1) |
| mx25r64 | nvm | 4 KB | Non-volatile memory |
| mx25r64 | image-1 | ~868 KB | App core update slot |
| mx25r64 | filesystem | ~7 MB | CircuitPython filesystem |

Applies to: **nrf5340dk**, **nrf7002dk**

### nRF54H20 (cpuapp + cpurad, shared MRAM behind IronSide SE)

The nRF54H20 has a single 2 MB MRAM shared by all cores (cpuapp, cpurad,
cpuppr, cpuflpr). Nordic's IronSide SE secure element occupies the first
192 KB and acts as the first-stage bootloader — it validates and boots MCUboot
(Adaboot) at offset 0x30000.

IronSide SE's update service is only for updating IronSide SE itself.
Application firmware is managed entirely by MCUboot. The app core boots the
radio core at runtime via `ironside_se_cpuconf()`.

The `netcore_partition` (labeled `image-2`) holds the cpurad firmware. UF2
blocks are routed by address: cpuapp blocks target the `image-0` range,
cpurad blocks target the `image-2` range — both within MRAM.

No `gpregret` is available on nRF54H20, so double-tap reset is disabled.
UF2 entrance is via GPIO button only.

| Device | Partition | Size | Purpose |
|--------|-----------|------|---------|
| mram1x | *(IronSide SE)* | 192 KB | Secure boot (reserved, not touched) |
| mram1x | mcuboot | 128 KB | Adaboot bootloader |
| mram1x | image-0 | 1344 KB | App core firmware |
| mram1x | image-2 | 352 KB | Radio core firmware (cpurad) |
| mram1x | nvm | 16 KB | Non-volatile memory |
| mram1x | storage | 16 KB | Settings |
| mx25uw63 | image-1 | ~1348 KB | App core update slot |
| mx25uw63 | filesystem | ~6.7 MB | CircuitPython filesystem |

### nRF54L15 (single core with external flash)

Standard single-core layout. Internal RRAM holds the bootloader, app, and
settings. External SPI flash holds the update slot and CircuitPython
filesystem. RRAM's `gpregret` provides double-tap reset support.

The nRF54L15 has no USB, so UF2 is disabled in its MCUboot config. Firmware
is flashed via J-Link.

## Adding a New Board

1. Create `boards/<vendor>/<board>/circuitpython.toml` with build extensions
   and USB VID/PID.

2. Add a board alias to `board_aliases.cmake`:
   ```cmake
   cp_board_alias(vendor_board board/soc/cpu)
   ```

3. Register the board in the fork's `bootloader/mcuboot/tools/boards.toml`
   with its canonical Zephyr board id and, if it has SPI/QSPI flash without
   `erase-block-size` in the device tree, an `[external_flash]` override:
   ```toml
   [boards.vendor_board]
   board = "board/soc/cpu"

   [boards.vendor_board.external_flash]
   label = "mx25r64"
   erase_block_size = 4096
   ```

4. Generate the partition numbers into the fork:
   ```sh
   python3 bootloader/mcuboot/tools/partition_layout.py --fix vendor_board
   ```
   For multi-core boards the tool can't fully handle, hand-write the
   `&<dev> { partitions { ... } }` blocks in
   `bootloader/mcuboot/dts/<vendor>/<board>.dtsi`.

5. Hand-edit `bootloader/mcuboot/dts/<vendor>/<board>.dtsi` to add the
   board-specific glue around the generated blocks: `/delete-node/` of default
   partitions, enable external flash, retention cell + `chosen`, and the
   `mcuboot-button0` alias.

6. Create `boards/<board>_<soc>_<cpu>.overlay` — set `zephyr,code-partition`
   to `&slot0_partition`, include `app.overlay` if USB is available. Do **not**
   `#include` the partition dtsi; sysbuild applies it to both images.

7. Create `boards/mcuboot_<board>.conf` if needed for retention, USB quirks,
   or disabling features (UF2, double-tap).

8. Create `boards/<board>_<soc>_<cpu>.conf` for any Kconfig settings
   (BLE, WiFi, logging, etc.).

## Notes

- The `partition_layout.py` tool detects predefined mcuboot partitions in the
  upstream Zephyr DTS. When external flash is available, it moves slot1 to
  external flash and grows slot0 to fill internal flash.
- Boards without USB (e.g. nrf54l15dk) get partitions for filesystem/nvm/storage
  but disable UF2 in their mcuboot config. Firmware is flashed via J-Link or
  serial recovery.
- Boards that don't use mcuboot but share the fork's standard partitioning
  (UF2-native RP2 boards, XIP/direct-boot boards like SiWG917 and
  STM32H750B-DK, and nRF54LM20 DK which isn't on mcuboot yet) have their layout
  in `bootloader/mcuboot/dts/<vendor>/<board>.dtsi` too, applied to the app image
  only — no `#include` in the board overlay. Both the layout paths and which
  boards boot via mcuboot are declared in
  `bootloader/mcuboot/dts/mcuboot_boards.cmake`. The native
  simulators (`native_sim`, `nrf5340bsim`, `nrf54lm20bsim`) keep their
  `filesystem_partition` inline; they're test harnesses and don't use the
  bootloader, so they stay out of the fork.
