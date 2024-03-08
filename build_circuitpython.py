import asyncio
import itertools
import importlib
import pathlib
from embedded import build
import embedded.cli

from lib import picolibc

# CPU_FLAGS = {
#     "cortex-m0+": "--target=arm-none-eabi -march=armv6-m -mthumb -mabi=aapcs-linux -mcpu=cortex-m0plus -msoft-float -mfloat-abi=soft"
# }

MODULE_CONFIGS = {
  "usb_cdc": {
    "console_enabled_default": True,
    "data_enabled_default": False,
  },
  "usb_hid": {
    "enabled_default": None,
  },
  "usb_midi": {
    "enabled_default": None,
  }
}

DEFAULT_MODULES = []

# io and collections are MP modules that fit this bill
MPCONFIG_FLAGS = ["ulab", "nvm", "displayio", "warnings", "alarm"]

ALLOW_FLAGS = {
    pathlib.Path("main.c"): ["atexit", "board", "bleio", "canio", "collections", "cyw43", "digitalio", "epaperdisplay", "io", "keypad", "memorymonitor", "microcontroller", "socketpool", "usb_hid", "watchdog", "wifi"],
    "supervisor/shared/usb/usb_desc.c": ["usb_midi", "usb_hid"],
    "supervisor/shared/micropython.c": ["atexit", "watchdog"]
}

# compile_circuitpython = hancho.Rule(
#     desc="Compile {source_files} -> {build_files}",
#     command="{compiler} -MMD -Wall -Werror -Wundef {cpu_flags} {module_flags} {circuitpython_flags} {port_flags} -c {source_files} -o {build_files}",
#     build_files="{swap_ext(source_files, '.o')}",
#     depfile="{swap_ext(build_files, '.o.d')}",
# )

# link = hancho.Rule(
#     desc="Link {source_files} -> {build_files}",
#     command="{compiler} {flags} -Wl,-T,{linker_script} {source_files} -o {build_files}",
# )


# collect_defs = hancho.Rule(
#     desc="Collecting {mode} defs",
#     command=[
#         "rm -f {build_files}.hash",
#         "python py/makeqstrdefs.py cat {mode} _ {out_dir}/genhdr/{mode} {build_files}",
#     ],
#     deps="py/makeqstrdefs.py",
# )

# python_script = hancho.Rule(
#     command="python {deps} {source_files} > {build_files}",
# )

top = pathlib.Path(__file__).parent

async def preprocess_and_split_defs(compiler, source_file, build_path, flags):
    build_file = source_file.with_suffix(".pp")
    build_file = build_path / (build_file.relative_to(top))
    await compiler.preprocess(source_file, build_file, flags=flags)
    async with asyncio.TaskGroup() as tg:
        for mode in ("qstr", "module", "root_pointer"):
            split_file = build_file.relative_to(build_path).with_suffix(f".split.{mode}")
            split_file = build_path / "genhdr" / mode / split_file
            split_file.parent.mkdir(exist_ok=True, parents=True)
            tg.create_task(build.run_command(["python", top / "py/makeqstrdefs.py", "split", mode, build_file, build_path / "genhdr" / mode, split_file]))

async def collect_defs(mode, build_path):
    output_file = build_path / f"{mode}defs.collected"
    await build.run_command(["python", top / "py" / "makeqstrdefs.py", "cat", mode, "_", build_path / "genhdr" / mode, output_file])
    return output_file

async def generate_qstr_headers(build_path, compiler, flags):
    collected = await collect_defs("qstr", build_path)

    strings = build_path / "genhdr" / "qstrdefs.str.h"
    preprocessed_strings = build_path / "genhdr" / "qstrdefs.str.preprocessed.h"
    preprocessed = build_path / "genhdr" / "qstrdefs.preprocessed.h"
    await build.run_command(["cat", top / "py" / "qstrdefs.h", "|", "sed", "'s/^Q(.*)/\"&\"/'", ">", strings])
    await compiler.preprocess(strings, preprocessed_strings, flags)
    await build.run_command(["cat", preprocessed_strings, "|", "sed", "'s/^\"\\(Q(.*)\\)\"/\1/'", ">", preprocessed])

    await build.run_command(["python", top / "py" / "makeqstrdata.py", collected, preprocessed, ">", build_path / "genhdr" / "qstrdefs.generated.h"])


async def generate_module_header(build_path):
    collected = await collect_defs("module", build_path)
    await build.run_command(["python", top / "py" / "makemoduledefs.py", collected, ">", build_path / "genhdr" / "moduledefs.h"])

async def generate_root_pointer_header(build_path):
    collected = await collect_defs("root_pointer", build_path)
    await build.run_command(["python", top / "py" / "make_root_pointers.py", collected, ">", build_path / "genhdr" / "root_pointers.h"])

async def board(
    port_id,
    board_id,
    board_name,
    cpu,
    port_source_files,
    port_objects,
    *,
    linker_script,
    compiler: embedded.Compiler,
    port_flags=[],
    flash_filesystem="external",
    tusb_mem_align=4,
    usb_num_endpoint_pairs=0,
    usb_host=False,
    usb_host_keyboard=False,
    external_flash_devices=[],
    web_workflow=False,
    enable_mpy_native=False,
    full_build=True,
    **kwargs,
):
    build_path = pathlib.Path("build").resolve()

    top = pathlib.Path(__file__).parent

    always_on_modules = ["sys"]
    genhdr = build_path / "genhdr"
    genhdr.mkdir(exist_ok=True, parents=True)
    async with asyncio.TaskGroup() as tg:
        tg.create_task(build.run_command(["python", pathlib.Path("py").resolve() / "makeversionhdr.py", genhdr / "mpversion.h"]))
        picolibc_task = tg.create_task(picolibc.build(
            cpu,
            compiler,
            build_path / "lib" / "picolibc",
        ))
    picolibc_includes, picolibc_a = picolibc_task.result()

    circuitpython_flags = ["-fdata-sections", "-ffunction-sections", "-Wall", "-Werror", "-Wundef"]
    circuitpython_flags.append("-Wno-expansion-to-defined")
    circuitpython_flags.append("-DCIRCUITPY")
    for module in always_on_modules:
        circuitpython_flags.append(f"-DCIRCUITPY_{module.upper()}=1")
    circuitpython_flags.append(f"-DCIRCUITPY_TINYUSB={1 if usb_num_endpoint_pairs > 0 else 0}")
    circuitpython_flags.append(f"-DCIRCUITPY_USB_DEVICE={1 if usb_num_endpoint_pairs > 0 else 0}")
    circuitpython_flags.append(f"-DCIRCUITPY_ENABLE_MPY_NATIVE={1 if enable_mpy_native else 0}")
    circuitpython_flags.append(f"-DCIRCUITPY_FULL_BUILD={1 if full_build else 0}")
    circuitpython_flags.append(f"-DCIRCUITPY_USB_HOST={1 if usb_host else 0}")
    circuitpython_flags.append(f'-DCIRCUITPY_BOARD_ID=\\"{board_id}\\"')
    circuitpython_flags.append(f"-DCIRCUITPY_TUSB_MEM_ALIGN={tusb_mem_align}")
    circuitpython_flags.append('-DFFCONF_H=\\"lib/oofatfs/ffconf.h\\"')
    circuitpython_flags.extend(("-I", top))
    circuitpython_flags.extend(("-I", "lib/tinyusb/src"))
    circuitpython_flags.extend(("-I", "supervisor/shared/usb"))
    circuitpython_flags.extend(("-isystem", "lib/cmsis/inc"))
    circuitpython_flags.extend(("-isystem", "lib/picolibc/newlib/libc/include/"))
    circuitpython_flags.extend(("-I", build_path))
    circuitpython_flags.extend(("-I", build_path / board_id))
    for picolibc_include_path in picolibc_includes:
        circuitpython_flags.extend(("-isystem ", picolibc_include_path))

    for fs_type in ["internal", "external", "qspi"]:
        value = 1 if flash_filesystem == fs_type else 0
        circuitpython_flags.append(f"-D{fs_type.upper()}_FLASH_FILESYSTEM={value}")

    supervisor_source = [
        "main.c",
        "lib/tlsf/tlsf.c",
        f"ports/{port_id}/supervisor/port.c",
        "supervisor/stub/misc.c",
    ]
    supervisor_source = [pathlib.Path(p) for p in supervisor_source]
    supervisor_source.extend(top.glob("supervisor/shared/*.c"))
    if web_workflow:
        supervisor_source.extend(top.glob("supervisor/shared/web_workflow/*.c"))
    if usb_num_endpoint_pairs > 0:
        for macro in ("USB_PID", "USB_VID"):
            circuitpython_flags.append(f"-D{macro}=0x{kwargs.get(macro.lower()):04x}")
        for macro, limit in (("USB_PRODUCT", 16), ("USB_MANUFACTURER", 8)):
            circuitpython_flags.append(f"-D{macro}='\"{kwargs.get(macro.lower())}\"'")
            circuitpython_flags.append(f"-D{macro}_{limit}='\"{kwargs.get(macro.lower())[:limit]}\"'")
        circuitpython_flags.append(f"-DUSB_NUM_ENDPOINT_PAIRS={usb_num_endpoint_pairs}")
        for direction in ("IN", "OUT"):
            circuitpython_flags.append(f"-DUSB_NUM_{direction}_ENDPOINTS={usb_num_endpoint_pairs}")

        # USB is special because it doesn't have a matching module.
        msc_enabled = kwargs.get("usb_msc", True)
        if msc_enabled:
          # force storage on
          kwargs["storage"] = True
        circuitpython_flags.append(f"-DCIRCUITPY_USB_MSC={1 if msc_enabled else 0}")

        if "usb_cdc" not in kwargs:
          kwargs["usb_cdc"] = True

        if "usb_hid_enabled_default" not in kwargs:
          kwargs["usb_hid_enabled_default"] = usb_num_endpoint_pairs >= 5

        if "usb_midi_enabled_default" not in kwargs:
          kwargs["usb_midi_enabled_default"] = usb_num_endpoint_pairs >= 8

        supervisor_source.extend(top.glob("supervisor/shared/usb/*.c"))
        if not usb_host_keyboard:
            supervisor_source.remove(top / "supervisor/shared/usb/host_keyboard.c")
    if flash_filesystem == "external":
        supervisor_source.extend(top.glob("supervisor/shared/usb/*.c"))

    # Make sure all modules have a setting by filling in defaults.
    for module in top.glob("shared-bindings/*"):
      enabled = kwargs.get(module.name, module.name in DEFAULT_MODULES)
      kwargs[module.name] = enabled

    kwargs["ulab"] = False

    # Add global flags for defines used in mpconfigs.
    for module_name in MPCONFIG_FLAGS:
      circuitpython_flags.append(f"-DCIRCUITPY_{module_name.upper()}={1 if kwargs[module_name] else 0}")

    source_files = supervisor_source + ["py/modsys.c", "extmod/vfs.c"]
    board_output = f'{{ root_dir / build_dir / "{board_id}" }}'
    qstr_flags="-DNO_QSTR"
    # preprocess_qstr = preprocess.extend(
    #         compiler=compiler,
    #         out_dir=board_output,
    #         ,
    #         deps=[version_header, picolibc_a],
    #         circuitpython_flags=circuitpython_flags,
    #         port_flags=port_flags,
    # )
    async with asyncio.TaskGroup() as tg:
        extra_source_flags = {}
        for source_file in source_files:
            if source_file in ALLOW_FLAGS:
                extra_flags = []
                for module_name in ALLOW_FLAGS[source_file]:
                    name_upper = module_name.upper()
                    extra_flags.append(f"-DCIRCUITPY_{name_upper}={1 if kwargs.get(module_name, False) else 0}")

                extra_source_flags[source_file] = extra_flags
                print(extra_flags)
            tg.create_task(
                preprocess_and_split_defs(compiler, top / source_file, build_path / board_id, [qstr_flags, *circuitpython_flags, *port_flags, *extra_source_flags.get(source_file, [])]),
                name=f"Split defs {source_file}",
            )
        return

        # Pass in our enabling flags to our source
        module_cflags = {}
        for module in top.glob("shared-bindings/*"):
          if not kwargs[module.name]:
            continue
          name_upper = module.name.upper()
          flags = [f"-DCIRCUITPY_{name_upper}={1 if enabled else 0}"]
          if module.name in MODULE_CONFIGS:
            for flag, default in MODULE_CONFIGS[module.name].items():
              flag_enabled = kwargs.get(module.name + "_" + flag, default)
              if flag_enabled is None:
                raise RuntimeError(f"{module_name}_{flag} must be set")
              flags.append(f"-DCIRCUITPY_{name_upper}_{flag.upper()}={1 if flag_enabled else 0}")

          all_flags = [qstr_flags, *circuitpython_flags, *port_flags, *flags]
          for source_file in itertools.chain(module.glob("*.c"), top.glob(f"ports/{port_id}/common-hal/{module.name}/*.c")):
            tg.create_task(
                preprocess_and_split_defs(compiler, source_file, build_path / board_id, all_flags),
                name=f"Split defs {source_file}",
            )
          module_cflags[module.name] = flags


    async with asyncio.TaskGroup() as tg:
        board_build = build_path / board_id
        tg.create_task(generate_qstr_headers(board_build, compiler, [qstr_flags, *circuitpython_flags, *port_flags]))
        tg.create_task(generate_module_header(board_build))
        tg.create_task(generate_root_pointer_header(board_build))

    objects = []
    objects.extend(picolibc_a)

    async with asyncio.TaskGroup() as tg:
        board_build = build_path / board_id
        for source_file in source_files:
            source_file = top / source_file
            build_file = source_file.with_suffix(".o")
            object_file = board_build / (build_file.relative_to(top))
            tg.create_task(compiler.compile(cpu, source_file, object_file, [*circuitpython_flags, *port_flags]))

        # Pass in our enabling flags to our source
        for module in top.glob("shared-bindings/*"):
          enabled = kwargs[module.name]
          if not enabled:
            continue
          name_upper = module.name.upper()
          for source_file in itertools.chain(module.glob("*.c"), top.glob(f"ports/{port_id}/common-hal/{module.name}/*.c")):
            build_file = source_file.with_suffix(".o")
            object_file = board_build / (build_file.relative_to(top))
            objects.append(object_file)
            tg.create_task(compiler.compile(cpu, source_file, object_file, [*circuitpython_flags, *port_flags, *module_cflags[module.name]]))

    await compiler.link(
        cpu,
        objects,
        build_path / f"circuitpython-{board_id}.elf",
        flags=["-nostdlib"],
        linker_script=linker_script
    )

async def build_circuitpython(board: list[str]):
    async with asyncio.TaskGroup() as tg:
        for board_name in board:
            # find out what port this board is from
            path = tuple(pathlib.Path(".").glob(f"ports/*/boards/{board_name}"))[0]
            port = path.parent.parent.name
            board = importlib.import_module(f"ports.{port}.boards.{board_name}")
            tg.create_task(board.build())

if __name__ == "__main__":
    embedded.cli.run(build_circuitpython)
