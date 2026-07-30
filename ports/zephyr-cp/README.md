# Zephyr

This is an initial port of CircuitPython onto Zephyr. We intend on migrating all
existing boards onto Zephyr. To start, we'll only support new boards. Existing
boards will be switched as the Zephyr port reaches parity with the existing
implementation.

## Getting Started

First, install Zephyr tools (see [Zephyr's Getting Started Guide](https://docs.zephyrproject.org/4.0.0/develop/getting_started/index.html)). (These are `fish` commands because that's what Scott uses.)


```sh
pip install west
west init -l zephyr-config
west update
west zephyr-export
pip install -r zephyr/scripts/requirements.txt
west sdk install
```

Now to build from `ports/zephyr-cp`:

```sh
make BOARD=nordic_nrf7002dk
```

This uses Zephyr's cmake to generate Makefiles that then delegate to
`tools/cpbuild/build_circuitpython.py` to build the CircuitPython bits in parallel.

## Native simulator build container

Building the native sim requires `libsdl2-dev:i386` and other 32bit dependencies that
can cause conflicts on 64bit systems resulting in the removal of 64bit versions of critical
software such as the display manager and network manager. A Containerfile and a few scripts
are provided to set up a container to make the native sim build inside without affecting the
host system.

The container automatically mounts this instance of the circuitpython repo inside at
`/home/dev/circuitpython`. Changes made in the repo inside the container and on the host PC
will sync automatically between host and container.

To use the container file:

1. Build the container with `podman build -t zephyr-cp-dev -f native_sim_build_Containerfile .`
2. Run/Start the container by running `./native_sim_build_run_container.sh` on the host PC.
   The script will automatically run or start based on whether the container has been run before.
3. Init requirements inside the container with `./native_sim_build_init_container.sh`

To delete the container and cleanup associated files:
```sh
podman ps -a --filter ancestor=zephyr-cp-dev -q | xargs -r podman rm -f
podman rmi zephyr-cp-dev
podman image prune -f
podman rm -f zcp
```

## Running the native simulator

From `ports/zephyr-cp`, run:

```sh
make run-sim
```

`run-sim` starts the native simulator in realtime.
It prints the PTY path to connect to the simulator REPL.
If a local `./CIRCUITPY/` folder exists, its files are used as the simulator's CIRCUITPY drive.

Edit files in `./CIRCUITPY` (for example `code.py`) and rerun `make run-sim` to test changes.

## Shields

Board defaults can be set in `boards/<vendor>/<board>/circuitpython.toml`:

```toml
SHIELDS = ["shield1", "shield2"]
```

For example, `boards/renesas/ek_ra8d1/circuitpython.toml` enables:

```toml
SHIELDS = ["rtkmipilcdb00000be"]
```

You can override shield selection from the command line:

```sh
# Single shield
make BOARD=renesas_ek_ra8d1 SHIELD=rtkmipilcdb00000be

# Multiple shields (comma, semicolon, or space separated)
make BOARD=my_vendor_my_board SHIELDS="shield1,shield2"
```

Behavior and precedence:

- If `SHIELD` or `SHIELDS` is explicitly provided, it overrides board defaults.
- If neither is provided, defaults from `circuitpython.toml` are used.
- Use `SHIELD=` (empty) to disable a board default shield for one build.

## OTA firmware updates over BLE

Boards with Bluetooth and a `slot1_partition` can update CircuitPython
itself over the air. A board opts in by shipping a `sysbuild.conf` in its
board folder:

```
boards/<vendor>/<board>/sysbuild.conf:

SB_CONFIG_BOOTLOADER_MCUBOOT=y
```

That makes sysbuild build the MCUboot bootloader into the board's
`boot_partition` and link the application into `slot0_partition` (it must
link into slot0 — see `nrf54lm20dk` for an example of a board that does
not). The app image is wrapped with an MCUboot header using imgtool —
signed, or hash-only when the board sets `SB_CONFIG_BOOT_SIGNATURE_TYPE_NONE`
(like `nordic_nrf54l15dk` does). The port Kconfig then enables Zephyr's
MCUmgr/SMP server over the Bluetooth SMP service:

- `img upload` writes the uploaded image into slot 1
- `img test`/`img confirm` plus `os reset` reboot into the new image
- the app confirms the running image once it has booted, so an update that
  fails to start is automatically reverted (the `TEST_AND_CONFIRM` flow in
  the reference clients)

The service requires a paired/bonded connection, matching the port's
existing BLE pairing support.

To update a device, flash it with `make BOARD=<board> flash` (which flashes
both MCUboot and the app) and then use an SMP client such as Nordic's open
source [nRF Connect Device Manager](https://github.com/nordicsemi/Android-nRF-Connect-Device-Manager)
apps (the sample app is what's published on the app stores) or the `mcumgr`
CLI, uploading the signed app image (`zephyr.signed.bin` from the build
directory).

`nordic_nrf54l15dk` is the first board with this enabled.

## Testing other boards

[Any Zephyr board](https://docs.zephyrproject.org/latest/boards/index.html#) can
be used with CircuitPython. To test a different board, use `west` directly to
build the board. The build will do its best to support as much as possible. By
default the Zephyr console will be used for output. USB support is limited by
initialization support in `supervisor/usb.c`. Only flash regions not used by
Zephyr are used for CIRCUITPY. A manual `circuitpython` partition can be
specified instead.

For example, to test the `nrf52840dk` board:

```sh
west build -b nrf52840dk/nrf52840
```

This is already supported in `ports/nordic` as `pca10056`.
