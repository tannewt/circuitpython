ifneq ($(lastword a b),b)
$(error These Makefiles require make 3.81 or newer)
endif

# Set TOP to be the path to get from the current directory (where make was
# invoked) to the top of the tree. $(lastword $(MAKEFILE_LIST)) returns
# the name of this makefile relative to where make was invoked.
#
# We assume that this file is in the py directory so we use $(dir ) twice
# to get to the top of the tree.

THIS_MAKEFILE := $(lastword $(MAKEFILE_LIST))
TOP := $(patsubst %/py/mkenv.mk,%,$(THIS_MAKEFILE))

# CIRCUITPY-CHANGE: verbosity differences, STEPECHO
# Turn on increased build verbosity by defining BUILD_VERBOSE in your main
# Makefile or in your environment. You can also use V="steps commands rules" or any combination thereof
# on the make command line.

ifeq ("$(origin V)", "command line")
BUILD_VERBOSE=$(V)
endif
ifndef BUILD_VERBOSE
$(info - Verbosity options: any combination of "steps commands rules", as `make V=...` or env var BUILD_VERBOSE)
BUILD_VERBOSE = ""
endif

ifneq ($(filter steps,$(BUILD_VERBOSE)),)
STEPECHO = @echo
else
STEPECHO = @:
endif

ifneq ($(filter commands,$(BUILD_VERBOSE)),)
Q =
else
Q = @
endif

# CIRCUITPY-CHANGE
ifneq ($(filter rules,$(BUILD_VERBOSE)),)
# This clever shell redefinition will print out the makefile line that is causing an action.
# Note that -j can cause the order to be confusing.
# https://www.cmcrossroads.com/article/tracing-rule-execution-gnu-make
OLD_SHELL := $(SHELL)
SHELL = $(warning BUILDING $@)$(OLD_SHELL)
endif

# default settings; can be overridden in main Makefile

PY_SRC ?= $(TOP)/py
BUILD ?= build

MICROPY_BUILD_CLANG ?= 0
ifneq ($(SYSROOT),)
export PATH := $(SYSROOT)/bin:$(PATH)
endif

RM = rm
ECHO = @echo
CP = cp
MKDIR = mkdir
SED = sed
CAT = cat
TOUCH = touch
PYTHON = python3
ZIP = zip

AS = $(CROSS_COMPILE)as
ifeq ($(MICROPY_BUILD_CLANG),1)
CC = clang
else
CC = $(CROSS_COMPILE)gcc
endif
CPP = $(CC) -E
CXX = $(CROSS_COMPILE)g++
GDB = $(CROSS_COMPILE)gdb
LD = $(CROSS_COMPILE)ld
OBJCOPY = $(CROSS_COMPILE)objcopy
SIZE = $(CROSS_COMPILE)size
STRIP = $(CROSS_COMPILE)strip
AR = $(CROSS_COMPILE)ar
WINDRES = $(CROSS_COMPILE)windres

MAKE_MANIFEST = $(PYTHON) $(TOP)/tools/makemanifest.py
MAKE_FROZEN = $(PYTHON) $(TOP)/tools/make-frozen.py
MPY_TOOL = $(PYTHON) $(TOP)/tools/mpy-tool.py
# CIRCUITPY-CHANGE
PREPROCESS_FROZEN_MODULES = PYTHONPATH=$(TOP)/tools/python-semver $(TOP)/tools/preprocess_frozen_modules.py

MPY_LIB_SUBMODULE_DIR = $(TOP)/lib/micropython-lib
MPY_LIB_DIR = $(MPY_LIB_SUBMODULE_DIR)

ifeq ($(MICROPY_MPYCROSS),)
MICROPY_MPYCROSS = $(TOP)/mpy-cross/build/mpy-cross
MICROPY_MPYCROSS_DEPENDENCY = $(MICROPY_MPYCROSS)
endif

all:
.PHONY: all

.DELETE_ON_ERROR:

MKENV_INCLUDED = 1
