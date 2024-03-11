from hancho import *
import pathlib

top = pathlib.Path.cwd()

compile_circuitpython = Rule(
    desc="Compile {files_in} -> {files_out}",
    command="{compiler} -MMD -c {files_in} {circuitpython_flags} {port_flags} -o {files_out}",
    files_out="{swap_ext(files_in, '.o')}",
    depfile="{swap_ext(files_out, '.o.d')}",
)

link = Rule(
    desc="Link {files_in} -> {files_out}",
    command="{compiler} {files_in} -o {files_out}",
)

preprocess = Rule(
    desc="Preprocess {files_in} -> {files_out}",
    command="{compiler} -E -MMD -c {files_in} {qstr_flags} {circuitpython_flags} {port_flags} -o {files_out}",
    files_out="{swap_ext(files_in, '.pp')}",
    depfile="{swap_ext(files_out, '.pp.d')}",
)

split_defs = Rule(
  desc = "Splitting {mode} defs",
  command = "python py/makeqstrdefs.py split {mode} {files_in} {build_dir}/genhdr/{mode} {files_out}",
  deps="py/makeqstrdefs.py",
  files_out="genhdr/{mode}/{swap_ext(files_in, '.split')}",
)

collect_defs = Rule(
  desc = "Collecting {mode} defs",
  command = "python py/makeqstrdefs.py cat {mode} _ {build_dir}/genhdr/{mode} {files_out}",
  deps="py/makeqstrdefs.py",
)

qstr_preprocessed = Rule(
    desc="Preprocess QSTRs",
    command='cat {files_in} | sed \'s/^Q(.*)/"&"/\' | {compiler} -DNO_QSTR {cflags} -E - | sed \'s/^\"\\(Q(.*)\\)\"/\1/\' > {files_out}',
    deps=[""]
)

qstr_generated = Rule(
    desc="Generate QSTRs",
    command="python {top}/py/makeqstrdata.py {files_in} > {files_out}",
    deps="{top}/py/makeqstrdata.py",
    top=str(top)
)

version_generate = Rule(
    desc="Generate version info",
    command="python py/makeversionhdr.py {files_out}",
    deps="py/makeversionhdr.py"
  )

version_header = version_generate([], files_out="genhdr/mpversion.h")

generate_root_pointers = Rule(
    desc="Generate root pointers",
    command="python py/make_root_pointers.py {files_in} > {files_out}",
    deps="py/make_root_pointers.py"
    )

def board(
    board_name, compiler="clang", port_flags="", flash_filesystem="external", **kwargs
):
    print("top level", board_name)

    board_id = pathlib.Path.cwd().name
    top_build = top / "build" / board_id
    print("top, board_id", board_id)

    circuitpython_flags = []
    circuitpython_flags.append("-Wno-expansion-to-defined")
    circuitpython_flags.append("-DCIRCUITPY")
    circuitpython_flags.append("-DFFCONF_H=\\\"lib/oofatfs/ffconf.h\\\"")
    circuitpython_flags.append(f"-I {top}")
    circuitpython_flags.append(f"-I {config.build_dir}")

    for fs_type in ["internal", "external", "qspi"]:
      value = 1 if flash_filesystem == fs_type else 0
      circuitpython_flags.append(f"-D{fs_type.upper()}_FLASH_FILESYSTEM={value}")

    circuitpython_flags = " ".join(circuitpython_flags)

    qstr_files = ["main.c"]
    preprocessed = [preprocess(f,
            compiler=compiler,
            qstr_flags="-DNO_QSTR",
            deps=version_header,
            circuitpython_flags=circuitpython_flags,
            port_flags=port_flags) for f in qstr_files]
    qstr_split = [split_defs(f, mode="qstr") for f in preprocessed]
    modules_split = [split_defs(f, mode="module") for f in preprocessed]
    root_pointers_split = [split_defs(f, mode="root_pointer") for f in preprocessed]

    qstr_collected = collect_defs(files_in=qstr_split, files_out="genhdr/qstrdefs.collected", mode="qstr")
    modules_collected = collect_defs(files_in=modules_split, files_out="genhdr/moduledefs.collected", mode="module")
    root_pointers_collected = collect_defs(files_in=root_pointers_split, files_out="genhdr/root_pointers.collected", mode="root_pointer")

    qstrpp = qstr_preprocessed(files_in=[top / "py/qstrdefs.h", qstr_collected], files_out="genhdr/qstrdefs.preprocessed.h", compiler=compiler, cflags=[circuitpython_flags, port_flags])
    qstrdefs = qstr_generated(qstrpp, files_out="genhdr/qstrdefs.generated.h")

    root_pointers = generate_root_pointers(files_in=root_pointers_collected, files_out="genhdr/root_pointers.h")

    # partial_link()
    objects = []
    objects.append(
        compile_circuitpython(
            top / "main.c",
            top_build / "main.o",
            deps=[qstrdefs, version_header, root_pointers],
            compiler=compiler,
            circuitpython_flags=circuitpython_flags,
            port_flags=port_flags,
        )
    )

    link(objects, f"circuitpython-{board_id}.elf", compiler=compiler)
