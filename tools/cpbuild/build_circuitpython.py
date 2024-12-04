import asyncio
import colorlog
import sys
import logging
import os
import pathlib
import tomllib

import cpbuild

print("hello zephyr", sys.argv)

print(os.environ)
srcdir = pathlib.Path("/home/tannewt/repos/circuitpython")
builddir = pathlib.Path.cwd()
cmake_args = {}
for var in sys.argv[1:]:
    key, value = var.split("=", 1)
    cmake_args[key] = value

compiler = cpbuild.Compiler(srcdir, builddir, cmake_args)

ALWAYS_ON_MODULES = ["sys"]
VENDOR_TO_PORT = {"nordic": "nordic"}
DEFAULT_MODULES = []
MPCONFIG_FLAGS = ["ulab", "nvm", "displayio", "warnings", "alarm", "array", "json"]


async def preprocess_and_split_defs(compiler, source_file, build_path, flags):
    build_file = source_file.with_suffix(".pp")
    build_file = build_path / (build_file.relative_to(srcdir))
    await compiler.preprocess(source_file, build_file, flags=flags)
    async with asyncio.TaskGroup() as tg:
        for mode in ("qstr", "module", "root_pointer"):
            split_file = build_file.relative_to(build_path).with_suffix(f".split.{mode}")
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
    await cpbuild.run_command(
        [
            "python",
            srcdir / "py" / "makeqstrdefs.py",
            "cat",
            mode,
            "_",
            build_path / "genhdr" / mode,
            output_file,
        ],
        srcdir,
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


async def build_circuitpython():
    circuitpython_flags = ["-DCIRCUITPY"]
    soc_directory = pathlib.Path(cmake_args["SOC_DIRECTORIES"])
    port = VENDOR_TO_PORT[soc_directory.name]
    port_flags = []
    # usb_num_endpoint_pairs = 8
    usb_num_endpoint_pairs = 0
    enable_mpy_native = False
    full_build = False
    usb_host = False
    tusb_mem_align = 4
    board = cmake_args.get("BOARD_ALIAS", cmake_args["BOARD"])
    for module in ALWAYS_ON_MODULES:
        circuitpython_flags.append(f"-DCIRCUITPY_{module.upper()}=1")
    circuitpython_flags.append(f"-DCIRCUITPY_TINYUSB={1 if usb_num_endpoint_pairs > 0 else 0}")
    circuitpython_flags.append(f"-DCIRCUITPY_USB_DEVICE={1 if usb_num_endpoint_pairs > 0 else 0}")
    circuitpython_flags.append(f"-DCIRCUITPY_ENABLE_MPY_NATIVE={1 if enable_mpy_native else 0}")
    circuitpython_flags.append(f"-DCIRCUITPY_FULL_BUILD={1 if full_build else 0}")
    circuitpython_flags.append(f"-DCIRCUITPY_USB_HOST={1 if usb_host else 0}")
    circuitpython_flags.append(f'-DCIRCUITPY_BOARD_ID=\\"{board}\\"')
    circuitpython_flags.append(f"-DCIRCUITPY_TUSB_MEM_ALIGN={tusb_mem_align}")
    circuitpython_flags.append('-DFFCONF_H=\\"lib/oofatfs/ffconf.h\\"')
    circuitpython_flags.extend(("-I", srcdir))
    circuitpython_flags.extend(("-I", srcdir / "lib/tinyusb/src"))
    circuitpython_flags.extend(("-I", srcdir / "supervisor/shared/usb"))
    circuitpython_flags.extend(("-I", builddir))
    circuitpython_flags.extend(("-I", srcdir / "ports" / port))
    circuitpython_flags.extend(("-I", srcdir / "boards" / board))
    # circuitpython_flags.extend(("-I", build_path / board_id))

    genhdr = builddir / "genhdr"
    genhdr.mkdir(exist_ok=True, parents=True)
    await cpbuild.run_command(
        ["python", srcdir / "py" / "makeversionhdr.py", genhdr / "mpversion.h"], srcdir
    )

    supervisor_source = [
        "main.c",
        "lib/tlsf/tlsf.c",
        f"ports/{port}/supervisor/port.c",
        f"ports/{port}/common-hal/microcontroller/__init__.c",
        f"ports/{port}/common-hal/microcontroller/Processor.c",
        "supervisor/stub/misc.c",
        "shared/readline/readline.c",
        "shared/runtime/pyexec.c",
        "shared/runtime/interrupt_char.c",
        "shared/runtime/stdout_helpers.c",
        "shared/runtime/sys_stdio_mphal.c",
        "extmod/vfs_reader.c",
    ]
    top = srcdir
    supervisor_source = [pathlib.Path(p) for p in supervisor_source]
    supervisor_source.extend(top.glob("supervisor/shared/*.c"))
    supervisor_source.append(top / "supervisor/shared/translate/translate.c")
    # if web_workflow:
    #     supervisor_source.extend(top.glob("supervisor/shared/web_workflow/*.c"))

    # Load the toml settings
    kwargs = {}
    with open(srcdir / "boards" / board / "mpconfigboard.toml", "rb") as f:
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
    for module in top.glob("shared-bindings/*"):
        if not module.is_dir():
            continue
        enabled = kwargs.get(module.name, module.name in DEFAULT_MODULES)
        kwargs[module.name] = enabled
        circuitpython_flags.append(
            f"-DCIRCUITPY_{module.name.upper()}={1 if kwargs[module.name] else 0}"
        )

    for mpflag in MPCONFIG_FLAGS:
        circuitpython_flags.append(f"-DCIRCUITPY_{mpflag.upper()}=0")

    source_files = supervisor_source + ["py/modsys.c", "extmod/vfs.c"]
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
    source_files.append(srcdir / "supervisor" / "zephyr" / "flash.c")

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
