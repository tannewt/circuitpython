// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2019 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "py/obj.h"
#include "shared-module/displayio/legacy/area.h"
#include "shared-module/displayio/Palette.h"

// See Group.h for the compositing core backend selection.
#if CIRCUITPY_DISPLAYIO_LVGL
#include "shared-module/displayio/lvgl/TileGrid.h"
#else
#include "shared-module/displayio/legacy/TileGrid.h"
#endif
