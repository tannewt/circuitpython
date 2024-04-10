import argparse
import inspect

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

class ARM(CPU):
    def __init__(self, mcpu: str, floating_point: bool = False):
        self.mcpu = mcpu
        self.floating_point = floating_point

    def get_arch_cflags(self, compiler: Compiler) -> str:
        flags = f"-mcpu={self.mcpu} -mthumb"
        if self.floating_point:
            flags += " -mfloat-abi=hard -mfpu=auto"
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

def embedded_build(function):
    parser = argparse.ArgumentParser()
    for param in inspect.signature(function).parameters.values():
        if param.annotation == CPU:
            target_help = "cpu target name"
            if cmsis_packs:
                target_help += " or ARM microcontroller name"
            parser.add_argument("-t", "--target", type=str,
                    help=target_help)

        # print(param, type(param), param.annotation)

    args = parser.parse_args()

    print(args.target)
    if cmsis_packs:
        print("looking in cmsis packs")
        cmsis_cache = cmsis_packs.Cache(True, False)

        target_processor = None
        for part in cmsis_cache.index.keys():
            if args.target in part:
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



class Artifacts:
    async def is_changed() -> bool:
        return True

def build(cpu: CPU) -> Artifacts:
    return Artifacts()


if __name__ == "__main__":
    embedded_build(build)
