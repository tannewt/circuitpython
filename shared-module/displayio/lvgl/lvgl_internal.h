// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Internal declarations shared by the files that implement the LVGL-based
// displayio compositing core (shared-module/displayio/lvgl/).

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "py/obj.h"
#include "shared-module/displayio/Group.h"
#include "shared-module/displayio/TileGrid.h"
#include "shared-module/displayio/Bitmap.h"
#include "shared-module/displayio/Palette.h"
#include "shared-module/displayio/OnDiskBitmap.h"
#include "shared-module/displayio/lvgl/ImageRoot.h"
#include "shared-module/displayio/lvgl/Palette.h"

#if CIRCUITPY_DISPLAYIO_LVGL

// Port hook: initialize LVGL itself and its tick source. Implemented by each port's
// common-hal and safe to call repeatedly.
void displayio_lvgl_port_init(void);

// Port-supplied recursion-safe lock around LVGL. The RTOS renders LVGL on its own
// thread (e.g. a Zephyr workqueue), so CircuitPython-side mirror mutations must be
// excluded from rendering. Depth counting in the shared wrappers means nested calls
// are safe.
void displayio_lvgl_port_lock(void);
void displayio_lvgl_port_unlock(void);

// Shared lock wrappers with depth counting. All display-side (CP thread) LVGL calls
// must happen inside these.
void displayio_lvgl_lock(void);
void displayio_lvgl_unlock(void);

// Set up LVGL for displayio: screen background. Called by the
// port's display constructor.
void displayio_lvgl_init(void);

// Register displayio's per-display refresh hooks, including the compose pass that brings every
// attached TileGrid's buffer up to date before each refresh draws anything. Called once per LVGL
// display.
void displayio_lvgl_register_display(lv_display_t *display);

// Attach/detach the compositor's root group. Implemented in lvgl/Group.c. NULL detach.
void displayio_lvgl_set_root_group(displayio_group_t *group);

#endif  // CIRCUITPY_DISPLAYIO_LVGL
