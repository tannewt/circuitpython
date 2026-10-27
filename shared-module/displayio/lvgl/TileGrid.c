// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2019 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// LVGL-based displayio TileGrid implementation. A single-tile TileGrid (a sprite) is one
// lv_image; larger grids are a plain lv_obj container with one lv_image per tile, each
// sourced from a descriptor that shares the source bitmap's pixel data. Pixels reach LVGL
// through a custom image decoder that applies the same pixel shaders as the legacy
// compositor, so output matches bit-for-bit.
//
// All mirror mutations run synchronously on the CircuitPython thread under the port's
// LVGL lock; the RTOS renders LVGL on its own thread. Shared Bitmaps and Palettes push
// content changes to their registered TileGrid users.

#include "shared-bindings/displayio/TileGrid.h"

#include <string.h>

#include "py/runtime.h"
#include "shared-bindings/displayio/Bitmap.h"
#include "shared-bindings/displayio/ColorConverter.h"
#include "shared-bindings/displayio/OnDiskBitmap.h"
#include "shared-bindings/displayio/Palette.h"
#if CIRCUITPY_TILEPALETTEMAPPER
#include "shared-bindings/tilepalettemapper/TilePaletteMapper.h"
#endif
#include "shared-module/displayio/lvgl/lvgl_internal.h"

#if CIRCUITPY_DISPLAYIO_LVGL

static void _raise_unsupported(void) {
    mp_raise_NotImplementedError(MP_ERROR_TEXT("LVGL displayio backend does not support flips/transpose yet"));
}

// Map displayio bpp to the LVGL color format with the same pixel data layout.
uint8_t displayio_lvgl_bitmap_bpp(mp_obj_t bitmap) {
    if (mp_obj_is_type(bitmap, &displayio_ondiskbitmap_type)) {
        displayio_ondiskbitmap_t *odb = MP_OBJ_TO_PTR(bitmap);
        return odb->bits_per_pixel;
    }
    displayio_bitmap_t *bmp = MP_OBJ_TO_PTR(bitmap);
    return bmp->bits_per_value;
}

static lv_color_format_t _bitmap_cf(uint8_t bpp) {
    switch (bpp) {
        case 1:
            return LV_COLOR_FORMAT_I1;
        case 2:
            return LV_COLOR_FORMAT_I2;
        case 4:
            return LV_COLOR_FORMAT_I4;
        case 8:
            return LV_COLOR_FORMAT_I8;
        case 16:
            return LV_COLOR_FORMAT_RGB565;
        case 24:
            return LV_COLOR_FORMAT_RGB888;
        case 32:
            return LV_COLOR_FORMAT_ARGB8888;
        default:
            return LV_COLOR_FORMAT_I8;
    }
}

// Byte stride of one bitmap row.
static uint32_t _bitmap_stride_bytes(mp_obj_t bitmap) {
    if (mp_obj_is_type(bitmap, &displayio_ondiskbitmap_type)) {
        displayio_ondiskbitmap_t *odb = MP_OBJ_TO_PTR(bitmap);
        return odb->stride;
    }
    displayio_bitmap_t *bmp = MP_OBJ_TO_PTR(bitmap);
    return (uint32_t)bmp->stride * sizeof(uint32_t);
}

// Tile pixel origin in the source bitmap for a tile index.
static void _tile_origin(displayio_tilegrid_t *self, uint16_t tile_index, uint16_t *x0, uint16_t *y0) {
    *x0 = (tile_index % self->bitmap_width_in_tiles) * self->tile_width;
    *y0 = (tile_index / self->bitmap_width_in_tiles) * self->tile_height;
}

// Read the tile index stored for a grid cell (handles the inline tiles optimization).
static uint16_t _tile_index(displayio_tilegrid_t *self, uint16_t cell_index) {
    void *tiles = self->tiles;
    if (self->inline_tiles) {
        tiles = &self->tiles;
    }
    if (tiles == NULL) {
        return 0;
    }
    uint32_t index = cell_index;
    if (self->tiles_in_bitmap > 255) {
        return ((uint16_t *)tiles)[index];
    }
    return ((uint8_t *)tiles)[index];
}

// Map a displayed cell (i, j) to the source tile index, applying the scrolling window
// offsets. Mirrors the legacy fill logic: the cell at (i, j) shows tile
// ((i + top_left_x) % W, (j + top_left_y) % H) of the tile matrix.
static uint16_t _cell_tile_index(displayio_tilegrid_t *self, uint16_t i, uint16_t j) {
    uint16_t sx = (i + self->top_left_x) % self->width_in_tiles;
    uint16_t sy = (j + self->top_left_y) % self->height_in_tiles;
    return _tile_index(self, sy * self->width_in_tiles + sx);
}

// Initialize a tile descriptor for the tile shown at cell (col, row).
static void _init_cell_tile_dsc(displayio_tilegrid_t *self, displayio_lvgl_image_dsc_t *dsc, uint16_t i, uint16_t j);
// Initialize a tile descriptor for the given source tile index.
static void _init_tile_dsc(displayio_tilegrid_t *self, displayio_lvgl_image_dsc_t *dsc, uint16_t tile_index) {
    lv_image_dsc_t *d = &dsc->dsc;
    uint16_t x0, y0;
    _tile_origin(self, tile_index, &x0, &y0);
    uint8_t bpp = displayio_lvgl_bitmap_bpp(self->bitmap);
    uint32_t stride = _bitmap_stride_bytes(self->bitmap);
    const uint8_t *base = NULL;
    if (!mp_obj_is_type(self->bitmap, &displayio_ondiskbitmap_type)) {
        displayio_bitmap_t *bmp = MP_OBJ_TO_PTR(self->bitmap);
        base = (const uint8_t *)bmp->data;
    }

    lv_memzero(d, sizeof(lv_image_dsc_t));
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = _bitmap_cf(bpp);
    d->header.w = self->tile_width;
    d->header.h = self->tile_height;
    d->header.stride = stride;
    d->header.flags = 0;
    if (base != NULL) {
        d->data = base + (uint32_t)y0 * stride;
        if (bpp >= 8) {
            d->data += (size_t)x0 * (bpp / 8);
        } else {
            d->data += (x0 * bpp) / 8;
        }
    } else {
        // File-backed sources are read by the decoder; the pointer is never dereferenced.
        d->data = (const uint8_t *)dsc;
    }
    d->data_size = stride * d->header.h;
    d->reserved_2 = dsc; // caniary used by the decoder
    dsc->bitmap = self->bitmap;
    dsc->pixel_shader = self->pixel_shader;
    dsc->src_x0 = x0;
    dsc->src_y0 = y0;
    dsc->version++;
    if (dsc->view_buf != NULL) {
        lv_memzero(dsc->view_buf, sizeof(lv_draw_buf_t));
    }
}

static void _init_cell_tile_dsc(displayio_tilegrid_t *self, displayio_lvgl_image_dsc_t *dsc, uint16_t i, uint16_t j) {
    _init_tile_dsc(self, dsc, _cell_tile_index(self, i, j));
}

// Re-source one cell's image and drop any cached decode of it. Caller holds the lock.
static void _update_tile_image(displayio_tilegrid_t *self, uint16_t cell_index) {
    if (self->tile_dscs == NULL) {
        return;
    }
    displayio_lvgl_image_dsc_t *dsc = &self->tile_dscs[cell_index];
    _init_cell_tile_dsc(self, dsc, cell_index % self->width_in_tiles,
        cell_index / self->width_in_tiles);
    displayio_lvgl_dsc_cache_drop(dsc);
    // Sprites use self->lv_obj itself as their single image.
    lv_obj_t *img = self->tile_images != NULL ? self->tile_images[cell_index] : self->lv_obj;
    if (img != NULL) {
        lv_image_set_src(img, &dsc->dsc);
    }
}

// Re-source (and thus invalidate) every cell. Caller holds the lock.
static void _refresh_all_tiles(displayio_tilegrid_t *self) {
    if (self->lv_obj == NULL) {
        return;
    }
    uint32_t total_tiles = (uint32_t)self->width_in_tiles * self->height_in_tiles;
    for (uint32_t cell = 0; cell < total_tiles; cell++) {
        _update_tile_image(self, cell);
    }
}

void displayio_lvgl_tilegrid_attach(displayio_tilegrid_t *self, lv_obj_t *parent) {
    if (!self->in_group || parent == NULL) {
        return;
    }
    displayio_lvgl_lock();
    bool single = self->width_in_tiles == 1 && self->height_in_tiles == 1;
    uint32_t tile_count = (uint32_t)self->width_in_tiles * self->height_in_tiles;

    if (self->tile_dscs == NULL || self->tile_count != tile_count) {
        // The mirror state lives in LVGL's own allocation pool: static TileGrids (the
        // supervisor terminal grids) reference these pointers, but the GC cannot scan
        // static data, so GC-heap allocations here would be swept as unreachable.
        if (self->tile_dscs != NULL) {
            lv_free(self->tile_dscs);
            self->tile_dscs = NULL;
        }
        if (self->tile_images != NULL) {
            lv_free(self->tile_images);
            self->tile_images = NULL;
        }
        self->tile_dscs = lv_malloc(tile_count * sizeof(displayio_lvgl_image_dsc_t));
        self->tile_count = tile_count;
        lv_memset(self->tile_dscs, 0, tile_count * sizeof(displayio_lvgl_image_dsc_t));
        self->tile_images = single ? NULL : lv_malloc(tile_count * sizeof(lv_obj_t *));
    }

    if (self->lv_obj == NULL) {
        if (single) {
            // The sprite is one lv_image directly.
            self->lv_obj = lv_image_create(parent);
            _init_tile_dsc(self, &self->tile_dscs[0], _cell_tile_index(self, 0, 0));
            lv_image_set_src(self->lv_obj, &self->tile_dscs[0].dsc);
        } else {
            self->lv_obj = lv_obj_create(parent);
            lv_obj_set_style_bg_opa(self->lv_obj, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(self->lv_obj, 0, 0);
            lv_obj_set_style_pad_all(self->lv_obj, 0, 0);
            lv_obj_add_flag(self->lv_obj, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
            lv_obj_clear_flag(self->lv_obj, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_clear_flag(self->lv_obj, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_size(self->lv_obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            for (uint16_t ty = 0; ty < self->height_in_tiles; ty++) {
                for (uint16_t tx = 0; tx < self->width_in_tiles; tx++) {
                    uint16_t cell = ty * self->width_in_tiles + tx;
                    _init_cell_tile_dsc(self, &self->tile_dscs[cell], tx, ty);
                    self->tile_images[cell] = lv_image_create(self->lv_obj);
                    lv_obj_set_pos(self->tile_images[cell], tx * self->tile_width, ty * self->tile_height);
                    lv_image_set_src(self->tile_images[cell], &self->tile_dscs[cell].dsc);
                }
            }
        }
        lv_obj_set_pos(self->lv_obj, self->x, self->y);
        displayio_lvgl_palette_register_user(MP_OBJ_TO_PTR(self->pixel_shader), self);
        if (mp_obj_is_type(self->bitmap, &displayio_bitmap_type)) {
            displayio_lvgl_bitmap_register_user(MP_OBJ_TO_PTR(self->bitmap), self);
        }
        displayio_lvgl_unlock();
        return;
    }

    // Re-attach: the bitmap or pixel shader may have changed while detached.
    lv_obj_set_parent(self->lv_obj, parent);
    _refresh_all_tiles(self);
    displayio_lvgl_palette_register_user(MP_OBJ_TO_PTR(self->pixel_shader), self);
    if (mp_obj_is_type(self->bitmap, &displayio_bitmap_type)) {
        displayio_lvgl_bitmap_register_user(MP_OBJ_TO_PTR(self->bitmap), self);
    }
    displayio_lvgl_unlock();
}

void displayio_lvgl_tilegrid_detach(displayio_tilegrid_t *self) {
    if (self->lv_obj == NULL) {
        return;
    }
    displayio_lvgl_lock();
    lv_obj_delete(self->lv_obj);
    self->lv_obj = NULL;
    displayio_lvgl_unlock();

    displayio_lvgl_bitmap_unregister_user(MP_OBJ_TO_PTR(self->bitmap), self);
    if (mp_obj_is_type(self->pixel_shader, &displayio_palette_type)) {
        displayio_lvgl_palette_unregister_user(MP_OBJ_TO_PTR(self->pixel_shader), self);
    }
    if (self->tile_dscs != NULL) {
        for (uint32_t cell = 0; cell < self->tile_count; cell++) {
            if (self->tile_dscs[cell].view_buf != NULL) {
                lv_free(self->tile_dscs[cell].view_buf);
                self->tile_dscs[cell].view_buf = NULL;
            }
            // Drop cached decodes; the freed descriptor memory may be reused and would
            // otherwise collide with the cache key.
            displayio_lvgl_dsc_cache_drop(&self->tile_dscs[cell]);
        }
        lv_free(self->tile_dscs);
        self->tile_dscs = NULL;
    }
    if (self->tile_images != NULL) {
        lv_free(self->tile_images);
        self->tile_images = NULL;
    }
    self->tile_count = 0;
}

// Apply the inherited scale chain to the tile images. TileGrids have no scale of their
// own; they render at the scale of the parent group chain.
void displayio_lvgl_tilegrid_update_scale(displayio_tilegrid_t *self) {
    if (self->lv_obj == NULL) {
        return;
    }
    displayio_lvgl_lock();
    if (self->tile_images != NULL) {
        for (uint32_t cell = 0; cell < self->tile_count; cell++) {
            if (self->tile_images[cell] != NULL) {
                lv_image_set_scale(self->tile_images[cell], (int32_t)self->lv_scale * LV_SCALE_NONE);
            }
        }
    } else {
        lv_image_set_scale(self->lv_obj, (int32_t)self->lv_scale * LV_SCALE_NONE);
    }
    displayio_lvgl_unlock();
}

void common_hal_displayio_tilegrid_construct(displayio_tilegrid_t *self, mp_obj_t bitmap,
    uint16_t bitmap_width_in_tiles, uint16_t bitmap_height_in_tiles,
    mp_obj_t pixel_shader, uint16_t width, uint16_t height,
    uint16_t tile_width, uint16_t tile_height, uint16_t x, uint16_t y, uint16_t default_tile) {

    uint32_t total_tiles = width * height;
    self->bitmap_width_in_tiles = bitmap_width_in_tiles;
    self->tiles_in_bitmap = bitmap_width_in_tiles * bitmap_height_in_tiles;

    // Determine if we need uint16_t or uint8_t for tile indices
    bool use_uint16 = self->tiles_in_bitmap > 255;

    // Sprites will only have one tile so save a little memory by inlining values in the pointer.
    uint8_t inline_tiles = sizeof(void *) / (use_uint16 ? sizeof(uint16_t) : sizeof(uint8_t));

    if (total_tiles <= inline_tiles) {
        self->tiles = 0;
        // Pack values into the pointer since there are only a few.
        if (use_uint16) {
            for (uint32_t i = 0; i < inline_tiles && i < total_tiles; i++) {
                ((uint16_t *)&self->tiles)[i] = default_tile;
            }
        } else {
            for (uint32_t i = 0; i < inline_tiles && i < total_tiles; i++) {
                ((uint8_t *)&self->tiles)[i] = (uint8_t)default_tile;
            }
        }
        self->inline_tiles = true;
    } else {
        if (use_uint16) {
            uint16_t *tiles16 = (uint16_t *)m_malloc_without_collect(total_tiles * sizeof(uint16_t));
            for (uint32_t i = 0; i < total_tiles; i++) {
                tiles16[i] = default_tile;
            }
            self->tiles = tiles16;
        } else {
            uint8_t *tiles8 = (uint8_t *)m_malloc_without_collect(total_tiles);
            for (uint32_t i = 0; i < total_tiles; i++) {
                tiles8[i] = (uint8_t)default_tile;
            }
            self->tiles = tiles8;
        }
        self->inline_tiles = false;
    }

    self->width_in_tiles = width;
    self->height_in_tiles = height;
    self->x = x;
    self->y = y;
    self->pixel_width = width * tile_width;
    self->pixel_height = height * tile_height;
    self->tile_width = tile_width;
    self->tile_height = tile_height;
    self->bitmap = bitmap;
    self->pixel_shader = pixel_shader;
    self->in_group = false;
    self->hidden = false;
    self->hidden_by_parent = false;
    self->flip_x = false;
    self->flip_y = false;
    self->transpose_xy = false;
    self->lv_obj = NULL;
    self->tile_images = NULL;
    self->tile_dscs = NULL;
    self->tile_count = 0;
    self->lv_scale = 1;
    #if CIRCUITPY_TILEPALETTEMAPPER
    if (mp_obj_is_type(self->pixel_shader, &tilepalettemapper_tilepalettemapper_type)) {
        tilepalettemapper_tilepalettemapper_bind(self->pixel_shader, self);
    }
    #endif
}

bool common_hal_displayio_tilegrid_get_hidden(displayio_tilegrid_t *self) {
    return self->hidden;
}

bool displayio_tilegrid_get_rendered_hidden(displayio_tilegrid_t *self) {
    (void)self;
    return false;
}

void displayio_tilegrid_set_hidden_by_parent(displayio_tilegrid_t *self, bool hidden) {
    self->hidden_by_parent = hidden;
    if (self->lv_obj != NULL) {
        displayio_lvgl_lock();
        if (self->hidden || self->hidden_by_parent) {
            lv_obj_add_flag(self->lv_obj, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(self->lv_obj, LV_OBJ_FLAG_HIDDEN);
        }
        displayio_lvgl_unlock();
    }
}

void common_hal_displayio_tilegrid_set_hidden(displayio_tilegrid_t *self, bool hidden) {
    self->hidden = hidden;
    displayio_tilegrid_set_hidden_by_parent(self, self->hidden_by_parent);
}

// Update the tile image mirror for the cell that shows tile (x, y) of this grid.
void displayio_tilegrid_mark_tile_dirty(displayio_tilegrid_t *self, uint16_t x, uint16_t y) {
    if (!self->in_group || self->lv_obj == NULL) {
        // Not mirrored yet; attaching re-reads all tiles.
        return;
    }
    displayio_lvgl_lock();
    int16_t tx = (x - self->top_left_x) % self->width_in_tiles;
    if (tx < 0) {
        tx += self->width_in_tiles;
    }
    int16_t ty = (y - self->top_left_y) % self->height_in_tiles;
    if (ty < 0) {
        ty += self->height_in_tiles;
    }
    uint16_t cell = ty * self->width_in_tiles + tx;
    _update_tile_image(self, cell);
    displayio_lvgl_unlock();
}

mp_int_t common_hal_displayio_tilegrid_get_x(displayio_tilegrid_t *self) {
    return self->x;
}
void common_hal_displayio_tilegrid_set_x(displayio_tilegrid_t *self, mp_int_t x) {
    if (self->x == x) {
        return;
    }
    self->x = x;
    if (self->lv_obj != NULL) {
        displayio_lvgl_lock();
        lv_obj_set_pos(self->lv_obj, self->x, self->y);
        displayio_lvgl_unlock();
    }
}
mp_int_t common_hal_displayio_tilegrid_get_y(displayio_tilegrid_t *self) {
    return self->y;
}

void common_hal_displayio_tilegrid_set_y(displayio_tilegrid_t *self, mp_int_t y) {
    if (self->y == y) {
        return;
    }
    self->y = y;
    if (self->lv_obj != NULL) {
        displayio_lvgl_lock();
        lv_obj_set_pos(self->lv_obj, self->x, self->y);
        displayio_lvgl_unlock();
    }
}

mp_obj_t common_hal_displayio_tilegrid_get_pixel_shader(displayio_tilegrid_t *self) {
    return self->pixel_shader;
}

void common_hal_displayio_tilegrid_set_pixel_shader(displayio_tilegrid_t *self, mp_obj_t pixel_shader) {
    mp_obj_t old = self->pixel_shader;
    self->pixel_shader = pixel_shader;
    if (self->lv_obj != NULL) {
        displayio_lvgl_lock();
        _refresh_all_tiles(self);
        displayio_lvgl_unlock();
        if (mp_obj_is_type(old, &displayio_palette_type)) {
            displayio_lvgl_palette_unregister_user(MP_OBJ_TO_PTR(old), self);
        }
        if (mp_obj_is_type(pixel_shader, &displayio_palette_type)) {
            displayio_lvgl_palette_register_user(MP_OBJ_TO_PTR(pixel_shader), self);
        }
    }
    #if CIRCUITPY_TILEPALETTEMAPPER
    if (mp_obj_is_type(self->pixel_shader, &tilepalettemapper_tilepalettemapper_type)) {
        tilepalettemapper_tilepalettemapper_bind(self->pixel_shader, self);
    }
    #endif
}

mp_obj_t common_hal_displayio_tilegrid_get_bitmap(displayio_tilegrid_t *self) {
    return self->bitmap;
}

void common_hal_displayio_tilegrid_set_bitmap(displayio_tilegrid_t *self, mp_obj_t bitmap) {
    mp_obj_t old = self->bitmap;
    self->bitmap = bitmap;
    if (self->lv_obj != NULL) {
        if (mp_obj_is_type(old, &displayio_bitmap_type)) {
            displayio_lvgl_bitmap_unregister_user(MP_OBJ_TO_PTR(old), self);
        }
        displayio_lvgl_lock();
        _refresh_all_tiles(self);
        displayio_lvgl_unlock();
        if (mp_obj_is_type(bitmap, &displayio_bitmap_type)) {
            displayio_lvgl_bitmap_register_user(MP_OBJ_TO_PTR(bitmap), self);
        }
    }
}

uint16_t common_hal_displayio_tilegrid_get_width(displayio_tilegrid_t *self) {
    return self->width_in_tiles;
}

uint16_t common_hal_displayio_tilegrid_get_height(displayio_tilegrid_t *self) {
    return self->height_in_tiles;
}

uint16_t common_hal_displayio_tilegrid_get_tile_width(displayio_tilegrid_t *self) {
    return self->tile_width;
}

uint16_t common_hal_displayio_tilegrid_get_tile_height(displayio_tilegrid_t *self) {
    return self->tile_height;
}

uint16_t common_hal_displayio_tilegrid_get_tile(displayio_tilegrid_t *self, uint16_t x, uint16_t y) {
    void *tiles = self->tiles;
    if (self->inline_tiles) {
        tiles = &self->tiles;
    }
    if (tiles == NULL) {
        return 0;
    }

    uint32_t index = y * self->width_in_tiles + x;
    if (self->tiles_in_bitmap > 255) {
        return ((uint16_t *)tiles)[index];
    }
    return ((uint8_t *)tiles)[index];
}

void common_hal_displayio_tilegrid_set_tile(displayio_tilegrid_t *self, uint16_t x, uint16_t y, uint16_t tile_index) {
    if (tile_index >= self->tiles_in_bitmap) {
        mp_raise_ValueError(MP_ERROR_TEXT("Tile index out of bounds"));
    }

    void *tiles = self->tiles;
    if (self->inline_tiles) {
        tiles = &self->tiles;
    }
    if (tiles == NULL) {
        return;
    }

    uint32_t index = y * self->width_in_tiles + x;
    if (self->tiles_in_bitmap > 255) {
        ((uint16_t *)tiles)[index] = tile_index;
    } else {
        ((uint8_t *)tiles)[index] = (uint8_t)tile_index;
    }
    displayio_tilegrid_mark_tile_dirty(self, x, y);
}

void common_hal_displayio_tilegrid_set_all_tiles(displayio_tilegrid_t *self, uint16_t tile_index) {
    if (tile_index >= self->tiles_in_bitmap) {
        mp_raise_ValueError(MP_ERROR_TEXT("Tile index out of bounds"));
    }

    void *tiles = self->tiles;
    if (self->inline_tiles) {
        tiles = &self->tiles;
    }
    if (tiles == NULL) {
        return;
    }

    if (self->tiles_in_bitmap > 255) {
        uint16_t *tiles16 = (uint16_t *)tiles;
        for (uint16_t y = 0; y < self->height_in_tiles; y++) {
            for (uint16_t x = 0; x < self->width_in_tiles; x++) {
                tiles16[y * self->width_in_tiles + x] = tile_index;
            }
        }
    } else {
        uint8_t *tiles8 = (uint8_t *)tiles;
        for (uint16_t y = 0; y < self->height_in_tiles; y++) {
            for (uint16_t x = 0; x < self->width_in_tiles; x++) {
                tiles8[y * self->width_in_tiles + x] = (uint8_t)tile_index;
            }
        }
    }

    if (self->lv_obj != NULL) {
        displayio_lvgl_lock();
        _refresh_all_tiles(self);
        displayio_lvgl_unlock();
    }
}

bool common_hal_displayio_tilegrid_get_flip_x(displayio_tilegrid_t *self) {
    return self->flip_x;
}

void common_hal_displayio_tilegrid_set_flip_x(displayio_tilegrid_t *self, bool flip_x) {
    if (self->flip_x == flip_x) {
        return;
    }
    if (flip_x) {
        _raise_unsupported();
    }
    self->flip_x = flip_x;
    if (self->lv_obj != NULL) {
        displayio_lvgl_lock();
        _refresh_all_tiles(self);
        displayio_lvgl_unlock();
    }
}

bool common_hal_displayio_tilegrid_get_flip_y(displayio_tilegrid_t *self) {
    return self->flip_y;
}

void common_hal_displayio_tilegrid_set_flip_y(displayio_tilegrid_t *self, bool flip_y) {
    if (self->flip_y == flip_y) {
        return;
    }
    if (flip_y) {
        _raise_unsupported();
    }
    self->flip_y = flip_y;
    if (self->lv_obj != NULL) {
        displayio_lvgl_lock();
        _refresh_all_tiles(self);
        displayio_lvgl_unlock();
    }
}

bool common_hal_displayio_tilegrid_get_transpose_xy(displayio_tilegrid_t *self) {
    return self->transpose_xy;
}

void common_hal_displayio_tilegrid_set_transpose_xy(displayio_tilegrid_t *self, bool transpose_xy) {
    if (self->transpose_xy == transpose_xy) {
        return;
    }
    if (transpose_xy) {
        _raise_unsupported();
    }
    self->transpose_xy = transpose_xy;
    if (self->lv_obj != NULL) {
        displayio_lvgl_lock();
        _refresh_all_tiles(self);
        displayio_lvgl_unlock();
    }
}

bool common_hal_displayio_tilegrid_contains(displayio_tilegrid_t *self, uint16_t x, uint16_t y) {
    uint16_t right_edge = self->x + (self->width_in_tiles * self->tile_width);
    uint16_t bottom_edge = self->y + (self->height_in_tiles * self->tile_height);
    return x >= self->x && x < right_edge &&
           y >= self->y && y < bottom_edge;
}

void common_hal_displayio_tilegrid_set_top_left(displayio_tilegrid_t *self, uint16_t x, uint16_t y) {
    self->top_left_x = x;
    self->top_left_y = y;
    if (self->lv_obj != NULL) {
        displayio_lvgl_lock();
        _refresh_all_tiles(self);
        displayio_lvgl_unlock();
    }
}

// Bitmap and Palette push notifications: the affected pixels of a shared source changed.
void displayio_lvgl_tilegrid_bitmap_changed(displayio_tilegrid_t *self,
    int16_t x1, int16_t y1, int16_t x2, int16_t y2) {
    (void)x1;
    (void)y1;
    (void)x2;
    (void)y2;
    if (self->lv_obj == NULL) {
        return;
    }
    lv_obj_invalidate(self->lv_obj);
}

void displayio_lvgl_tilegrid_palette_changed(displayio_tilegrid_t *self,
    int16_t x1, int16_t y1, int16_t x2, int16_t y2) {
    (void)x1;
    (void)y1;
    (void)x2;
    (void)y2;
    if (self->lv_obj == NULL) {
        return;
    }
    lv_obj_invalidate(self->lv_obj);
}

#endif  // CIRCUITPY_DISPLAYIO_LVGL
