// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2023 Jeff Epler for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"
#include "lib/tjpgd/src/tjpgd.h"
#include "shared-module/displayio/Bitmap.h"

#define TJPGD_WORKSPACE_SIZE 3500

typedef struct jpegio_jpegdecoder_obj {
    mp_obj_base_t base;
    JDEC decoder;
    byte workspace[TJPGD_WORKSPACE_SIZE];
    mp_obj_t data_obj;
    mp_buffer_info_t bufinfo;
    displayio_bitmap_t *dest;
    uint16_t x, y;
    bitmaptools_rect_t lim;
    uint32_t skip_source_index, skip_dest_index;
    bool skip_source_index_none, skip_dest_index_none;
    uint8_t scale;
} jpegio_jpegdecoder_obj_t;

#if CIRCUITPY_JPEGIO_HW
// Ports with JPEG decode hardware provide this. It is called by decode() only when the
// source passed to open() is an in-memory buffer; `jpeg` is that whole buffer. The decode
// arguments (x, y, lim, skip indices) are already stored in `self`, and `self->dest` is
// the target bitmap.
// Returns true if the port decoded the image into `self->dest`. Returns false to decode
// with TJpgDec instead, which rewrites every pixel the image covers. Must not raise.
bool jpegio_jpegdecoder_decode_hw(jpegio_jpegdecoder_obj_t *self, const mp_buffer_info_t *jpeg, int scale);
#endif
