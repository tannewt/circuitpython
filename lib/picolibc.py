from hancho import *

meson = Rule(
    base=build_config,
    command="meson setup --reconfigure -Dtests=false -Dmultilib=false --cross-file {cross_file} picolibc {build_path}",
    build_files=["build.ninja"]
)

ninja = Rule(
    base=build_config,
    command=["ninja -j {job_count}", "touch {files_out}"],
    job_count=1 if build_config.jobs == 1 else (build_config.jobs // 2),
)

print(f"{build_config=}")

def build(build_dir, cflags="", compiler="clang"):
    include_paths = [build_dir, "lib/picolibc/newlib/libc/stdio"]
    libraries = []

    ninja_file = meson(
        "picolibc/meson.build",
        build_path=build_dir,
        cross_file="picolibc/scripts/cross-clang-thumbv6m-none-eabi.txt",
    )

    picolibc_a = ninja(
        ninja_file,
        build_files=["newlib/libm.a", "newlib/libc.a", "newlib/libg.a"],
        command_path=build_dir,
    )

    return include_paths, picolibc_a
