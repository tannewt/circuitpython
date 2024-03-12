import hancho
import pathlib

CPU_FLAGS = {
    "cortex-m0+": "--target=arm-none-eabi -march=armv6-m -mthumb -mabi=aapcs-linux -mcpu=cortex-m0plus -msoft-float -mfloat-abi=soft"

}

top = pathlib.Path.cwd()

picolibc = hancho.load("lib/picolibc.py")

compile_circuitpython = hancho.Rule(
    desc="Compile {files_in} -> {files_out}",
    command="{compiler} -MMD {cpu_flags} {circuitpython_flags} {port_flags} -c {files_in} -o {files_out}",
    files_out="{swap_ext(files_in, '.o')}",
    depfile="{swap_ext(files_out, '.o.d')}",
)

link = hancho.Rule(
    desc="Link {files_in} -> {files_out}",
    command="{compiler} {files_in} -o {files_out}",
)

preprocess = hancho.Rule(
    desc="Preprocess {files_in} -> {files_out}",
    command="{compiler} -E -MMD -c {files_in} {qstr_flags} {circuitpython_flags} {port_flags} -o {files_out}",
    files_out="{swap_ext(files_in, '.pp')}",
    depfile="{swap_ext(files_out, '.pp.d')}",
)

split_defs = hancho.Rule(
  desc = "Splitting {mode} defs",
  command = "python py/makeqstrdefs.py split {mode} {files_in} {build_dir}/genhdr/{mode} {files_out}",
  deps="py/makeqstrdefs.py",
  files_out="genhdr/{mode}/{swap_ext(files_in, '.split')}.{mode}",
)

collect_defs = hancho.Rule(
  desc = "Collecting {mode} defs",
  command = ["rm -f {files_out}.hash", "python py/makeqstrdefs.py cat {mode} _ {build_dir}/genhdr/{mode} {files_out}"],
  deps="py/makeqstrdefs.py",
)

python_script = hancho.Rule(
    command = "python {deps} {files_in} > {files_out}",
)

qstr_preprocessed = hancho.Rule(
    desc="Preprocess QSTRs",
    command='cat {files_in} | sed \'s/^Q(.*)/"&"/\' | {compiler} -DNO_QSTR {cflags} -E - | sed \'s/^\"\\(Q(.*)\\)\"/\1/\' > {files_out}',
    deps=[""]
)

qstr_generated = python_script.extend(
    deps="py/makeqstrdata.py"
)

version_generate = hancho.Rule(
    desc="Generate version info",
    command="python py/makeversionhdr.py {files_out}",
    deps="py/makeversionhdr.py"
  )

version_header = version_generate([], files_out="genhdr/mpversion.h")

def board(
    board_name, cpu, compiler="clang", port_flags="", flash_filesystem="external", tusb_mem_align=4, usb=False, **kwargs
):
    print("top level", board_name)

    board_id = pathlib.Path.cwd().name
    port_id = pathlib.Path.cwd().parent.parent.name
    top_build = top / "build" / board_id
    print("top, board_id", board_id)

    always_on_modules = ["sys"]

    cpu_flags = CPU_FLAGS[cpu]

    picolibc_includes, picolibc_a = picolibc.build(top / hancho.config.build_dir / "picolibc" / cpu, cpu_flags)

    circuitpython_flags = []
    circuitpython_flags.append("-Wno-expansion-to-defined")
    circuitpython_flags.append("-DCIRCUITPY")
    for module in always_on_modules:
        circuitpython_flags.append(f"-DCIRCUITPY_{module.upper()}=1")
    circuitpython_flags.append(f"-DCIRCUITPY_USB={1 if usb else 0}")
    circuitpython_flags.append(f"-DCIRCUITPY_BOARD_ID=\\\"{board_id}\\\"")
    circuitpython_flags.append(f"-DCIRCUITPY_TUSB_MEM_ALIGN={tusb_mem_align}")
    circuitpython_flags.append("-DFFCONF_H=\\\"lib/oofatfs/ffconf.h\\\"")
    circuitpython_flags.append(f"-I {top}")
    circuitpython_flags.append(f"-I lib/tinyusb/src")
    circuitpython_flags.append(f"-isystem lib/cmsis/inc")
    circuitpython_flags.append("-isystem lib/picolibc/newlib/libc/include/")
    circuitpython_flags.append(f"-I {hancho.config.build_dir}")
    for picolibc_include_path in picolibc_includes:
        circuitpython_flags.extend(("-isystem ", picolibc_include_path))

    for fs_type in ["internal", "external", "qspi"]:
      value = 1 if flash_filesystem == fs_type else 0
      circuitpython_flags.append(f"-D{fs_type.upper()}_FLASH_FILESYSTEM={value}")

    supervisor_source = ["main.c", "lib/tlsf/tlsf.c", f"ports/{port_id}/supervisor/port.c", "supervisor/stub/misc.c"] + hancho.glob("supervisor/shared/**/*.c")

    source_files = supervisor_source + ["py/modsys.c", "extmod/vfs.c"]
    preprocessed = [preprocess(f,
            compiler=compiler,
            qstr_flags="-DNO_QSTR",
            deps=[version_header, picolibc_a],
            circuitpython_flags=circuitpython_flags,
            port_flags=port_flags) for f in source_files]
    qstr_split = [split_defs(f, mode="qstr") for f in preprocessed]
    modules_split = [split_defs(f, mode="module") for f in preprocessed]
    root_pointers_split = [split_defs(f, mode="root_pointer") for f in preprocessed]

    qstr_collected = collect_defs(files_in=qstr_split, files_out="genhdr/qstrdefs.collected", mode="qstr")
    modules_collected = collect_defs(files_in=modules_split, files_out="genhdr/moduledefs.collected", mode="module")
    root_pointers_collected = collect_defs(files_in=root_pointers_split, files_out="genhdr/root_pointers.collected", mode="root_pointer")

    module_header = python_script(modules_collected, "genhdr/moduledefs.h", deps="py/makemoduledefs.py")

    qstrpp = qstr_preprocessed(files_in=[top / "py/qstrdefs.h"], files_out="genhdr/qstrdefs.preprocessed.h", compiler=compiler, cflags=[circuitpython_flags, port_flags])
    qstrdefs = qstr_generated(files_in=[qstrpp, qstr_collected], files_out="genhdr/qstrdefs.generated.h")

    root_pointers = python_script(files_in=root_pointers_collected, files_out="genhdr/root_pointers.h", deps="py/make_root_pointers.py")

    objects = [picolibc_a]
    for source_file in source_files:
        objects.append(
            compile_circuitpython(
                source_file,
                deps=[qstrdefs, version_header, root_pointers, module_header, picolibc_a],
                compiler=compiler,
                cpu_flags=cpu_flags,
                circuitpython_flags=circuitpython_flags,
                port_flags=port_flags,
            )
        )

    link(objects, f"circuitpython-{board_id}.elf", compiler=compiler)
