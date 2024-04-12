import argparse
import inspect
import pathlib
import shlex
import subprocess
import sys

from typing import Optional

try:
    import cmsis_pack_manager as cmsis_packs
except ImportError:
    cmsis_packs = None

class Microcontroller:
    pass

class Compiler:
    pass

class CPU:
    pass

    def get_arch_cflags(self, compiler: Compiler) -> str:
        return ""

class RISCV(CPU):
    def __init__(self, extensions: tuple[str], bits=32):
        self.extensions = extensions
        self.unique_id = "rv" + str(bits) + "".join(extensions)
        self.bits = bits

    def get_arch_cflags(self, compiler: Compiler) -> list[str]:
        joined = "_".join(self.extensions).lower()
        flags = [f"--target=riscv{self.bits}", f"-march=rv{self.bits}{joined}"]
        return flags

class Hazard3(RISCV):
    def __init__(self):
        super().__init__(("I", "M", "A", "C", "Zicsr", "Zba", "Zbb", "Zbc", "Zbs", "Zbkb", "Zcb", "Zcmp"))
        self.unique_id = "hazard3"


class ARM(CPU):
    def __init__(self, mcpu: str, floating_point: bool = False):
        self.mcpu = mcpu
        self.floating_point = floating_point
        self.unique_id = mcpu

    def get_arch_cflags(self, compiler: Compiler) -> list[str]:
        flags = [f"-mcpu={self.mcpu}", "-mthumb"]
        if self.floating_point:
            flags.extend(("-mfloat-abi=hard", "-mfpu=auto"))
        return flags


    @staticmethod
    def from_pdsc(description: dict) -> Optional[CPU]:
        if "core" in description:
            core = description["core"]
            print(core)
            if core == "CortexM0Plus":
                print("cm0")
                return CortexM0Plus()
            elif core == "CortexM4":
                return CortexM4(description["fpu"] != "None")
            elif core == "CortexM7":
                return CortexM7(description["fpu"])
            elif core == "CortexM33":
                return CortexM33(description["fpu"])
        return None

class CortexM0Plus(ARM):
    def __init__(self, small_multiply=False):
        if small_multiply:
            small_multiply = ".small-multiply"
        else:
            small_multiply = ""
        super().__init__("cortex-m0plus" + small_multiply, False)

class CortexM4(ARM):
    def __init__(self, floating_point: bool):
        if floating_point:
            fp = ""
        else:
            fp = "+nofp"
        super().__init__("cortex-m4" + fp, floating_point)

class CortexM7(ARM):
    def __init__(self, floating_point: str):
        if floating_point:
            fp = ""
        else:
            fp = "+nofp"
        super().__init__("cortex-m7" + fp, floating_point)

class CortexM33(ARM):
    def __init__(self, floating_point: str, dsp: bool):
        if floating_point:
            fp = ""
        else:
            fp = "+nofp"
        if dsp:
            dsp_flag = ""
        else:
            dsp_flag = "+nodsp"
        super().__init__("cortex-m33" + fp + dsp_flag, floating_point)

def get_cpu_from_mcu(substr):
    if not cmsis_packs:
        return None

    print("looking in cmsis packs")
    cmsis_cache = cmsis_packs.Cache(True, False)

    target_processor = None
    for part in cmsis_cache.index.keys():
        if substr in part:
            print(part)
            device_info = cmsis_cache.index[part]
            if "processor" in device_info:
                print(device_info["processor"])
            if "processors" in device_info:
                for processor in device_info["processors"]:
                    if "svd" in processor:
                        del processor["svd"]
                    if target_processor is None:
                        target_processor = processor
                    elif target_processor != processor:
                        print("mismatched processor", target_processor, processor)
    print(target_processor)
    cpu = ARM.from_pdsc(target_processor)
    print(cpu.get_arch_cflags(None))
    return cpu

def get_cpu_from_name(name):
    name = name.lower()
    if name == "hazard3":
        return Hazard3()
    elif name == "riscv32imac":
        return RISCV(("I", "M", "A", "C"))
    elif name in ("cm0+", "cortex-m0plus", "cortex-m0+"):
        return CortexM0Plus()
    elif name in ("cm4", "cortex-m4"):
        return CortexM4(False)
    elif name in ("cm4f", "cortex-m4f"):
        return CortexM4(True)
    elif name in ("cm7", "cortex-m7"):
        return CortexM7(False)

def embedded_build(function):
    function_args = []
    parser = argparse.ArgumentParser()
    for param in inspect.signature(function).parameters.values():
        if param.annotation == CPU:
            cpu_help = "cpu target name"
            parser.add_argument("-c", "--cpu", type=str,
                    help=cpu_help)
            function_args.append("cpu")
        if param.annotation == CPU or param.annotation == Microcontroller:
            mcu_help = "mcu target name"
            parser.add_argument("-m", "--mcu", type=str,
                    help=mcu_help)
            if param.annotation == Microcontroller:
                function_args.append("mcu")

        # print(param, type(param), param.annotation)

    cli_args = parser.parse_args()

    for i, farg in enumerate(function_args):
        if farg == "cpu":
            print(cli_args)
            if cli_args.cpu:
                function_args[i] = get_cpu_from_name(cli_args.cpu)
            elif cli_args.mcu:
                cpu = get_cpu_from_mcu(cli_args.mcu)
                function_args[i] = cpu

    function(*function_args)

def create_meson_cross_file(cpu: CPU):
    cflags = ",".join(f"'{flag}'" for flag in cpu.get_arch_cflags(None))
    link_flags = cflags
    if isinstance(cpu, ARM):
        cpu_family = "arm"
        c_compiler = "arm-none-eabi-gcc"
        cpp_compiler = "arm-none-eabi-g++"
        ar = "arm-none-eabi-ar"
        strip = "arm-none-eabi-strip"
    elif isinstance(cpu, RISCV):
        c_compiler = "clang"
        cpp_compiler = "clang++"
        ar = "llvm-ar"
        strip = "llvm-strip"
        cpu_family = f"riscv{cpu.bits}"
        link_flags += ",'-fuse-ld=lld', '-nostdlib'"
    else:
        raise RuntimeError("Unsupported CPU")
    return f"""[binaries]
c = '{c_compiler}'
cpp = '{cpp_compiler}'
ar = '{ar}'
strip = '{strip}'

[host_machine]
system = ''
kernel = 'none'
cpu_family = '{cpu_family}'
cpu = '{cpu_family}'
endian = 'little'

[built-in options]
c_args = [{cflags}]
c_link_args = [{link_flags}]
"""

class Artifacts:
    async def is_changed() -> bool:
        return True

def build(cpu: CPU) -> Artifacts:
    absolute_source_dir = pathlib.Path(__file__).parent / "picolibc"
    if sys.version_info.minor >= 12:
        source_dir = absolute_source_dir.relative_to(pathlib.Path.cwd(), walk_up=True)
    else:
        source_dir = absolute_source_dir.relative_to(pathlib.Path.cwd())
    build_path = pathlib.Path("build") / cpu.unique_id
    build_path.mkdir(parents=True, exist_ok=True)
    cross_file = build_path / "cross_file.txt"
    cross_file.write_text(create_meson_cross_file(cpu))

    cmd = f"meson setup --reconfigure -Dsemihost=false -Dtests=false -Dmultilib=false --cross-file {cross_file} {source_dir} {build_path}"
    print(cmd)
    result = subprocess.run(shlex.split(cmd), capture_output=True)
    if result.returncode != 0:
        print(result.stdout.decode("utf-8"))

    cmd = f"ninja"
    print(cmd)
    result = subprocess.run(shlex.split(cmd), capture_output=True, cwd=build_path)
    print(result.returncode)
    if result.returncode != 0:
        print(result.stdout.decode("utf-8"))
    return Artifacts()


if __name__ == "__main__":
    embedded_build(build)
