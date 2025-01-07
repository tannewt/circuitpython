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
zephyrbuilddir = portdir / "build" / "zephyr"

sys.path.append(str(portdir / "lib/zephyr/scripts/dts/python-devicetree/src/"))
from devicetree import dtlib

compiler = cpbuild.Compiler(srcdir, builddir, cmake_args)

ALWAYS_ON_MODULES = ["sys"]
DEFAULT_MODULES = []
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
}


async def build_circuitpython():
    circuitpython_flags = ["-DCIRCUITPY"]
    soc_directory = pathlib.Path(cmake_args["SOC_DIRECTORIES"])
    while soc_directory.parent.name != "soc":
        soc_directory = soc_directory.parent
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
    circuitpython_flags.append(f"-DCIRCUITPY_TINYUSB={1 if usb_num_endpoint_pairs > 0 else 0}")
    circuitpython_flags.append(f"-DCIRCUITPY_USB_DEVICE={1 if usb_num_endpoint_pairs > 0 else 0}")
    circuitpython_flags.append(f"-DCIRCUITPY_ENABLE_MPY_NATIVE={1 if enable_mpy_native else 0}")
    circuitpython_flags.append(f"-DCIRCUITPY_FULL_BUILD={1 if full_build else 0}")
    circuitpython_flags.append(f"-DCIRCUITPY_USB_HOST={1 if usb_host else 0}")
    circuitpython_flags.append(f'-DCIRCUITPY_BOARD_ID=\\"{board}\\"')
    circuitpython_flags.append(f"-DCIRCUITPY_TUSB_MEM_ALIGN={tusb_mem_align}")
    circuitpython_flags.append("-DINTERNAL_FLASH_FILESYSTEM")
    circuitpython_flags.append('-DFFCONF_H=\\"lib/oofatfs/ffconf.h\\"')
    circuitpython_flags.extend(("-I", srcdir))
    circuitpython_flags.extend(("-I", srcdir / "lib/tinyusb/src"))
    circuitpython_flags.extend(("-I", srcdir / "supervisor/shared/usb"))
    circuitpython_flags.extend(("-I", builddir))
    circuitpython_flags.extend(("-I", portdir))
    # circuitpython_flags.extend(("-I", srcdir / "ports" / port / "peripherals"))

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
        soc_name = board_yaml["board"]["socs"][0]["name"]
        board_name = board_yaml["board"]["full_name"]
        # board_id_yaml = zephyr_board_dir / (zephyr_board_dir.name + ".yaml")
        # board_id_yaml = yaml.safe_load(board_id_yaml.read_text())
        # print(board_id_yaml)
        # board_name = board_id_yaml["name"]

        dts = portdir / "build" / "zephyr" / "zephyr.dts"
        edt_pickle = dtlib.DT(dts)
        node2alias = {}
        for alias in edt_pickle.alias2node:
            node = edt_pickle.alias2node[alias]
            if node not in node2alias:
                node2alias[node] = []
            node2alias[node].append(alias)
        ioports = {}
        board_names = {}
        status_led = None
        remaining_nodes = set([edt_pickle.root])
        while remaining_nodes:
            node = remaining_nodes.pop()
            remaining_nodes.update(node.nodes.values())
            gpio = node.props.get("gpio-controller", False)
            gpio_map = node.props.get("gpio-map", [])

            compatible = []
            if "compatible" in node.props:
                compatible = node.props["compatible"].to_strings()
            if gpio:
                if "ngpios" in node.props:
                    ngpios = node.props["ngpios"].to_num()
                else:
                    ngpios = 32
                ioports[node.labels[0]] = set(range(0, ngpios))
            if gpio_map:
                print("markers", compatible)
                i = 0
                for offset, t, label in gpio_map._markers:
                    if not label:
                        continue
                    num = int.from_bytes(gpio_map.value[offset + 4 : offset + 8], "big")
                    print(label, num)
                    board_names[(label, num)] = CONNECTORS[compatible[0]][i]
                    i += 1
            if "gpio-leds" in compatible:
                for led in node.nodes:
                    props = node.nodes[led].props
                    ioport = props["gpios"]._markers[1][2]
                    num = int.from_bytes(props["gpios"].value[4:8], "big")
                    board_names[(ioport, num)] = props["label"].to_string()
                    if led in node2alias:
                        if "led0" in node2alias[led]:
                            board_names[(ioport, num)] = "LED"
                            board_names[(ioport, num)] = "LED0"
                            status_led = (ioport, num)

            if "gpio-keys" in compatible:
                for key in node.nodes:
                    props = node.nodes[key].props
                    ioport = props["gpios"]._markers[1][2]
                    num = int.from_bytes(props["gpios"].value[4:8], "big")
                    board_names[(ioport, num)] = props["label"].to_string()
                    if key in node2alias:
                        if "sw0" in node2alias[key]:
                            board_names[(ioport, num)] = "BUTTON"
                            board_names[(ioport, num)] = "SW0"

        a, b = list(ioports.keys())[:2]
        i = 0
        while a[i] == b[i]:
            i += 1
        shared_prefix = a[:i]
        for ioport in ioports:
            if not ioport.startswith(shared_prefix):
                shared_prefix = ""
                break

        pin_defs = []
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
                mcu_pin_mapping.append(
                    f"{{ MP_ROM_QSTR(MP_QSTR_{pin_object_name}), MP_ROM_PTR(&pin_{pin_object_name}) }},"
                )
                board_pin_name = board_names.get((ioport, num), None)

                if board_pin_name:
                    board_pin_name = board_pin_name.upper().replace(" ", "_")
                    board_pin_mapping.append(
                        f"{{ MP_ROM_QSTR(MP_QSTR_{board_pin_name}), MP_ROM_PTR(&pin_{pin_object_name}) }},"
                    )
                    print(" ", board_pin_name)

        pin_defs = "\n".join(pin_defs)
        board_pin_mapping = "\n    ".join(board_pin_mapping)
        mcu_pin_mapping = "\n    ".join(mcu_pin_mapping)

        board_dir.mkdir(exist_ok=True, parents=True)
        toml = board_dir / "mpconfigboard.toml"
        toml.write_text("")
        header = board_dir / "mpconfigboard.h"
        if status_led:
            status_led = f"#define MICROPY_HW_LED_STATUS (&pin_{status_led})\n"
        else:
            status_led = ""
        header.write_text(
            f"""#pragma once

#define MICROPY_HW_BOARD_NAME       "{board_name}"
#define MICROPY_HW_MCU_NAME         "{soc_name}"
{status_led}
            """
        )
        pins = board_dir / "pins.c"
        pins.write_text(
            f"""
        // This file is autogenerated by build_circuitpython.py

#include "shared-bindings/board/__init__.h"

#include "py/obj.h"
#include "py/mphal.h"

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
    kwargs = {}
    kwargs["busio"] = True
    kwargs["digitalio"] = True
    with open(board_dir / "mpconfigboard.toml", "rb") as f:
        mpconfigboard = tomllib.load(f)
    if usb_num_endpoint_pairs > 0:
        for macro in ("USB_PID", "USB_VID"):
            circuitpython_flags.append(f"-D{macro}=0x{mpconfigboard.get(macro):04x}")
        for macro, limit in (("USB_PRODUCT", 16), ("USB_MANUFACTURER", 8)):
            circuitpython_flags.append(f"-D{macro}='\"{mpconfigboard.get(macro)}\"'")
            circuitpython_flags.append(
                f"-D{macro}_{limit}='\"{mpconfigboard.get(macro[:limit])}\"'"
            )
        circuitpython_flags.append(f"-DUSB_NUM_ENDPOINT_PAIRS={usb_num_endpoint_pairs}")
        for direction in ("IN", "OUT"):
            circuitpython_flags.append(f"-DUSB_NUM_{direction}_ENDPOINTS={usb_num_endpoint_pairs}")
        # USB is special because it doesn't have a matching module.
        msc_enabled = False
        if msc_enabled:
            # force storage on
            kwargs["storage"] = True
        circuitpython_flags.append(f"-DCIRCUITPY_USB_MSC={1 if msc_enabled else 0}")
        # if "usb_cdc" not in kwargs:
        #   kwargs["usb_cdc"] = True
        # if "usb_hid_enabled_default" not in kwargs:
        #   kwargs["usb_hid_enabled_default"] = usb_num_endpoint_pairs >= 5
        # if "usb_midi_enabled_default" not in kwargs:
        #   kwargs["usb_midi_enabled_default"] = usb_num_endpoint_pairs >= 8
        # supervisor_source.extend(top.glob("supervisor/shared/usb/*.c"))
        # if not usb_host_keyboard:
        #     supervisor_source.remove(top / "supervisor/shared/usb/host_keyboard.c")
    # if flash_filesystem == "external":
    #     supervisor_source.extend(top.glob("supervisor/shared/usb/*.c"))
    # Make sure all modules have a setting by filling in defaults.
    hal_source = []
    for module in top.glob("shared-bindings/*"):
        if not module.is_dir():
            continue
        enabled = kwargs.get(module.name, module.name in DEFAULT_MODULES)
        kwargs[module.name] = enabled
        # print(f"Module {module.name} enabled: {enabled}")
        circuitpython_flags.append(
            f"-DCIRCUITPY_{module.name.upper()}={1 if kwargs[module.name] else 0}"
        )

        if enabled:
            hal_source.extend(top.glob(f"ports/zephyr-cp/common-hal/{module.name}/*.c"))
            hal_source.extend(top.glob(f"shared-bindings/{module.name}/*.c"))

    for mpflag in MPCONFIG_FLAGS:
        circuitpython_flags.append(f"-DCIRCUITPY_{mpflag.upper()}=0")

    source_files = supervisor_source + hal_source + ["extmod/vfs.c"]
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

    assembly_files = []
    assembly_files.append(srcdir / "ports/nordic/supervisor/cpu.s")

    source_files.extend(assembly_files)

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
