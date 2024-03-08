import inspect
import pathlib
import shlex
import subprocess
import sys

import embedded
import embedded.cli
import embedded.compiler
from embedded.build import meson
from embedded.build import ninja

import asyncio

async def build(cpu: embedded.CPU, compiler: embedded.Compiler, build_directory: pathlib.Path):
    absolute_source_dir = pathlib.Path(__file__).parent / "picolibc"
    build_path = build_directory / cpu.unique_id
    build_path.mkdir(parents=True, exist_ok=True)

    await meson.setup(absolute_source_dir, build_path, cpu, compiler, reconfigure=True, options={"semihost": False, "tests": False, "multilib": False})

    await ninja.run(build_path)

    picolibc_includes = [absolute_source_dir / "include", absolute_source_dir / "newlib/libc/stdio", build_path]
    print(build_path)
    picolibc_a = list(build_path.glob("**/*.a"))
    return picolibc_includes, picolibc_a

# for compiler rt
# cmake -DCOMPILER_RT_BAREMETAL_BUILD=ON -DCOMPILER_RT_BUILD_LIBFUZZER=OFF -DCOMPILER_RT_BUILD_PROFILE=OFF -DCOMPILER_RT_BUILD_SANITIZERS=OFF -DCOMPILER_RT_BUILD_XRAY=OFF -DCOMPILER_RT_INCLUDE_TESTS=OFF -DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY -DCMAKE_C_COMPILER_TARGET="arm-none-eabihf" -DCMAKE_ASM_COMPILER_TARGET="arm-none-eabihf" -DCMAKE_BUILD_TYPE=Release -DLLVM_USE_LINKER=lld -DLLVM_DEFAULT_TARGET_TRIPLE="arm-none-eabihf" -DLLVM_TARGETS_TO_BUILD="ARM" -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_FLAGS="-march=armv8-m.main+fp+dsp -mthumb -mabi=aapcs-linux -mcpu=cortex-m33 -mfloat-abi=hard" -DCMAKE_ASM_FLAGS="-march=armv8-m.main+fp+dsp -mthumb -mabi=aapcs-linux -mcpu=cortex-m33 -mfloat-abi=hard" -DCMAKE_C_COMPILER=clang -G "Ninja" ../compiler-rt/

async def two_at_a_time():
    cpu = embedded.cpu.arm.CortexM33(floating_point=True, dsp=True)
    cpu2 = embedded.cpu.arm.CortexM0Plus(small_multiply=True)
    build_directory = pathlib.Path(__file__).parent / "build"
    async with asyncio.TaskGroup() as tg:
        task1 = tg.create_task(build(cpu, build_directory))
        task2 = tg.create_task(build(cpu2, build_directory))

if __name__ == "__main__":
    embedded.cli.run(build)
    # embedded.cli.run(two_at_a_time)
