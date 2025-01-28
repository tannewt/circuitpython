ALL_BOARDS_IN_PORT := $($(wildcard boards/*/*):boards/=:/=)
# An incorrect BOARD might have been specified, so check against the list.
# There is deliberately no space after the :=
VALID_BOARD :=$(filter $(BOARD),$(ALL_BOARDS_IN_PORT))

# If the build directory is not given, make it reflect the board name.
BUILD ?= build-$(BOARD)

all:
	west build -b $(BOARD) -d $(BUILD) -- -DZEPHYR_BOARD_ALIASES=$(shell pwd)/boards/board_aliases.cmake
