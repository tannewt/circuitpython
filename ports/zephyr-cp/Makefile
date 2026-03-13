ALL_BOARDS_IN_PORT := $($(wildcard boards/*/*):boards/=:/=)
# An incorrect BOARD might have been specified, so check against the list.
# There is deliberately no space after the :=
VALID_BOARD :=$(filter $(BOARD),$(ALL_BOARDS_IN_PORT))

# If the build directory is not given, make it reflect the board name.
BUILD ?= build-$(BOARD)

TRANSLATION ?= en_US

.DEFAULT_GOAL := $(BUILD)/zephyr-cp/zephyr/zephyr.elf

.PHONY: $(BUILD)/zephyr-cp/zephyr/zephyr.elf flash recover debug run run-sim clean menuconfig all clean-all test fetch-port-submodules

$(BUILD)/zephyr-cp/zephyr/zephyr.elf:
	python cptools/pre_zephyr_build_prep.py $(BOARD)
	CIRCUITPY_TRANSLATION=$(TRANSLATION) west build -b $(BOARD) -d $(BUILD) --sysbuild -- -DZEPHYR_BOARD_ALIASES=$(CURDIR)/boards/board_aliases.cmake

$(BUILD)/firmware.elf: $(BUILD)/zephyr-cp/zephyr/zephyr.elf
	cp $^ $@

$(BUILD)/firmware.hex: $(BUILD)/zephyr-cp/zephyr/zephyr.elf
	cp $(BUILD)/zephyr-cp/zephyr/zephyr.hex $@

$(BUILD)/firmware.exe: $(BUILD)/zephyr-cp/zephyr/zephyr.elf
	cp $(BUILD)/zephyr-cp/zephyr/zephyr.exe $@

$(BUILD)/firmware.uf2: $(BUILD)/zephyr-cp/zephyr/zephyr.elf
	cp $(BUILD)/zephyr-cp/zephyr/zephyr.uf2 $@

flash: $(BUILD)/zephyr-cp/zephyr/zephyr.elf
	west flash -d $(BUILD)

recover: $(BUILD)/zephyr-cp/zephyr/zephyr.elf
		west flash --recover -d $(BUILD)

debug: $(BUILD)/zephyr-cp/zephyr/zephyr.elf
	west debug -d $(BUILD)

run: $(BUILD)/firmware.exe
	$^

run-sim:
	$(MAKE) BOARD=native_native_sim BUILD=build-native_native_sim build-native_native_sim/firmware.exe
	truncate -s 2M build-native_native_sim/flash.bin
	mformat -i build-native_native_sim/flash.bin ::
	@if [ -d CIRCUITPY ] && [ -n "$$(find CIRCUITPY -mindepth 1 -print -quit)" ]; then \
		echo "Populating build-native_native_sim/flash.bin from ./CIRCUITPY"; \
		mcopy -s -i build-native_native_sim/flash.bin CIRCUITPY/* ::; \
	fi
	build-native_native_sim/firmware.exe --flash=build-native_native_sim/flash.bin --flash_rm -wait_uart -rt

menuconfig:
	west build --sysbuild -d $(BUILD) -t menuconfig

clean:
	rm -rf $(BUILD)

fetch-port-submodules:
		python ../../tools/ci_fetch_deps.py zephyr-cp

# Build all boards. The + prefix allows jobserver file descriptors to be passed through.
# This enables parallel builds across all boards when invoked with `make -jN all`.
all:
	+python cptools/build_all_boards.py --continue-on-error

clean-all:
	rm -rf build build-*

test: build-native_native_sim/zephyr-cp/zephyr/zephyr.exe
	pytest cptools/tests
	pytest tests/ -v
