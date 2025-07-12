// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2024 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

#include "shared-module/lvfontio/OnDiskFont.h"

extern const mp_obj_type_t lvfontio_ondiskfont_type;

mp_obj_t common_hal_lvfontio_ondiskfont_get_bitmap(const lvfontio_ondiskfont_t *self);
mp_obj_t common_hal_lvfontio_ondiskfont_get_bounding_box(const lvfontio_ondiskfont_t *self);
void common_hal_lvfontio_ondiskfont_get_dimensions(const lvfontio_ondiskfont_t *self, uint16_t *width, uint16_t *height);

// Function prototypes
void common_hal_lvfontio_ondiskfont_construct(lvfontio_ondiskfont_t *self, const char *file_path, uint16_t max_glyphs, bool use_gc_allocator);
void common_hal_lvfontio_ondiskfont_deinit(lvfontio_ondiskfont_t *self);
bool common_hal_lvfontio_ondiskfont_deinited(lvfontio_ondiskfont_t *self);
int16_t common_hal_lvfontio_ondiskfont_cache_glyph(lvfontio_ondiskfont_t *self, uint32_t codepoint, bool *is_full_width);
void common_hal_lvfontio_ondiskfont_release_glyph(lvfontio_ondiskfont_t *self, uint32_t slot);
