import asyncio
import colorlog
import sys
import logging
import os
import pathlib
import tomllib
import yaml
import pickle

import cpbuild

from compat2driver import COMPAT_TO_DRIVER

print("hello zephyr", sys.argv)

# print(os.environ)
cmake_args = {}
for var in sys.argv[1:]:
    key, value = var.split("=", 1)
    cmake_args[key] = value

# Path to ports/zephyr-cp
portdir = pathlib.Path(cmake_args["PORT_SRC_DIR"])

# Path to CP root
srcdir = portdir.parent.parent

# Path to where CMake wants to put our build output.
builddir = pathlib.Path.cwd()

# Path to where CMake puts Zephyr's build output.
zephyrbuilddir = portdir / "build" / "zephyr-cp" / "zephyr"

sys.path.append(str(portdir / "lib/zephyr/scripts/dts/python-devicetree/src/"))
from devicetree import dtlib

compiler = cpbuild.Compiler(srcdir, builddir, cmake_args)

ALWAYS_ON_MODULES = ["sys", "collections"]
DEFAULT_MODULES = ["time", "os", "microcontroller", "struct", "array", "json", "random"]
MPCONFIG_FLAGS = ["ulab", "nvm", "displayio", "warnings", "alarm", "array", "json"]


async def preprocess_and_split_defs(compiler, source_file, build_path, flags):
    build_file = source_file.with_suffix(".pp")
    build_file = build_path / (build_file.relative_to(srcdir))
    await compiler.preprocess(source_file, build_file, flags=flags)
    async with asyncio.TaskGroup() as tg:
        for mode in ("qstr", "module", "root_pointer"):
            split_file = build_file.relative_to(build_path).with_suffix(f".{mode}")
            split_file = build_path / "genhdr" / mode / split_file
            split_file.parent.mkdir(exist_ok=True, parents=True)
            tg.create_task(
                cpbuild.run_command(
                    [
                        "python",
                        srcdir / "py/makeqstrdefs.py",
                        "split",
                        mode,
                        build_file,
                        build_path / "genhdr" / mode,
                        split_file,
                    ],
                    srcdir,
                )
            )


async def collect_defs(mode, build_path):
    output_file = build_path / f"{mode}defs.collected"
    splitdir = build_path / "genhdr" / mode
    await cpbuild.run_command(
        ["cat", "-s", *splitdir.glob(f"**/*.{mode}"), ">", output_file],
        splitdir,
    )
    return output_file


async def generate_qstr_headers(build_path, compiler, flags):
    collected = await collect_defs("qstr", build_path)
    strings = build_path / "genhdr" / "qstrdefs.str.h"
    preprocessed_strings = build_path / "genhdr" / "qstrdefs.str.preprocessed.h"
    preprocessed = build_path / "genhdr" / "qstrdefs.preprocessed.h"
    generated = build_path / "genhdr" / "qstrdefs.generated.h"

    await cpbuild.run_command(
        ["cat", srcdir / "py" / "qstrdefs.h", "|", "sed", "'s/^Q(.*)/\"&\"/'", ">", strings],
        srcdir,
    )
    await compiler.preprocess(strings, preprocessed_strings, flags)
    await cpbuild.run_command(
        ["cat", preprocessed_strings, "|", "sed", "'s/^\"\\(Q(.*)\\)\"/\1/'", ">", preprocessed],
        srcdir,
    )
    await cpbuild.run_command(
        ["python", srcdir / "py" / "makeqstrdata.py", collected, preprocessed, ">", generated],
        srcdir,
    )

    translation = "en_US"
    compression_level = 9

    # TODO: Do this alongside qstr stuff above.
    await cpbuild.run_command(
        [
            "python",
            srcdir / "tools" / "msgfmt.py",
            "-o",
            build_path / f"{translation}.mo",
            srcdir / "locale" / f"{translation}.po",
        ],
        srcdir,
    )

    await cpbuild.run_command(
        [
            "python",
            srcdir / "py" / "maketranslationdata.py",
            "--compression_filename",
            build_path / "genhdr" / "compressed_translations.generated.h",
            "--translation",
            build_path / f"{translation}.mo",
            "--translation_filename",
            build_path / f"translations-{translation}.c",
            "--qstrdefs_filename",
            generated,
            "--compression_level",
            compression_level,
            generated,
        ],
        srcdir,
    )


async def generate_module_header(build_path):
    collected = await collect_defs("module", build_path)
    await cpbuild.run_command(
        [
            "python",
            srcdir / "py" / "makemoduledefs.py",
            collected,
            ">",
            build_path / "genhdr" / "moduledefs.h",
        ],
        srcdir,
    )


async def generate_root_pointer_header(build_path):
    collected = await collect_defs("root_pointer", build_path)
    await cpbuild.run_command(
        [
            "python",
            srcdir / "py" / "make_root_pointers.py",
            collected,
            ">",
            build_path / "genhdr" / "root_pointers.h",
        ],
        srcdir,
    )


CONNECTORS = {
    "mikro-bus": [
        "AN",
        "RST",
        "CS",
        "SCK",
        "MISO",
        "MOSI",
        "PWM",
        "INT",
        "RX",
        "TX",
        "SCL",
        "SDA",
    ],
    "arduino-header-r3": [
        "A0",
        "A1",
        "A2",
        "A3",
        "A4",
        "A5",
        "D0",
        "D1",
        "D2",
        "D3",
        "D4",
        "D5",
        "D6",
        "D7",
        "D8",
        "D9",
        "D10",
        "D11",
        "D12",
        "D13",
        "D14",
        "D15",
    ],
    "adafruit-feather-header": [
        "A0",
        "A1",
        "A2",
        "A3",
        "A4",
        "A5",
        "SCK",
        "MOSI",
        "MISO",
        "RX",
        "TX",
        "D4",
        "SDA",
        "SCL",
        "D5",
        "D6",
        "D9",
        "D10",
        "D11",
        "D12",
        "D13",
    ],
    "renesas,ra-gpio-mipi-header": [
        "IIC_SDA",
        "DISP_BLEN",
        "IIC_SCL",
        "DISP_INT",
        "DISP_RST",
    ],
}

MANUAL_COMPAT_TO_DRIVER = {
    "renesas_ra_nv_flash": "flash",
}

TINYUSB_SETTINGS = {
    "": {
        "CFG_TUSB_MCU": "OPT_MCU_MIMXRT10XX",
        "CFG_TUD_CDC_RX_BUFSIZE": 640,
        "CFG_TUD_CDC_TX_BUFSIZE": 512,
    },
    "stm32u575xx": {"CFG_TUSB_MCU": "OPT_MCU_STM32U5"},
    "nrf52840": {"CFG_TUSB_MCU": "OPT_MCU_NRF5X"},
    "nrf5340": {"CFG_TUSB_MCU": "OPT_MCU_NRF5X"},
    # "r7fa8d1bhecbd": {"CFG_TUSB_MCU": "OPT_MCU_RAXXX", "USB_HIGHSPEED": "1", "USBHS_USB_INT_RESUME_IRQn": "54", "USBFS_INT_IRQn": "54", "CIRCUITPY_USB_DEVICE_INSTANCE": "1"},
    # ifeq ($(CHIP_FAMILY),$(filter $(CHIP_FAMILY),MIMXRT1011 MIMXRT1015))
    # CFLAGS += -DCFG_TUD_MIDI_RX_BUFSIZE=512 -DCFG_TUD_MIDI_TX_BUFSIZE=64 -DCFG_TUD_MSC_BUFSIZE=512
    # else
    # CFLAGS += -DCFG_TUD_MIDI_RX_BUFSIZE=512 -DCFG_TUD_MIDI_TX_BUFSIZE=512 -DCFG_TUD_MSC_BUFSIZE=1024
    # endif
}

TINYUSB_SOURCE = {
    "stm32u575xx": [
        "src/portable/st/stm32_fsdev/dcd_stm32_fsdev.c",
        "src/portable/synopsys/dwc2/dcd_dwc2.c",
        "src/portable/synopsys/dwc2/hcd_dwc2.c",
        "src/portable/synopsys/dwc2/dwc2_common.c",
    ],
    "nrf52840": [
        "src/portable/nordic/nrf5x/dcd_nrf5x.c",
    ],
    "nrf5340": [
        "src/portable/nordic/nrf5x/dcd_nrf5x.c",
    ],
    # "r7fa8d1bhecbd": [
    #     "src/portable/renesas/rusb2/dcd_rusb2.c",
    #     "src/portable/renesas/rusb2/hcd_rusb2.c",
    #     "src/portable/renesas/rusb2/rusb2_common.c",
    # ],
}


async def build_circuitpython():
    circuitpython_flags = ["-DCIRCUITPY"]
    port_flags = []
    # usb_num_endpoint_pairs = 8
    usb_num_endpoint_pairs = 0
    enable_mpy_native = False
    full_build = False
    usb_host = False
    tusb_mem_align = 4
    board = cmake_args["BOARD_ALIAS"]
    if not board:
        board = cmake_args["BOARD"]
    for module in ALWAYS_ON_MODULES:
        circuitpython_flags.append(f"-DCIRCUITPY_{module.upper()}=1")
    circuitpython_flags.append(f"-DCIRCUITPY_ENABLE_MPY_NATIVE={1 if enable_mpy_native else 0}")
    circuitpython_flags.append(f"-DCIRCUITPY_FULL_BUILD={1 if full_build else 0}")
    circuitpython_flags.append(f"-DCIRCUITPY_USB_HOST={1 if usb_host else 0}")
    circuitpython_flags.append(f'-DCIRCUITPY_BOARD_ID=\\"{board}\\"')
    circuitpython_flags.append(f"-DCIRCUITPY_TUSB_MEM_ALIGN={tusb_mem_align}")
    circuitpython_flags.append("-DINTERNAL_FLASH_FILESYSTEM")
    circuitpython_flags.append("-DLONGINT_IMPL_MPZ")
    circuitpython_flags.append("-DCIRCUITPY_SSL_MBEDTLS")
    circuitpython_flags.append('-DFFCONF_H=\\"lib/oofatfs/ffconf.h\\"')
    circuitpython_flags.extend(("-I", srcdir))
    circuitpython_flags.extend(("-I", srcdir / "lib/tinyusb/src"))
    circuitpython_flags.extend(("-I", srcdir / "supervisor/shared/usb"))
    circuitpython_flags.extend(("-I", builddir))
    circuitpython_flags.extend(("-I", portdir))
    # circuitpython_flags.extend(("-I", srcdir / "ports" / port / "peripherals"))

    kwargs = {}
    board_dir = srcdir / "boards" / board
    if not board_dir.exists():
        print("Generating board info")
        board_dir = builddir / "board"
        # Auto generate board files from device tree.

        runners = zephyrbuilddir / "runners.yaml"
        runners = yaml.safe_load(runners.read_text())
        print(runners)
        zephyr_board_dir = pathlib.Path(runners["config"]["board_dir"])
        board_yaml = zephyr_board_dir / "board.yml"
        board_yaml = yaml.safe_load(board_yaml.read_text())
        print(board_yaml)
        vendor_index = zephyr_board_dir.parent / "index.rst"
        if vendor_index.exists():
            vendor_index = vendor_index.read_text()
            vendor_index = vendor_index.split("\n")
            vendor_name = vendor_index[2].strip()
        else:
            vendor_name = board_yaml["board"]["vendor"]
        soc_name = board_yaml["board"]["socs"][0]["name"]
        board_name = board_yaml["board"]["full_name"]
        # board_id_yaml = zephyr_board_dir / (zephyr_board_dir.name + ".yaml")
        # board_id_yaml = yaml.safe_load(board_id_yaml.read_text())
        # print(board_id_yaml)
        # board_name = board_id_yaml["name"]

        dts = zephyrbuilddir / "zephyr.dts"
        print(dts)
        edt_pickle = dtlib.DT(dts)
        node2alias = {}
        for alias in edt_pickle.alias2node:
            node = edt_pickle.alias2node[alias]
            if node not in node2alias:
                node2alias[node] = []
            node2alias[node].append(alias)
        ioports = {}
        all_ioports = []
        board_names = {}
        flashes = []
        rams = []
        status_led = None
        path2chosen = {}
        chosen2path = {}
        for k in edt_pickle.root.nodes["chosen"].props:
            value = edt_pickle.root.nodes["chosen"].props[k]
            path2chosen[value.to_path()] = k
            chosen2path[k] = value.to_path()
        remaining_nodes = set([edt_pickle.root])
        while remaining_nodes:
            node = remaining_nodes.pop()
            remaining_nodes.update(node.nodes.values())
            gpio = node.props.get("gpio-controller", False)
            gpio_map = node.props.get("gpio-map", [])
            status = node.props.get("status", None)
            if status is None:
                status = "okay"
            else:
                status = status.to_string()

            compatible = []
            if "compatible" in node.props:
                compatible = node.props["compatible"].to_strings()
            print(node.name, status)
            chosen = None
            if node in path2chosen:
                chosen = path2chosen[node]
                print(" chosen:", chosen)
            for c in compatible:
                underscored = c.replace(",", "_").replace("-", "_")
                driver = COMPAT_TO_DRIVER.get(underscored, None)
                if "mmio" in c:
                    print(" ", c, node.labels, node.props)
                    address, size = node.props["reg"].to_nums()
                    end = address + size
                    if chosen == "zephyr,sram":
                        start = "z_mapped_end"
                    elif "zephyr,memory-region" in node.props:
                        start = "__" + node.props["zephyr,memory-region"].to_string() + "_end"
                    else:
                        # Check to see if the chosen sram is a subset of this region. If it is,
                        # then do as above for a smaller region and assume the rest is reserved.
                        chosen_sram = chosen2path["zephyr,sram"]
                        chosen_address, chosen_size = chosen_sram.props["reg"].to_nums()
                        chosen_end = chosen_address + chosen_size
                        if address <= chosen_address <= end and address <= chosen_end <= end:
                            start = "z_mapped_end"
                            address = chosen_address
                            size = chosen_size
                            end = chosen_end
                        else:
                            start = address
                    print(node.props["reg"])
                    print("  bounds", start, hex(end))
                    info = (node.labels[0], start, end, size, node.path)
                    if chosen == "zephyr,sram":
                        rams.insert(0, info)
                    else:
                        rams.append(info)
                if not driver:
                    driver = MANUAL_COMPAT_TO_DRIVER.get(underscored, None)
                print(" ", underscored, driver)
                if not driver:
                    continue
                if (
                    driver == "flash"
                    and status == "okay"
                    and not chosen
                    and ("write-block-size" in node.props or "sfdp-bfp" in node.props)
                ):
                    # Skip chosen nodes because they are used by Zephyr.
                    # Skip nodes without "write-block-size" because it could be a controller.
                    flashes.append(f"DEVICE_DT_GET(DT_NODELABEL({node.labels[0]}))")
                if driver == "usb/udc" and status == "okay":
                    print("found usb!")
                    if soc_name not in TINYUSB_SETTINGS:
                        print(f"Missing tinyusb settings for {soc_name}")
                    else:
                        kwargs["usb_device"] = True
                        props = node.props
                        if "num-bidir-endpoints" not in props:
                            props = node.parent.props
                        usb_num_endpoint_pairs = 0
                        if "num-bidir-endpoints" in props:
                            usb_num_endpoint_pairs = props["num-bidir-endpoints"].to_num()
                        single_direction_endpoints = []
                        for d in ("in", "out"):
                            eps = f"num-{d}-endpoints"
                            single_direction_endpoints.append(
                                props[eps].to_num() if eps in props else 0
                            )
                        # Count separate in/out pairs as bidirectional.
                        usb_num_endpoint_pairs += min(single_direction_endpoints)
                if driver.startswith("wifi") and status == "okay":
                    kwargs["wifi"] = True
                    kwargs["socketpool"] = True
                    kwargs["ssl"] = True

            if gpio:
                if "ngpios" in node.props:
                    ngpios = node.props["ngpios"].to_num()
                else:
                    ngpios = 32
                all_ioports.append(node.labels[0])
                if status == "okay":
                    ioports[node.labels[0]] = set(range(0, ngpios))
            if gpio_map:
                print("markers", compatible)
                i = 0
                for offset, t, label in gpio_map._markers:
                    if not label:
                        continue
                    num = int.from_bytes(gpio_map.value[offset + 4 : offset + 8], "big")
                    print(label, num, i)
                    print(CONNECTORS[compatible[0]][i])
                    if (label, num) not in board_names:
                        board_names[(label, num)] = []
                    board_names[(label, num)].append(CONNECTORS[compatible[0]][i])
                    i += 1
            if "gpio-leds" in compatible:
                for led in node.nodes:
                    led = node.nodes[led]
                    props = led.props
                    ioport = props["gpios"]._markers[1][2]
                    num = int.from_bytes(props["gpios"].value[4:8], "big")
                    if "label" in props:
                        if (ioport, num) not in board_names:
                            board_names[(ioport, num)] = []
                        board_names[(ioport, num)].append(props["label"].to_string())
                    if led in node2alias:
                        if (ioport, num) not in board_names:
                            board_names[(ioport, num)] = []
                        if "led0" in node2alias[led]:
                            board_names[(ioport, num)].append("LED")
                            status_led = (ioport, num)
                        board_names[(ioport, num)].extend(node2alias[led])

            if "gpio-keys" in compatible:
                for key in node.nodes:
                    props = node.nodes[key].props
                    ioport = props["gpios"]._markers[1][2]
                    num = int.from_bytes(props["gpios"].value[4:8], "big")

                    if (ioport, num) not in board_names:
                        board_names[(ioport, num)] = []
                    board_names[(ioport, num)].append(props["label"].to_string())
                    if key in node2alias:
                        if "sw0" in node2alias[key]:
                            board_names[(ioport, num)].append("BUTTON")
                        board_names[(ioport, num)].extend(node2alias[key])

        a, b = all_ioports[:2]
        i = 0
        while a[i] == b[i]:
            i += 1
        shared_prefix = a[:i]
        for ioport in ioports:
            if not ioport.startswith(shared_prefix):
                shared_prefix = ""
                break

        pin_defs = []
        pin_declarations = ["#pragma once"]
        mcu_pin_mapping = []
        board_pin_mapping = []
        for ioport in sorted(ioports.keys()):
            for num in ioports[ioport]:
                pin_object_name = f"P{ioport[len(shared_prefix):].upper()}_{num:02d}"
                if status_led and (ioport, num) == status_led:
                    status_led = pin_object_name
                pin_defs.append(
                    f"const mcu_pin_obj_t pin_{pin_object_name} = {{ .base.type = &mcu_pin_type, .port = DEVICE_DT_GET(DT_NODELABEL({ioport})), .number = {num}}};"
                )
                pin_declarations.append(f"extern const mcu_pin_obj_t pin_{pin_object_name};")
                mcu_pin_mapping.append(
                    f"{{ MP_ROM_QSTR(MP_QSTR_{pin_object_name}), MP_ROM_PTR(&pin_{pin_object_name}) }},"
                )
                board_pin_names = board_names.get((ioport, num), [])

                for board_pin_name in board_pin_names:
                    board_pin_name = board_pin_name.upper().replace(" ", "_").replace("-", "_")
                    board_pin_mapping.append(
                        f"{{ MP_ROM_QSTR(MP_QSTR_{board_pin_name}), MP_ROM_PTR(&pin_{pin_object_name}) }},"
                    )
                    print(" ", board_pin_name)

        pin_defs = "\n".join(pin_defs)
        pin_declarations = "\n".join(pin_declarations)
        board_pin_mapping = "\n    ".join(board_pin_mapping)
        mcu_pin_mapping = "\n    ".join(mcu_pin_mapping)

        usb_settings = ""
        if usb_num_endpoint_pairs > 0:
            usb_settings = f"""
            USB_VID = 0x239A
            USB_PID = 0x0013
            USB_PRODUCT = "{board_name}"
            USB_MANUFACTURER = "{vendor_name}"
            """

        board_dir.mkdir(exist_ok=True, parents=True)
        toml = board_dir / "mpconfigboard.toml"
        toml.write_text(usb_settings)
        header = board_dir / "mpconfigboard.h"
        if status_led:
            status_led = f"#define MICROPY_HW_LED_STATUS (&pin_{status_led})\n"
        else:
            status_led = ""
        print(flashes)
        print(rams)
        ram_list = []
        ram_externs = []
        max_size = 0
        for ram in rams:
            device, start, end, size, path = ram
            max_size = max(max_size, size)
            if isinstance(start, str):
                ram_externs.append(f"extern uint32_t {start};")
                start = "&" + start
            else:
                start = f"(uint32_t*) 0x{start:08x}"
            ram_list.append(f"    {start}, (uint32_t*) 0x{end:08x}, // {path}")
        ram_list = "\n".join(ram_list)
        ram_externs = "\n".join(ram_externs)
        header.write_text(
            f"""#pragma once

#define MICROPY_HW_BOARD_NAME       "{board_name}"
#define MICROPY_HW_MCU_NAME         "{soc_name}"
#define CIRCUITPY_RAM_DEVICE_COUNT  {len(rams)}
{status_led}
            """
        )
        pins = board_dir / "autogen-pins.h"
        pins.write_text(pin_declarations)

        pins = board_dir / "pins.c"
        pins.write_text(
            f"""
        // This file is autogenerated by build_circuitpython.py

#include "shared-bindings/board/__init__.h"

#include <stdint.h>

#include "py/obj.h"
#include "py/mphal.h"

const struct device* const flashes[] = {{ {", ".join(flashes)} }};
const int circuitpy_flash_device_count = {len(flashes)};

{ram_externs}
const uint32_t* const ram_bounds[] = {{
{ram_list}
}};
const size_t circuitpy_max_ram_size = {max_size};

{pin_defs}

static const mp_rom_map_elem_t mcu_pin_globals_table[] = {{
    {mcu_pin_mapping}
}};
MP_DEFINE_CONST_DICT(mcu_pin_globals, mcu_pin_globals_table);

static const mp_rom_map_elem_t board_module_globals_table[] = {{
    CIRCUITPYTHON_BOARD_DICT_STANDARD_ITEMS

    {board_pin_mapping}

    // {{ MP_ROM_QSTR(MP_QSTR_UART), MP_ROM_PTR(&board_uart_obj) }},
}};

MP_DEFINE_CONST_DICT(board_module_globals, board_module_globals_table);
"""
        )
    else:
        print(board_dir, "exists")

    circuitpython_flags.extend(("-I", board_dir))
    # circuitpython_flags.extend(("-I", build_path / board_id))

    genhdr = builddir / "genhdr"
    genhdr.mkdir(exist_ok=True, parents=True)
    version_header = genhdr / "mpversion.h"
    await cpbuild.run_command(
        [
            "python",
            srcdir / "py" / "makeversionhdr.py",
            version_header,
            "&&",
            "touch",
            version_header,
        ],
        srcdir,
        check_hash=[version_header],
    )

    supervisor_source = [
        "main.c",
        "extmod/vfs_fat.c",
        "lib/tlsf/tlsf.c",
        portdir / "background.c",
        portdir / "common-hal/microcontroller/__init__.c",
        portdir / "common-hal/microcontroller/Pin.c",
        portdir / "common-hal/microcontroller/Processor.c",
        portdir / "common-hal/os/__init__.c",
        "supervisor/stub/misc.c",
        "shared/readline/readline.c",
        "shared/runtime/context_manager_helpers.c",
        "shared/runtime/pyexec.c",
        "shared/runtime/interrupt_char.c",
        "shared/runtime/stdout_helpers.c",
        "shared/runtime/sys_stdio_mphal.c",
        "shared-bindings/board/__init__.c",
        "shared-bindings/supervisor/Runtime.c",
        "shared-bindings/microcontroller/Pin.c",
        "shared-bindings/util.c",
        "shared-module/board/__init__.c",
        "extmod/vfs_reader.c",
        "extmod/vfs_blockdev.c",
        "extmod/vfs_fat_file.c",
        board_dir / "pins.c",
    ]
    top = srcdir
    supervisor_source = [pathlib.Path(p) for p in supervisor_source]
    supervisor_source.extend(top.glob("supervisor/shared/*.c"))
    supervisor_source.append(top / "supervisor/shared/translate/translate.c")
    # if web_workflow:
    #     supervisor_source.extend(top.glob("supervisor/shared/web_workflow/*.c"))

    # Load the toml settings
    kwargs["busio"] = True
    kwargs["digitalio"] = True
    with open(board_dir / "mpconfigboard.toml", "rb") as f:
        mpconfigboard = tomllib.load(f)

    circuitpython_flags.append(f"-DCIRCUITPY_TINYUSB={1 if usb_num_endpoint_pairs > 0 else 0}")
    circuitpython_flags.append(f"-DCIRCUITPY_USB_DEVICE={1 if usb_num_endpoint_pairs > 0 else 0}")

    tinyusb_files = []
    if usb_num_endpoint_pairs > 0:
        for setting in TINYUSB_SETTINGS[soc_name]:
            circuitpython_flags.append(f"-D{setting}={TINYUSB_SETTINGS[soc_name][setting]}")
        tinyusb_files.extend((top / "lib" / "tinyusb" / path for path in TINYUSB_SOURCE[soc_name]))
        for macro in ("USB_PID", "USB_VID"):
            circuitpython_flags.append(f"-D{macro}=0x{mpconfigboard.get(macro):04x}")
        for macro, limit in (("USB_PRODUCT", 16), ("USB_MANUFACTURER", 8)):
            circuitpython_flags.append(f"-D{macro}='\"{mpconfigboard.get(macro)}\"'")
            circuitpython_flags.append(
                f"-D{macro}_{limit}='\"{mpconfigboard.get(macro[:limit])}\"'"
            )

        usb_interface_name = "CircuitPython"

        circuitpython_flags.append("-DCFG_TUSB_OS=OPT_OS_ZEPHYR")
        circuitpython_flags.append(f"-DUSB_INTERFACE_NAME='\"{usb_interface_name}\"'")
        circuitpython_flags.append(f"-DUSB_NUM_ENDPOINT_PAIRS={usb_num_endpoint_pairs}")
        for direction in ("IN", "OUT"):
            circuitpython_flags.append(f"-DUSB_NUM_{direction}_ENDPOINTS={usb_num_endpoint_pairs}")
        # USB is special because it doesn't have a matching module.
        msc_enabled = True
        if msc_enabled:
            # force storage on
            kwargs["storage"] = True
            kwargs["usb_msc"] = True
            circuitpython_flags.append("-DCFG_TUD_MSC_BUFSIZE=1024")
            circuitpython_flags.append("-DCIRCUITPY_USB_MSC_ENABLED_DEFAULT=1")
        circuitpython_flags.append(f"-DCIRCUITPY_USB_MSC={1 if msc_enabled else 0}")
        if "usb_cdc" not in kwargs:
            kwargs["usb_cdc"] = True
        if kwargs["usb_cdc"]:
            tinyusb_files.extend(top.glob("lib/tinyusb/*.c"))
            circuitpython_flags.append("-DCFG_TUD_CDC_RX_BUFSIZE=640")
            circuitpython_flags.append("-DCFG_TUD_CDC_TX_BUFSIZE=512")
            circuitpython_flags.append("-DCFG_TUD_CDC=2")
            circuitpython_flags.append("-DCIRCUITPY_USB_CDC_CONSOLE_ENABLED_DEFAULT=1")
            circuitpython_flags.append("-DCIRCUITPY_USB_CDC_DATA_ENABLED_DEFAULT=0")

        if "usb_hid_enabled_default" not in kwargs:
            kwargs["usb_hid_enabled_default"] = usb_num_endpoint_pairs >= 5
        if "usb_midi_enabled_default" not in kwargs:
            kwargs["usb_midi_enabled_default"] = usb_num_endpoint_pairs >= 8

        tinyusb_files.extend(
            (top / "lib/tinyusb/src/common/tusb_fifo.c", top / "lib/tinyusb/src/tusb.c")
        )
        supervisor_source.extend(
            (portdir / "supervisor/usb.c", top / "supervisor/shared/usb/usb.c")
        )

    if kwargs.get("usb_device", False):
        tinyusb_files.extend(
            (
                top / "lib/tinyusb/src/class/cdc/cdc_device.c",
                top / "lib/tinyusb/src/device/usbd.c",
                top / "lib/tinyusb/src/device/usbd_control.c",
            )
        )
        supervisor_source.extend(
            (top / "supervisor/shared/usb/usb_desc.c", top / "supervisor/shared/usb/usb_device.c")
        )

    if kwargs.get("usb_cdc", False):
        supervisor_source.extend(
            (
                top / "shared-bindings/usb_cdc/__init__.c",
                top / "shared-bindings/usb_cdc/Serial.c",
                top / "shared-module/usb_cdc/__init__.c",
                top / "shared-module/usb_cdc/Serial.c",
            )
        )

    # ifeq ($(CIRCUITPY_USB_HID), 1)
    #   SRC_SUPERVISOR += \
    #     lib/tinyusb/src/class/hid/hid_device.c \
    #     shared-bindings/usb_hid/__init__.c \
    #     shared-bindings/usb_hid/Device.c \
    #     shared-module/usb_hid/__init__.c \
    #     shared-module/usb_hid/Device.c \

    # endif

    # ifeq ($(CIRCUITPY_USB_MIDI), 1)
    #   SRC_SUPERVISOR += \
    #     lib/tinyusb/src/class/midi/midi_device.c \
    #     shared-bindings/usb_midi/__init__.c \
    #     shared-bindings/usb_midi/PortIn.c \
    #     shared-bindings/usb_midi/PortOut.c \
    #     shared-module/usb_midi/__init__.c \
    #     shared-module/usb_midi/PortIn.c \
    #     shared-module/usb_midi/PortOut.c \

    # endif

    if kwargs.get("usb_msc", False):
        tinyusb_files.append(top / "lib/tinyusb/src/class/msc/msc_device.c")
        supervisor_source.append(top / "supervisor/shared/usb/usb_msc_flash.c")

    # ifeq ($(CIRCUITPY_USB_VIDEO), 1)
    #   SRC_SUPERVISOR += \
    #     shared-bindings/usb_video/__init__.c \
    #     shared-module/usb_video/__init__.c \
    #     shared-bindings/usb_video/USBFramebuffer.c \
    #     shared-module/usb_video/USBFramebuffer.c \
    #     lib/tinyusb/src/class/video/video_device.c \

    #   CFLAGS += -DCFG_TUD_VIDEO=1 -DCFG_TUD_VIDEO_STREAMING=1 -DCFG_TUD_VIDEO_STREAMING_EP_BUFSIZE=256 -DCFG_TUD_VIDEO_STREAMING_BULK=1
    # endif

    # ifeq ($(CIRCUITPY_USB_VENDOR), 1)
    #   SRC_SUPERVISOR += \
    #     lib/tinyusb/src/class/vendor/vendor_device.c \

    # endif

    # ifeq ($(CIRCUITPY_TINYUSB_HOST), 1)
    #   SRC_SUPERVISOR += \
    #     lib/tinyusb/src/host/hub.c \
    #     lib/tinyusb/src/host/usbh.c \

    # endif

    # ifeq ($(CIRCUITPY_USB_KEYBOARD_WORKFLOW), 1)
    #   SRC_SUPERVISOR += \
    #     lib/tinyusb/src/class/hid/hid_host.c \
    #     supervisor/shared/usb/host_keyboard.c \

    # endif

    if kwargs.get("ssl", False):
        # TODO: Figure out how to get these paths from zephyr
        circuitpython_flags.append('-DMBEDTLS_CONFIG_FILE=\\"config-tls-generic.h\\"')
        circuitpython_flags.extend(
            ("-isystem", portdir / "modules" / "crypto" / "tinycrypt" / "lib" / "include")
        )
        circuitpython_flags.extend(
            ("-isystem", portdir / "modules" / "crypto" / "mbedtls" / "include")
        )
        circuitpython_flags.extend(
            ("-isystem", portdir / "modules" / "crypto" / "mbedtls" / "configs")
        )
        circuitpython_flags.extend(
            ("-isystem", portdir / "modules" / "crypto" / "mbedtls" / "include")
        )
        circuitpython_flags.extend(
            ("-isystem", portdir / "lib" / "zephyr" / "modules" / "mbedtls" / "configs")
        )
        supervisor_source.append(top / "lib" / "mbedtls_config" / "crt_bundle.c")

    # Make sure all modules have a setting by filling in defaults.
    hal_source = []
    for module in sorted(top.glob("shared-bindings/*")):
        if not module.is_dir():
            continue
        enabled = kwargs.get(module.name, module.name in DEFAULT_MODULES)
        kwargs[module.name] = enabled
        print(f"Module {module.name} enabled: {enabled}")
        circuitpython_flags.append(
            f"-DCIRCUITPY_{module.name.upper()}={1 if kwargs[module.name] else 0}"
        )

        if enabled:
            hal_source.extend(top.glob(f"ports/zephyr-cp/common-hal/{module.name}/*.c"))
            hal_source.extend(top.glob(f"shared-bindings/{module.name}/*.c"))
            hal_source.extend(top.glob(f"shared-module/{module.name}/*.c"))

    for mpflag in MPCONFIG_FLAGS:
        enabled = mpflag in DEFAULT_MODULES
        circuitpython_flags.append(f"-DCIRCUITPY_{mpflag.upper()}={1 if enabled else 0}")

    source_files = supervisor_source + hal_source + ["extmod/vfs.c"]
    assembly_files = []
    for file in top.glob("py/*.c"):
        source_files.append(file)
    qstr_flags = "-DNO_QSTR"
    async with asyncio.TaskGroup() as tg:
        extra_source_flags = {}
        for source_file in source_files:
            tg.create_task(
                preprocess_and_split_defs(
                    compiler,
                    top / source_file,
                    builddir,
                    [qstr_flags, *circuitpython_flags, *port_flags],
                ),
                name=f"Split defs {source_file}",
            )

        if kwargs.get("ssl", False):
            crt_bundle = builddir / "x509_crt_bundle.S"
            roots_pem = srcdir / "lib/certificates/data/roots.pem"
            generator = srcdir / "tools/gen_crt_bundle.py"
            tg.create_task(
                cpbuild.run_command(
                    [
                        "python",
                        generator,
                        "-i",
                        roots_pem,
                        "-o",
                        crt_bundle,
                        "--asm",
                    ],
                    srcdir,
                )
            )
            assembly_files.append(crt_bundle)

    async with asyncio.TaskGroup() as tg:
        board_build = builddir
        tg.create_task(
            generate_qstr_headers(
                board_build, compiler, [qstr_flags, *circuitpython_flags, *port_flags]
            )
        )
        tg.create_task(generate_module_header(board_build))
        tg.create_task(generate_root_pointer_header(board_build))

    # This file is generated by the QSTR/translation process.
    translation = "en_US"
    source_files.append(builddir / f"translations-{translation}.c")
    # These files don't include unique QSTRs. They just need to be compiled.
    source_files.append(portdir / "supervisor" / "flash.c")
    source_files.append(portdir / "supervisor" / "port.c")
    source_files.append(portdir / "supervisor" / "serial.c")
    source_files.append(srcdir / "lib" / "oofatfs" / "ff.c")
    source_files.append(srcdir / "lib" / "oofatfs" / "ffunicode.c")
    source_files.append(srcdir / "extmod" / "vfs_fat_diskio.c")
    source_files.append(srcdir / "shared/timeutils/timeutils.c")
    source_files.append(srcdir / "shared-module/time/__init__.c")
    source_files.append(srcdir / "shared-module/os/__init__.c")
    source_files.append(srcdir / "shared-module/supervisor/__init__.c")
    # source_files.append(srcdir / "ports" / port / "peripherals" / "nrf" / "nrf52840" / "pins.c")

    assembly_files.append(srcdir / "ports/nordic/supervisor/cpu.s")

    source_files.extend(assembly_files)

    source_files.extend(tinyusb_files)

    objects = []
    async with asyncio.TaskGroup() as tg:
        for source_file in source_files:
            source_file = top / source_file
            build_file = source_file.with_suffix(".o")
            object_file = builddir / (build_file.relative_to(top))
            objects.append(object_file)
            tg.create_task(
                compiler.compile(source_file, object_file, [*circuitpython_flags, *port_flags])
            )

    await compiler.archive(objects, pathlib.Path(cmake_args["OUTPUT_FILE"]))


async def main():
    try:
        await build_circuitpython()
    except* RuntimeError as e:
        sys.exit(len(e.exceptions))


handler = colorlog.StreamHandler()
handler.setFormatter(colorlog.ColoredFormatter("%(log_color)s%(levelname)s:%(name)s:%(message)s"))

logging.basicConfig(level=logging.INFO, handlers=[handler])

asyncio.run(main())
