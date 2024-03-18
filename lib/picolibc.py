from hancho import *

meson = Rule(
    command="meson setup --reconfigure -Dtests=false -Dmultilib=false --cross-file {cross_file} picolibc {out_dir}",
    files_out=["build.ninja"],
)

ninja = Rule(
    command=["ninja -j {job_count}", "touch {files_out}"],
    out_dir="{work_dir}",
    job_count=1 if config.jobs == 1 else (config.jobs // 2),
)


def build(build_dir, cflags="", compiler="clang"):
    include_paths = [build_dir, "lib/picolibc/newlib/libc/stdio"]
    libraries = []

    ninja_file = meson(
        "picolibc/meson.build",
        out_dir=build_dir,
        cross_file="picolibc/scripts/cross-clang-thumbv6m-none-eabi.txt",
    )

    picolibc_a = ninja(
        ninja_file,
        files_out=["newlib/libm.a", "newlib/libc.a", "newlib/libg.a"],
        work_dir=build_dir,
    )

    return include_paths, picolibc_a
