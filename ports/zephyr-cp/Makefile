ALL_BOARDS_IN_PORT := $($(wildcard boards/*/*):boards/=:/=)
# An incorrect BOARD might have been specified, so check against the list.
# There is deliberately no space after the :=
VALID_BOARD :=$(filter $(BOARD),$(ALL_BOARDS_IN_PORT))

# If the build directory is not given, make it reflect the board name.
BUILD ?= build-$(BOARD)

TRANSLATION ?= en_US

.PHONY: $(BUILD)/zephyr-cp/zephyr/zephyr.elf flash debug clean

$(BUILD)/zephyr-cp/zephyr/zephyr.elf:
	west build -b $(BOARD) -d $(BUILD) --sysbuild -- -DZEPHYR_BOARD_ALIASES=$(CURDIR)/boards/board_aliases.cmake -Dzephyr-cp_TRANSLATION=$(TRANSLATION)

$(BUILD)/firmware.elf: $(BUILD)/zephyr-cp/zephyr/zephyr.elf
	cp $^ $@

flash: $(BUILD)/zephyr-cp/zephyr/zephyr.elf
	west flash --sysbuild -d $(BUILD)

debug: $(BUILD)/zephyr-cp/zephyr/zephyr.elf
	west debug --sysbuild -d $(BUILD)

clean:
	rm -rf $(BUILD)
