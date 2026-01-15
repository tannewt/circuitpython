# CircuitPython Development Reference

## Build Commands
- Build CircuitPython for specific board: `cd ports/[port_name] && make BOARD=[board_name]`
- Build with multiple cores: `make BOARD=[board_name] -j[cores]`
- Build mpy-cross: `make -C mpy-cross`
- Update submodules: `make fetch-all-submodules` or `make fetch-port-submodules`
- Run specific test: `cd tests && ./run-tests.py [test_file.py]`
- Run test suite: `cd tests && ./run-tests.py`
- Format code: `pre-commit run` (C: uncrustify, Python: ruff)

## Code Style Guidelines
- Follow MicroPython conventions: github.com/micropython/micropython/blob/master/CODECONVENTIONS.md
- Use composition over inheritance for device drivers
- Properties for device state, methods for actions
- Use native types and constants from `micropython import const`
- Maintain API compatibility with CPython when possible
- Document all modules, classes, methods, and variables with rST-style docstrings
- Follow register naming conventions and units from Adafruit Unified Sensor Driver
- Avoid memory allocations in drivers where possible
- For naming, use "main/peripheral" instead of "master/slave" terminology
- For I2C/SPI, use BusDevice library and Register library when possible