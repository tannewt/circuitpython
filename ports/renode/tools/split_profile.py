# Split a profile file into smaller chunks.

import pathlib
import sys

input_file = pathlib.Path(sys.argv[-1])

supervisor = input_file.with_suffix(".supervisor" + input_file.suffix)
boot_py = input_file.with_suffix(".boot_py" + input_file.suffix)
code_py = input_file.with_suffix(".code_py" + input_file.suffix)

supervisor_f = supervisor.open("w")
boot_py_f = boot_py.open("w")
code_py_f = code_py.open("w")


def write(line):
    if "run_boot_py" in line:
        boot_py_f.write(line)
    if "run_code_py" in line:
        code_py_f.write(line)


delay_stack = None
delay_time = 0
for line in input_file.open():
    # consolidate delay calls except background tasks
    if "mp_hal_delay_ms" in line and (
        "background_callback_run" not in line
        or line.rindex("mp_hal_delay_ms") > line.rindex("background_callback_run")
    ):
        split = line.rsplit(" ", 1)
        if delay_stack is None:
            delay_stack = split[0]
        delay_time += int(split[1])
        # print("drop", line.strip())
        continue
    elif delay_time > 0:
        # print("bg", line.strip())
        # if "mp_hal_delay_ms" in line:
        #     print(line.rindex("mp_hal_delay_ms"), line.rindex("background_callback_run"))
        # print(f"{delay_stack} {delay_time}")
        write(f"{delay_stack} {delay_time}\n")
        delay_time = 0
        delay_stack = None

    write(line)

supervisor_f.close()
boot_py_f.close()
code_py_f.close()
