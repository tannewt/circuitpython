import argparse
import inspect

try:
    import cmsis_pack_manager as cmsis_packs
except ImportError:
    cmsis_packs = None

class Microcontroller:
    pass

class CPU:
    pass

class Compiler:
    pass

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
                        if target_processor is None:
                            target_processor = processor
                        elif target_processor != processor:
                            print("mismatched processor", target_processor, processor)
        print(target_processor)



class Artifacts:
    async def is_changed() -> bool:
        return True

def build(cpu: CPU) -> Artifacts:
    return Artifacts()


if __name__ == "__main__":
    embedded_build(build)
