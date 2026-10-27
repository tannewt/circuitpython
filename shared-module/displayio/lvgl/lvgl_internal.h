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

#if CIRCUITPY_DISPLAYIO_LVGL

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

// Set up LVGL for displayio: screen background, custom image decoder. Called by the
// port's display constructor.
void displayio_lvgl_init(void);
void displayio_lvgl_register_image_decoder(void);

// Attach/detach the compositor's root group. Implemented in lvgl/Group.c. NULL detach.
void displayio_lvgl_set_root_group(displayio_group_t *group);
void displayio_lvgl_set_colorspace(const _displayio_colorspace_t *colorspace);

// Group/TileGrid mirror primitives, implemented in Group.c and TileGrid.c.
void displayio_lvgl_group_attach(displayio_group_t *self, lv_obj_t *parent);
void displayio_lvgl_group_detach(displayio_group_t *self);
void displayio_lvgl_tilegrid_attach(displayio_tilegrid_t *self, lv_obj_t *parent);
void displayio_lvgl_tilegrid_detach(displayio_tilegrid_t *self);
void displayio_lvgl_tilegrid_update_scale(displayio_tilegrid_t *self);
void displayio_lvgl_tilegrid_refresh_tiles(displayio_tilegrid_t *self);
uint8_t displayio_lvgl_bitmap_bpp(mp_obj_t bitmap);
void displayio_lvgl_dsc_cache_drop(displayio_lvgl_image_dsc_t *dsc);
void displayio_lvgl_mirror_gc(void);

// Bitmap and Palette content-change notification, implemented in Bitmap.c and
// Palette.c. TileGrids register as mirror users when attached; changes to the
// shared bitmap or palette invalidate their images.
void displayio_lvgl_bitmap_register_user(displayio_bitmap_t *self, displayio_tilegrid_t *tilegrid);
void displayio_lvgl_bitmap_unregister_user(displayio_bitmap_t *self, displayio_tilegrid_t *tilegrid);
void displayio_lvgl_bitmap_notify_users(displayio_bitmap_t *self, int16_t x1, int16_t y1, int16_t x2, int16_t y2);
void displayio_lvgl_palette_register_user(displayio_palette_t *self, displayio_tilegrid_t *tilegrid);
void displayio_lvgl_palette_unregister_user(displayio_palette_t *self, displayio_tilegrid_t *tilegrid);
void displayio_lvgl_palette_notify_users(displayio_palette_t *self, int16_t x1, int16_t y1, int16_t x2, int16_t y2);
void displayio_lvgl_tilegrid_bitmap_changed(displayio_tilegrid_t *self, int16_t x1, int16_t y1, int16_t x2, int16_t y2);
void displayio_lvgl_tilegrid_palette_changed(displayio_tilegrid_t *self, int16_t x1, int16_t y1, int16_t x2, int16_t y2);

#endif  // CIRCUITPY_DISPLAYIO_LVGL
