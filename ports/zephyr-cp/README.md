# Zephyr

This is an initial port of CircuitPython onto Zephyr. We intend on migrating all
existing boards onto Zephyr. To start, we'll only support new boards. Existing
boards will be switched as the Zephyr port reaches parity with the existing
implementation.

## Getting Started

First, install Zephyr tools (see [Zephyr's Getting Started Guide](https://docs.zephyrproject.org/4.0.0/develop/getting_started/index.html)). (These are `fish` commands because that's what Scott uses.)


```sh
pip install west
west init -l zephyr-config
west update
west zephyr-export
pip install -r lib/zephyr/scripts/requirements.txt
west sdk install
```

Now to build from the top level:

```sh
make BOARD=nordic_nrf7002dk
```

This uses Zephyr's cmake to generate Makefiles that then delegate to
`tools/cpbuild/build_circuitpython.py` to build the CircuitPython bits in parallel.
