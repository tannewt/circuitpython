from hancho import *

meson = Rule(
    command = "meson setup --reconfigure -Dtests=false -Dmultilib=false --cross-file {cross_file} picolibc {out_dir}",
    files_out = ["build.ninja"],
)

ninja = Rule(
    command = "ninja -j 1",
    out_dir = "{task_dir}"
)

def build(build_dir, cflags="", compiler="clang"):
    include_paths = [build_dir, "lib/picolibc/newlib/libc/stdio"]
    libraries = []


    ninja_file = meson("picolibc/meson.build", out_dir=build_dir, cross_file="picolibc/scripts/cross-clang-thumbv6m-none-eabi.txt")

    picolibc_a = ninja(ninja_file, files_out=["newlib/libm.a", "newlib/libc.a", "newlib/libg.a"], task_dir=build_dir)


    return include_paths, picolibc_a
