from hancho import *
import glob
import pathlib

port_dir = pathlib.Path.cwd().absolute()

circuitpython = load("../../HANCHO.py")

# compile_circuitpython = Rule(
#   desc = "Compile {files_in} -> {files_out}",
#   command = "{compiler} -MMD -c {files_in} {circuitpython_flags} {port_flags} -o {files_out}",
#   files_out = str(top / "build/{swap_ext(files_in, '.o')}"),
#   depfile = "{swap_ext(files_out, '.d')}",
#   circuitpython_flags = f"-I {top}"
# )


def board(board_name, *, external_flash_devices, **kwargs):
    print("board()", glob.glob("*.c"))
    print("hello board")

    board_id = pathlib.Path.cwd().name

    port_flags = []
    port_flags.append(f"-I {port_dir}")
    port_flags.append(f"-isystem {port_dir}/sdk")
    port_flags.append(f"-isystem {port_dir}/sdk/src/rp2_common/cmsis")
    port_flags.append(f"-isystem {port_dir}/sdk_config")
    port_flags.append(f"-I {port_dir}/boards/{board_id}")
    port_flags.append(
        "-DRASPBERRYPI -DPICO_ON_DEVICE=1 -DPICO_NO_BINARY_INFO=0 -DPICO_TIME_DEFAULT_ALARM_POOL_DISABLED=0 -DPICO_DIVIDER_CALL_IDIV0=0 -DPICO_DIVIDER_CALL_LDIV0=0 -DPICO_DIVIDER_HARDWARE=1 -DPICO_DOUBLE_ROM=1 -DPICO_FLOAT_ROM=1 -DPICO_MULTICORE=1 -DPICO_BITS_IN_RAM=0 -DPICO_DIVIDER_IN_RAM=0 -DPICO_DOUBLE_PROPAGATE_NANS=0 -DPICO_DOUBLE_IN_RAM=0 -DPICO_MEM_IN_RAM=0 -DPICO_FLOAT_IN_RAM=0 -DPICO_FLOAT_PROPAGATE_NANS=1 -DPICO_NO_FLASH=0 -DPICO_COPY_TO_RAM=0 -DPICO_DISABLE_SHARED_IRQ_HANDLERS=0 -DPICO_NO_BI_BOOTSEL_VIA_DOUBLE_RESET=0 -DDVI_1BPP_BIT_REVERSE=0"
    )
    port_flags.append(
        "-DCFG_TUSB_OS=OPT_OS_PICO -DTUD_OPT_RP2040_USB_DEVICE_ENUMERATION_FIX=1 -DCFG_TUSB_MCU=OPT_MCU_RP2040 -DCFG_TUD_MIDI_RX_BUFSIZE=128 -DCFG_TUD_CDC_RX_BUFSIZE=256 -DCFG_TUD_MIDI_TX_BUFSIZE=128 -DCFG_TUD_CDC_TX_BUFSIZE=256 -DCFG_TUD_MSC_BUFSIZE=1024"
    )

    for include in (port_dir / "sdk" / "src" / "common").glob("**/include"):
        port_flags.append(f"-isystem {include}")
    for include in (port_dir / "sdk" / "src" / "rp2040").glob("**/include"):
        port_flags.append(f"-isystem {include}")
    for include in (port_dir / "sdk" / "src" / "rp2_common").glob("**/include"):
        port_flags.append(f"-isystem {include}")

    circuitpython.board(
        board_name,
        "cortex-m0+",
        port_flags=" ".join(port_flags),
        flash_filesystem="internal",
        **kwargs,
    )
