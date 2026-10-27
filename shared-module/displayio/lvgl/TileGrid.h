// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "py/obj.h"
#include "shared-module/displayio/Palette.h"

#include "lvgl.h"

// LVGL-based version of displayio TileGrid. A TileGrid mirrors to a single lv_image
// (single-tile sprites) or an lv_obj container with one lv_image per tile. Images share
// the source bitmap's pixel data directly when the source format matches the display
// format; otherwise a custom LVGL image decoder converts tiles. Dirty tracking is LVGL's
// own (object invalidation); see legacy/TileGrid.h for the scanline compositor's state.

// Extends the public lv_image_dsc_t with the source description the custom LVGL image
// decoder needs. For in-RAM bitmaps, dsc.data points directly into the source bitmap's
// pixel data (shared, never copied). The reserved_2 field links back to the struct itself
// so the decoder can identify our descriptors. view_buf is only used when the source data
// is already in the display's color format and there is no pixel shader — the decoded
// draw_buf then wraps the shared data.
typedef struct {
    lv_image_dsc_t dsc;
    mp_obj_t bitmap;                       // displayio Bitmap or OnDiskBitmap.
    mp_obj_t pixel_shader;                 // Palette / ColorConverter / None.
    uint16_t src_x0;                       // Tile origin in the source bitmap.
    uint16_t src_y0;
    uint32_t version;                      // Bumped when bitmap data/pixels change to drop caches.
    lv_draw_buf_t *view_buf;               // Zero-copy decoded buffer for the view case, lazily
                                           // allocated since most tiles go through the decoder.
} displayio_lvgl_image_dsc_t;

typedef struct {
    mp_obj_base_t base;
    mp_obj_t bitmap;
    mp_obj_t pixel_shader;
    int16_t x;
    int16_t y;
    uint16_t pixel_width;
    uint16_t pixel_height;
    uint16_t bitmap_width_in_tiles;
    uint16_t tiles_in_bitmap;
    uint16_t width_in_tiles;
    uint16_t height_in_tiles;
    uint16_t tile_width;
    uint16_t tile_height;
    uint16_t top_left_x;
    uint16_t top_left_y;
    void *tiles;  // Can be either uint8_t* or uint16_t* depending on tiles_in_bitmap
    bool inline_tiles : 1;
    bool in_group : 1;
    bool flip_x : 1;
    bool flip_y : 1;
    bool transpose_xy  : 1;
    bool hidden : 1;
    bool hidden_by_parent : 1;
    uint16_t padding : 9;
    // LVGL mirror state.
    lv_obj_t *lv_obj;                    // Container object (or the single lv_image for sprites).
    lv_obj_t **tile_images;              // One lv_image per tile, NULL when single-image.
    displayio_lvgl_image_dsc_t *tile_dscs; // Per-tile descriptors sharing the bitmap's data.
    uint32_t tile_count;                 // Number of descriptors allocated.
    uint16_t lv_scale;                   // Scale inherited from the parent group.
    uint8_t lv_pad;
} displayio_tilegrid_t;

// One displayio TileGrid registered with a shared Bitmap or Palette: pushed change
// notifications walk this list (head pointers live on the Bitmap/Palette object).
typedef struct displayio_lvgl_mirror_user {
    displayio_tilegrid_t *tilegrid;
    struct displayio_lvgl_mirror_user *next;
} displayio_lvgl_mirror_user_t;

void displayio_tilegrid_set_hidden_by_parent(displayio_tilegrid_t *self, bool hidden);
bool displayio_tilegrid_get_rendered_hidden(displayio_tilegrid_t *self);
void displayio_tilegrid_mark_tile_dirty(displayio_tilegrid_t *self, uint16_t x, uint16_t y);
