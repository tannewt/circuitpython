from hancho import *

meson = Rule(
    command = "meson setup -Dtests=false -Dmultilib=false --cross-file {cross_file} picolibc {build_dir}"
)

ninja = Rule(
    command = "ninja -f {files_in} -j 1"
)

def build(build_dir, cflags="", compiler="clang"):
    include_paths = [build_dir, "lib/picolibc/newlib/libc/stdio"]
    libraries = []


    ninja_file = meson("picolibc/meson.build", build_dir=build_dir, cross_file="picolibc/scripts/cross-clang-thumbv6m-none-eabi.txt")


    return include_paths, libraries
