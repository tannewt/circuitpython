import argparse
import inspect
import pathlib
import shlex
import subprocess

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
    def __init__(self, extensions: set[str], bits=32):
        self.extensions = extensions
        self.unique_id = "rv" + str(bits) + "".join(extensions)
        self.bits = bits

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

def embedded_build(function):
    function_args = []
    parser = argparse.ArgumentParser()
    for param in inspect.signature(function).parameters.values():
        if param.annotation == CPU:
            target_help = "cpu target name"
            if cmsis_packs:
                target_help += " or ARM microcontroller name"
            parser.add_argument("-t", "--target", type=str,
                    help=target_help)
            function_args.append("target")

        # print(param, type(param), param.annotation)

    cli_args = parser.parse_args()

    for i, farg in enumerate(function_args):
        if getattr(cli_args, farg) is None:
            parser.print_help()
            return

        if farg == "target":
            print(cli_args.target)
            if cmsis_packs:
                print("looking in cmsis packs")
                cmsis_cache = cmsis_packs.Cache(True, False)

                target_processor = None
                for part in cmsis_cache.index.keys():
                    if cli_args.target in part:
                        # print(part)
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
                function_args[i] = cpu

    function(*function_args)

def create_meson_cross_file(cpu: CPU):
    cflags = ",".join(f"'{flag}'" for flag in cpu.get_arch_cflags(None))
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

[properties]
c_args = [{cflags}]
"""

class Artifacts:
    async def is_changed() -> bool:
        return True

def build(cpu: CPU) -> Artifacts:
    source_dir = (pathlib.Path(__file__).parent / "picolibc").relative_to(pathlib.Path.cwd(), walk_up=True)
    build_path = pathlib.Path("build") / cpu.unique_id
    build_path.mkdir(parents=True, exist_ok=True)
    cross_file = build_path / "cross_file.txt"
    cross_file.write_text(create_meson_cross_file(cpu))

    cmd = f"meson setup --reconfigure -Dtests=false -Dmultilib=false --cross-file {cross_file} {source_dir} {build_path}"
    print(cmd)
    subprocess.run(shlex.split(cmd), capture_output=True)

    cmd = f"ninja"
    print(cmd)
    result = subprocess.run(shlex.split(cmd), capture_output=True, cwd=build_path)
    print(result.returncode)
    print(result.stdout)
    return Artifacts()


if __name__ == "__main__":
    embedded_build(build)
