import hancho
import pathlib

CPU_FLAGS = {
    "cortex-m0+": "--target=arm-none-eabi -march=armv6-m -mthumb -mabi=aapcs-linux -mcpu=cortex-m0plus -msoft-float -mfloat-abi=soft"
}

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
    "supervisor/shared/usb/usb_desc.c": ["usb_midi", "usb_hid"]
}

config.work_dir = "{ root_dir / rule_dir }"
config.deps_dir = "{ root_dir / rule_dir }"
config.in_dir = "{ root_dir / call_dir }"
config.out_dir = "{ root_dir / build_dir / call_dir }"

picolibc = hancho.load("lib/picolibc.py")

top = pathlib.Path.cwd()

compile_circuitpython = hancho.Rule(
    desc="Compile {files_in} -> {files_out}",
    command="{compiler} -MMD -Wall -Werror -Wundef {cpu_flags} {module_flags} {circuitpython_flags} {port_flags} -c {files_in} -o {files_out}",
    files_out="{swap_ext(files_in, '.o')}",
    depfile="{swap_ext(files_out, '.o.d')}",
)

link = hancho.Rule(
    desc="Link {files_in} -> {files_out}",
    command="{compiler} {flags} -Wl,-T,{linker_script} {files_in} -o {files_out}",
)

preprocess = hancho.Rule(
    desc="Preprocess {files_in} -> {files_out}",
    command="{compiler} -E -MMD -c {files_in} {module_flags} {qstr_flags} {circuitpython_flags} {port_flags} -o {files_out}",
    files_out="{swap_ext(files_in, '.pp')}",
    depfile="{swap_ext(files_out, '.pp.d')}",
)

split_defs = hancho.Rule(
    desc="Splitting {mode} defs",
    command="python py/makeqstrdefs.py split {mode} {files_in} {build_dir}/genhdr/{mode} {files_out}",
    deps="py/makeqstrdefs.py",
    files_out="genhdr/{mode}/{swap_ext(files_in, '.split')}.{mode}",
)

collect_defs = hancho.Rule(
    desc="Collecting {mode} defs",
    command=[
        "rm -f {files_out}.hash",
        "python py/makeqstrdefs.py cat {mode} _ {out_dir}/genhdr/{mode} {files_out}",
    ],
    deps="py/makeqstrdefs.py",
)

python_script = hancho.Rule(
    command="python {deps} {files_in} > {files_out}",
)

qstr_preprocessed = hancho.Rule(
    desc="Preprocess QSTRs",
    command="cat {files_in} | sed 's/^Q(.*)/\"&\"/' | {compiler} -DNO_QSTR {cflags} -E - | sed 's/^\"\\(Q(.*)\\)\"/\1/' > {files_out}",
    deps=[""],
)

qstr_generated = python_script.extend(deps="py/makeqstrdata.py")

version_generate = hancho.Rule(
    desc="Generate version info",
    command="python py/makeversionhdr.py {files_out}",
    deps="py/makeversionhdr.py",
)


def board(
    board_name,
    cpu,
    port_source_files,
    port_objects,
    *,
    linker_script,
    compiler="clang",
    port_flags="",
    flash_filesystem="external",
    tusb_mem_align=4,
    usb_num_endpoint_pairs=0,
    usb_host=False,
    web_workflow=False,
    enable_mpy_native=False,
    full_build=True,
    **kwargs,
):
    board_id = pathlib.Path.cwd().name
    port_id = pathlib.Path.cwd().parent.parent.name

    always_on_modules = ["sys"]

    cpu_flags = CPU_FLAGS[cpu]

    version_header = version_generate([], files_out="genhdr/mpversion.h")

    picolibc_includes, picolibc_a = picolibc.build(
        config.root_dir / config.build_dir / "lib" / "picolibc" / cpu,
        cpu_flags,
    )

    circuitpython_flags = []
    circuitpython_flags.append("-Wno-expansion-to-defined")
    circuitpython_flags.append("-DCIRCUITPY")
    for module in always_on_modules:
        circuitpython_flags.append(f"-DCIRCUITPY_{module.upper()}=1")
    circuitpython_flags.append(f"-DCIRCUITPY_USB={1 if usb_num_endpoint_pairs > 0 else 0}")
    circuitpython_flags.append(f"-DCIRCUITPY_ENABLE_MPY_NATIVE={1 if enable_mpy_native else 0}")
    circuitpython_flags.append(f"-DCIRCUITPY_FULL_BUILD={1 if full_build else 0}")
    circuitpython_flags.append(f"-DCIRCUITPY_USB_HOST={1 if usb_host else 0}")
    circuitpython_flags.append(f'-DCIRCUITPY_BOARD_ID=\\"{board_id}\\"')
    circuitpython_flags.append(f"-DCIRCUITPY_TUSB_MEM_ALIGN={tusb_mem_align}")
    circuitpython_flags.append('-DFFCONF_H=\\"lib/oofatfs/ffconf.h\\"')
    circuitpython_flags.append("-I {root_dir}")
    circuitpython_flags.append("-I lib/tinyusb/src")
    circuitpython_flags.append("-I supervisor/shared/usb")
    circuitpython_flags.append(f"-isystem lib/cmsis/inc")
    circuitpython_flags.append("-isystem lib/picolibc/newlib/libc/include/")
    circuitpython_flags.append(f"-I {config.build_dir}")
    circuitpython_flags.append(f"-I {config.build_dir}/{board_id}")
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
    supervisor_source.extend(top.glob("supervisor/shared/*.c"))
    if web_workflow:
        supervisor_source.extend(top.glob("supervisor/shared/web_workflow/*.c"))
    if usb_num_endpoint_pairs > 0:
        for macro in ("USB_PID", "USB_VID"):
            circuitpython_flags.append(f"-D{macro}=0x{kwargs.get(macro.lower()):04x}")
        for macro in ("USB_PRODUCT", "USB_MANUFACTURER"):
            circuitpython_flags.append(f"-D{macro}='\"{kwargs.get(macro.lower())}\"'")
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
    if flash_filesystem == "external":
        supervisor_source.extend(top.glob("supervisor/shared/usb/*.c"))

    # Make sure all modules have a setting by filling in defaults.
    for module in top.glob("shared-bindings/*"):
      enabled = kwargs.get(module.name, module.name in DEFAULT_MODULES)
      kwargs[module.name] = enabled

    # Add global flags for defines used in mpconfigs.
    for module_name in MPCONFIG_FLAGS:
      circuitpython_flags.append(f"-DCIRCUITPY_{module_name.upper()}={1 if kwargs[module_name] else 0}")

    print(supervisor_source)

    source_files = supervisor_source + ["py/modsys.c", "extmod/vfs.c"]
    board_output = f'{{ root_dir / build_dir / "{board_id}" }}'
    preprocess_qstr = preprocess.extend(
            compiler=compiler,
            out_dir=board_output,
            qstr_flags="-DNO_QSTR",
            deps=[version_header, picolibc_a],
            circuitpython_flags=circuitpython_flags,
            port_flags=port_flags,
    )
    preprocessed = [
        preprocess_qstr(f)
        for f in source_files
    ]

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
      for c_source in module.glob("*.c"):
        preprocessed.append(preprocess_qstr(c_source, module_flags=flags))
      # To gather error messages we need to preprocess the implementation code.
      for source_file in top.glob(f"ports/{port_id}/common-hal/{module.name}/*.c"):
        objects.append(compile_for_board(source_file, module_flags=flags))
      module_cflags[module.name] = flags


    board_split_defs = split_defs.extend(out_dir=board_output)
    qstr_split = [board_split_defs(f, mode="qstr") for f in preprocessed]
    modules_split = [board_split_defs(f, mode="module") for f in preprocessed]
    root_pointers_split = [
        board_split_defs(f, mode="root_pointer") for f in preprocessed
    ]

    board_collect_defs = collect_defs.extend(out_dir=board_output)
    qstr_collected = board_collect_defs(
        files_in=qstr_split, files_out="genhdr/qstrdefs.collected", mode="qstr"
    )
    modules_collected = board_collect_defs(
        files_in=modules_split, files_out="genhdr/moduledefs.collected", mode="module"
    )
    root_pointers_collected = board_collect_defs(
        files_in=root_pointers_split,
        files_out="genhdr/root_pointers.collected",
        mode="root_pointer",
    )

    board_python_script = python_script.extend(out_dir=board_output)
    module_header = board_python_script(
        modules_collected, "genhdr/moduledefs.h", deps="py/makemoduledefs.py"
    )

    qstrpp = qstr_preprocessed(
        files_in=["py/qstrdefs.h"],
        files_out="genhdr/qstrdefs.preprocessed.h",
        compiler=compiler,
        cflags=[circuitpython_flags, port_flags],
        out_dir=board_output,
    )
    qstrdefs = qstr_generated(
        files_in=[qstrpp, qstr_collected],
        files_out="genhdr/qstrdefs.generated.h",
        out_dir=board_output,
    )

    root_pointers = board_python_script(
        files_in=root_pointers_collected,
        files_out="genhdr/root_pointers.h",
        deps="py/make_root_pointers.py",
    )

    objects = [picolibc_a]

    compile_for_board = compile_circuitpython.extend(
                deps=[
                    qstrdefs,
                    version_header,
                    root_pointers,
                    module_header,
                    picolibc_a,
                ],
                compiler=compiler,
                cpu_flags=cpu_flags,
                circuitpython_flags=circuitpython_flags,
                port_flags=port_flags,
                out_dir=board_output,
      )
    for source_file in source_files:
        objects.append(
            compile_for_board(
                source_file,
            )
        )

    # Pass in our enabling flags to our source
    for module in top.glob("shared-bindings/*"):
      enabled = kwargs[module.name]
      if not enabled:
        continue
      name_upper = module.name.upper()
      for source_file in module.glob("*.c"):
        objects.append(compile_for_board(source_file, module_flags=module_cflags[module.name]))
      for source_file in top.glob(f"ports/{port_id}/common-hal/{module.name}/*.c"):
        objects.append(compile_for_board(source_file, module_flags=module_cflags[module.name]))

    link(
        objects,
        f"circuitpython-{board_id}.elf",
        compiler=compiler,
        linker_script=linker_script,
        flags=["-nostdlib", cpu_flags],
    )
