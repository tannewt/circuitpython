ALL_BOARDS_IN_PORT := $(patsubst boards/%,%,$(wildcard boards/*/*))

# Needed to print the board list one board per line below.
empty :=
space := $(empty) $(empty)
define newline


endef
# An incorrect BOARD might have been specified, so check against the list.
# Boards are stored as vendor/name but BOARD is the flattened vendor_name, so
# flatten the list the same way (vendors contain no underscores). There is
# deliberately no space after the :=
VALID_BOARD :=$(filter $(BOARD),$(subst /,_,$(ALL_BOARDS_IN_PORT)))

# Fail fast on a mistyped BOARD instead of letting west build a board the port
# isn't configured for (which dies later with confusing Kconfig errors). Targets
# like `clean` don't take a BOARD, so only check when one is given.
ifneq ($(strip $(BOARD)),)
ifeq ($(strip $(VALID_BOARD)),)
$(error Invalid BOARD '$(BOARD)'. Valid boards:$(newline)$(subst $(space),$(newline),$(subst /,_,$(ALL_BOARDS_IN_PORT))))
endif
endif

# If the build directory is not given, make it reflect the board name.
BUILD ?= build-$(BOARD)

TRANSLATION ?= en_US

# Compute shield args once. Command-line SHIELD/SHIELDS values override board defaults from circuitpython.toml.
ifneq ($(strip $(BOARD)),)
WEST_SHIELD_ARGS := $(shell SHIELD_ORIGIN="$(origin SHIELD)" SHIELDS_ORIGIN="$(origin SHIELDS)" SHIELD="$(SHIELD)" SHIELDS="$(SHIELDS)" python cptools/get_west_shield_args.py $(BOARD))
endif

WEST_CMAKE_ARGS := -DZEPHYR_BOARD_ALIASES=$(CURDIR)/boards/board_aliases.cmake -Dzephyr-cp_TRANSLATION=$(TRANSLATION)

# Board files live in the board's own folder as board.overlay / board.conf. Zephyr only auto-applies
# boards/<name>.overlay and boards/<name>.conf named after the RESOLVED Zephyr board, so it finds
# neither those (wrong directory) nor files named after a board declared through cp_board_alias()
# (wrong name) - the build silently produces a different firmware instead of warning. Pass them
# explicitly when they exist.
CP_BOARD_DIR := $(CURDIR)/boards/$(firstword $(subst _, ,$(BOARD)))/$(patsubst $(firstword $(subst _, ,$(BOARD)))_%,%,$(BOARD))
# DTC_OVERLAY_FILE, not EXTRA_: a board overlay REPLACES app.overlay (Zephyr only falls back to
# app.overlay when no board overlay is found), which is why the board overlays here include it
# themselves - and why one board that has no USB deliberately does not. Adding the overlay instead
# would force app.overlay on every board. socs/ ships .conf only, so nothing else is displaced.
CP_BOARD_OVERLAY := $(wildcard $(CP_BOARD_DIR)/board.overlay)
ifneq ($(CP_BOARD_OVERLAY),)
WEST_CMAKE_ARGS += -Dzephyr-cp_DTC_OVERLAY_FILE=$(CP_BOARD_OVERLAY)
endif
CP_BOARD_CONF := $(wildcard $(CP_BOARD_DIR)/board.conf)

# When DEBUG=1, apply additional Kconfig fragments for debug-friendly settings.
DEBUG_CONF_FILE ?= $(CURDIR)/debug.conf
ifeq ($(DEBUG),1)
ifneq ($(CP_BOARD_CONF),)
CP_BOARD_CONF := $(CP_BOARD_CONF);$(DEBUG_CONF_FILE)
else
CP_BOARD_CONF := $(DEBUG_CONF_FILE)
endif
endif
ifneq ($(CP_BOARD_CONF),)
WEST_CMAKE_ARGS += -Dzephyr-cp_EXTRA_CONF_FILE=$(CP_BOARD_CONF)
endif

# Boards can opt into building the MCUboot bootloader (and thus BLE OTA
# updates) by shipping a sysbuild.conf fragment. It sets sysbuild-level
# Kconfig (SB_CONFIG_BOOTLOADER_MCUBOOT, bootloader mode, signing keys, ...).
CP_BOARD_SYSBUILD_CONF := $(wildcard $(CP_BOARD_DIR)/sysbuild.conf)
ifneq ($(CP_BOARD_SYSBUILD_CONF),)
WEST_CMAKE_ARGS += -DSB_EXTRA_CONF_FILE=$(CP_BOARD_SYSBUILD_CONF)
endif

# Boards that enable the bootloader can also tune the MCUboot image itself
# (Kconfig for the bootloader build) via a mcuboot.conf fragment.
CP_BOARD_MCUBOOT_CONF := $(wildcard $(CP_BOARD_DIR)/mcuboot.conf)
ifneq ($(CP_BOARD_MCUBOOT_CONF),)
WEST_CMAKE_ARGS += -Dmcuboot_EXTRA_CONF_FILE=$(CP_BOARD_MCUBOOT_CONF)
endif

.PHONY: $(BUILD)/zephyr-cp/zephyr/zephyr.elf flash recover debug debug-jlink debugserver attach run run-sim clean menuconfig all clean-all sim clean-sim test fetch-port-submodules mcuboot

$(BUILD)/zephyr-cp/zephyr/zephyr.elf:
	python cptools/pre_zephyr_build_prep.py $(BOARD)
	west build -b $(BOARD) -d $(BUILD) $(WEST_SHIELD_ARGS) --sysbuild -- $(WEST_CMAKE_ARGS)

$(BUILD)/firmware.elf: $(BUILD)/zephyr-cp/zephyr/zephyr.elf
	cp $^ $@

$(BUILD)/firmware.hex: $(BUILD)/zephyr-cp/zephyr/zephyr.elf
	cp $(BUILD)/zephyr-cp/zephyr/zephyr.hex $@

$(BUILD)/firmware.exe: $(BUILD)/zephyr-cp/zephyr/zephyr.elf
	cp $(BUILD)/zephyr-cp/zephyr/zephyr.exe $@

$(BUILD)/firmware.uf2: $(BUILD)/zephyr-cp/zephyr/zephyr.elf
	python zephyr/scripts/build/uf2conv.py -c -b $(shell grep CONFIG_FLASH_LOAD_OFFSET $(BUILD)/zephyr-cp/zephyr/.config | cut -d= -f2) -o $@ $(BUILD)/zephyr-cp/zephyr/zephyr.signed.bin

$(BUILD)/firmware.rps: $(BUILD)/zephyr-cp/zephyr/zephyr.elf
	cp $(BUILD)/zephyr-cp/zephyr/zephyr.rps $@

flash: $(BUILD)/zephyr-cp/zephyr/zephyr.elf
	west flash -d $(BUILD)

recover: $(BUILD)/zephyr-cp/zephyr/zephyr.elf
		west flash --recover -d $(BUILD)

debug: $(BUILD)/zephyr-cp/zephyr/zephyr.elf
	west debug -d $(BUILD)

debug-jlink: $(BUILD)/zephyr-cp/zephyr/zephyr.elf
	west debug --runner jlink -d $(BUILD)

debugserver: $(BUILD)/zephyr-cp/zephyr/zephyr.elf
	west debugserver -d $(BUILD)

attach: $(BUILD)/zephyr-cp/zephyr/zephyr.elf
	west attach -d $(BUILD)

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
	west build $(WEST_SHIELD_ARGS) --sysbuild -d $(BUILD) -t menuconfig -- $(WEST_CMAKE_ARGS)

mcuboot:
	west build -d $(BUILD) --domain mcuboot

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

# Build all sim boards concurrently using the same jobserver as `make all`.
sim:
	+python cptools/build_all_boards.py --vendor native --continue-on-error

clean-sim:
	rm -rf $(wildcard build-native_*)

test: build-native_native_sim/zephyr-cp/zephyr/zephyr.exe
	pytest cptools/tests
	pytest tests/ -v
