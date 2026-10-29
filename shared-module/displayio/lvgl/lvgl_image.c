// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Custom LVGL image decoder for displayio tile sources. Pixels pass through the same
// CircuitPython pixel shader functions the legacy compositor uses (guaranteeing bit-exact
// output) and are packed for LVGL's copy of the display driver's buffer format. When a
// tile's source color format already matches the display format and no shader is applied,
// the decoded buffer wraps the source bitmap's shared pixel data directly (zero copy).

#include <stdio.h>
#include "shared-module/displayio/lvgl/lvgl_internal.h"

#if CIRCUITPY_DISPLAYIO_LVGL

#include <string.h>

#include "py/runtime.h"
#include "py/stream.h"
#include "misc/cache/instance/lv_image_cache.h"
#include "misc/cache/instance/lv_image_header_cache.h"
#include "shared-bindings/displayio/Bitmap.h"
#include "shared-bindings/displayio/ColorConverter.h"
#include "shared-bindings/displayio/OnDiskBitmap.h"
#include "shared-bindings/displayio/Palette.h"
#include "shared-module/displayio/ColorConverter.h"

// The decoder descriptor struct is private in LVGL 9.6+, and custom decoders are expected
// to fill it in. Pull in the private definitions for the fields we set.
#include "image/lv_image_decoder_private.h"
#include "core/lv_global.h"

static _displayio_colorspace_t cp_lvgl_colorspace;
static bool cp_lvgl_has_colorspace = false;
static lv_image_decoder_t *cp_lvgl_decoder = NULL;

void displayio_lvgl_set_colorspace(const _displayio_colorspace_t *colorspace) {
    cp_lvgl_colorspace = *colorspace;
    cp_lvgl_has_colorspace = true;
}

static lv_color_format_t cp_lvgl_display_cf(void) {
    lv_display_t *display = lv_display_get_default();
    if (display == NULL) {
        return LV_COLOR_FORMAT_UNKNOWN;
    }
    return lv_display_get_color_format(display);
}

// Pack a display-converted pixel into an LVGL buffer at out.
//
// `packed` holds what the legacy compositor would write into its transfer buffer for the
// active colorspace: an RGB565 u16 for depth 16, 0x00RRGGBB for depths 24/32, and one byte
// (or bit) for smaller depths. When the decode target's format differs from the
// colorspace's (e.g. a transparent palette forces the ARGB8888 decode target on an RGB565
// display), expand to RGB888 exactly like the SDL driver does so bytes stay identical to
// the legacy output.
static void cp_lvgl_pack_pixel(uint32_t packed, uint8_t depth, bool reverse_bytes,
    lv_color_format_t cf, uint8_t *out) {
    (void)reverse_bytes;
    (void)depth;

    // Resolve the packed value to 8-bit channels.
    uint8_t r8, g8, b8;
    switch (cp_lvgl_colorspace.depth) {
        case 16: {
            uint16_t v = (uint16_t)packed;
            r8 = ((v >> 11) & 0x1f) * 255 / 31;
            g8 = ((v >> 5) & 0x3f) * 255 / 63;
            b8 = (v & 0x1f) * 255 / 31;
            break;
        }
        case 8:
            switch (cf) {
                case LV_COLOR_FORMAT_L8:
                    out[0] = packed & 0xff;
                    return;
                case LV_COLOR_FORMAT_AL88:
                    out[0] = packed & 0xff;
                    out[1] = 0xff;
                    return;
                case LV_COLOR_FORMAT_I1:
                    // Bit packing happens in the caller.
                    out[0] = packed & 0x1;
                    return;
                default: {
                    uint8_t gray = packed & 0xff;
                    r8 = g8 = b8 = gray;
                    break;
                }
            }
            break;
        default:
            if (cp_lvgl_colorspace.depth < 8) {
                uint8_t bit = packed & 0x1;
                r8 = g8 = b8 = bit ? 255 : 0;
            } else {
                r8 = (packed >> 16) & 0xff;
                g8 = (packed >> 8) & 0xff;
                b8 = packed & 0xff;
            }
            break;
    }

    switch (cf) {
        case LV_COLOR_FORMAT_RGB565:
        case LV_COLOR_FORMAT_RGB565_SWAPPED: {
            uint16_t rgb565 = (uint16_t)(((r8 >> 3) << 11) | ((g8 >> 2) << 5) | (b8 >> 3));
            if (cf == LV_COLOR_FORMAT_RGB565_SWAPPED) {
                rgb565 = (uint16_t)((rgb565 << 8) | (rgb565 >> 8));
            }
            memcpy(out, &rgb565, sizeof(uint16_t));
            break;
        }
        case LV_COLOR_FORMAT_RGB888:
            out[0] = b8;
            out[1] = g8;
            out[2] = r8;
            break;
        case LV_COLOR_FORMAT_ARGB8888:
        case LV_COLOR_FORMAT_XRGB8888:
            out[0] = b8;
            out[1] = g8;
            out[2] = r8;
            out[3] = 0xff;
            break;
        default: {
            uint8_t size = lv_color_format_get_size(cf);
            memset(out, 0, size);
            break;
        }
    }
}

static void cp_lvgl_pack_transparent(lv_color_format_t cf, uint8_t *out) {
    memset(out, 0, lv_color_format_get_size(cf));
}

// Identify our descriptors: the reserved_2 caniary points back at the struct itself.
static bool cp_lvgl_is_ours(const void *src) {
    const lv_image_dsc_t *dsc = src;
    return dsc->header.magic == LV_IMAGE_HEADER_MAGIC && dsc->reserved_2 == dsc;
}

static lv_result_t cp_lvgl_decoder_info(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc,
    lv_image_header_t *header) {
    (void)decoder;
    (void)dsc;
    const lv_image_dsc_t *src = dsc->src;
    if (!cp_lvgl_is_ours(src)) {
        return LV_RESULT_INVALID;
    }
    // The widget sizes itself from the source slice geometry.
    header->w = src->header.w;
    header->h = src->header.h;
    header->cf = cp_lvgl_display_cf();
    header->stride = 0; // Recomputed from the target below.
    return LV_RESULT_OK;
}

// The decoded buffer for the zero-copy view case lives inside the descriptor so it
// survives as long as the TileGrid, and cache eviction never frees shared data (no
// ALLOCATED flag).
static void cp_lvgl_fill_view_buf(displayio_lvgl_image_dsc_t *ext) {
    if (ext->view_buf == NULL) {
        // Allocated on demand; cached decode entries reference it and, because it never
        // carries the ALLOCATED flag, eviction frees nothing (small struct leak).
        ext->view_buf = lv_malloc(sizeof(lv_draw_buf_t));
        if (ext->view_buf == NULL) {
            return;
        }
    }
    lv_draw_buf_t *buf = ext->view_buf;
    lv_memzero(buf, sizeof(lv_draw_buf_t));
    lv_image_header_t *header = &buf->header;
    const lv_image_dsc_t *dsc = &ext->dsc;
    header->w = dsc->header.w;
    header->h = dsc->header.h;
    header->cf = dsc->header.cf;
    header->stride = dsc->header.stride;
    header->magic = LV_IMAGE_HEADER_MAGIC;
    buf->data = (void *)dsc->data;
    buf->unaligned_data = buf->data;
    buf->data_size = (uint32_t)dsc->header.stride * dsc->header.h;
    buf->handlers = lv_draw_buf_get_handlers();
}

static lv_result_t cp_lvgl_decoder_open(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc) {
    (void)decoder;
    displayio_lvgl_image_dsc_t *ext = (displayio_lvgl_image_dsc_t *)dsc->src;
    if (!cp_lvgl_is_ours(ext)) {
        return LV_RESULT_INVALID;
    }
    if (!cp_lvgl_has_colorspace) {
        return LV_RESULT_INVALID;
    }
    const lv_image_dsc_t *src = &ext->dsc;
    lv_color_format_t display_cf = cp_lvgl_display_cf();
    if (display_cf == LV_COLOR_FORMAT_UNKNOWN) {
        return LV_RESULT_INVALID;
    }
    uint16_t w = src->header.w;
    uint16_t h = src->header.h;

    // Zero-copy view: in-RAM source, no shader, matching formats. The bitmap data is
    // shared directly with LVGL. Sub-byte tiles must fall on a byte boundary.
    uint8_t bpp = displayio_lvgl_bitmap_bpp(ext->bitmap);
    bool byte_aligned = bpp >= 8 || ((uint32_t)ext->src_x0 * bpp) % 8 == 0;
    if (!mp_obj_is_type(ext->bitmap, &displayio_ondiskbitmap_type) && byte_aligned &&
        ext->pixel_shader == mp_const_none && src->header.cf == display_cf &&
        display_cf != LV_COLOR_FORMAT_I1) {
        cp_lvgl_fill_view_buf(ext);
        if (ext->view_buf != NULL) {
            dsc->decoded = ext->view_buf;
            dsc->header = src->header;
            return LV_RESULT_OK;
        }
        // On allocation failure fall through to the decode path.
    }

    // Does the shader ever produce transparency?
    bool has_transparency = false;
    if (mp_obj_is_type(ext->pixel_shader, &displayio_palette_type)) {
        displayio_palette_t *palette = MP_OBJ_TO_PTR(ext->pixel_shader);
        for (uint32_t i = 0; i < palette->color_count; i++) {
            if (palette->colors[i].transparent) {
                has_transparency = true;
                break;
            }
        }
    }

    // Decode target: the display's format, unless transparency needs an alpha-carrying
    // format the display itself doesn't have.
    lv_color_format_t target = display_cf;
    if (display_cf != LV_COLOR_FORMAT_ARGB8888 && has_transparency) {
        target = LV_COLOR_FORMAT_ARGB8888;
    }

    lv_draw_buf_t *decoded = lv_draw_buf_create_ex(
        &(LV_GLOBAL_DEFAULT()->image_cache_draw_buf_handlers), w, h, target, 0);
    if (decoded == NULL) {
        return LV_RESULT_INVALID;
    }

    uint8_t *out = decoded->data;
    uint32_t out_stride = decoded->header.stride;
    uint8_t target_size = lv_color_format_get_size(target);
    uint8_t depth = cp_lvgl_colorspace.depth;

    bool is_odb = mp_obj_is_type(ext->bitmap, &displayio_ondiskbitmap_type);

    // File-backed sources read the whole pixel data once (a handful of stream calls
    // instead of one per pixel) and unpack it exactly like the legacy OnDiskBitmap.
    uint8_t *odb_data = NULL;
    if (is_odb) {
        const displayio_ondiskbitmap_t *odb = MP_OBJ_TO_PTR(ext->bitmap);
        mp_uint_t data_size = (mp_uint_t)odb->stride * odb->height;
        int errcode = 0;
        if (data_size <= 8u * 1024u * 1024u &&
            mp_stream_seek(odb->file, odb->data_offset, MP_SEEK_SET, &errcode) >= 0) {
            odb_data = lv_malloc(data_size);
            if (odb_data != NULL) {
                mp_int_t read = mp_stream_rw(odb->file, odb_data, data_size, &errcode,
                    MP_STREAM_RW_READ | MP_STREAM_RW_ONCE);
                if (read < 0 || (mp_uint_t)read != data_size) {
                    lv_free(odb_data);
                    odb_data = NULL;
                }
            }
        }

    }

    for (uint16_t y = 0; y < h; y++) {
        uint8_t *out_row = out + (uint32_t)y * out_stride;
        if (target == LV_COLOR_FORMAT_I1) {
            memset(out_row, 0, out_stride);
        }
        // For partially-cached ODB rows that couldn't be read whole.
        uint8_t *odb_row = NULL;
        if (is_odb && odb_data == NULL) {
            const displayio_ondiskbitmap_t *odb = MP_OBJ_TO_PTR(ext->bitmap);
            static uint8_t odb_row_fallback[2048];
            int errcode = 0;
            mp_uint_t off = odb->data_offset + (mp_uint_t)(odb->height - y - 1) * odb->stride;
            if (mp_stream_seek(odb->file, off, MP_SEEK_SET, &errcode) >= 0) {
                mp_uint_t len = odb->stride;
                if (len > sizeof(odb_row_fallback)) {
                    len = sizeof(odb_row_fallback);
                }
                mp_int_t read = mp_stream_rw(odb->file, odb_row_fallback, len, &errcode,
                    MP_STREAM_RW_READ | MP_STREAM_RW_ONCE);
                if (read >= 0 && (mp_uint_t)read == len) {
                    odb_row = odb_row_fallback;
                }
            }
        }

        for (uint16_t x = 0; x < w; x++) {
            uint32_t index = 0;
            if (is_odb) {
                const displayio_ondiskbitmap_t *odb = MP_OBJ_TO_PTR(ext->bitmap);
                uint16_t bx = ext->src_x0 + x;
                uint16_t by = ext->src_y0 + y;
                if (bx >= odb->width || by >= odb->height) {
                    index = 0;
                } else {
                    const uint8_t *row = odb_data != NULL
                        ? odb_data + (mp_uint_t)(odb->height - by - 1) * odb->stride
                        : odb_row;
                    if (row == NULL) {
                        index = 0;
                    } else {
                        uint8_t bits = odb->bits_per_pixel;
                        if (bits < 8) {
                            uint8_t pixels_per_byte = 8 / bits;
                            uint8_t value = row[bx / pixels_per_byte];
                            uint8_t pos = pixels_per_byte - (bx % pixels_per_byte) - 1;
                            index = (value >> (pos * bits)) & ((1u << bits) - 1);
                        } else if (bits == 8) {
                            index = row[bx];
                        } else if (bits == 16) {
                            uint16_t v;
                            memcpy(&v, row + (size_t)bx * 2, 2);
                            if (odb->g_bitmask == 0x07e0) {
                                // 565
                                uint32_t r = (v & odb->r_bitmask) >> 11;
                                uint32_t g = (v & odb->g_bitmask) >> 5;
                                uint32_t b = v & odb->b_bitmask;
                                index = (r << 19) | (g << 10) | (b << 3);
                            } else {
                                // 555
                                uint32_t r = (v & odb->r_bitmask) >> 10;
                                uint32_t g = (v & odb->g_bitmask) >> 4;
                                uint32_t b = v & odb->b_bitmask;
                                index = (r << 19) | (g << 10) | (b << 3);
                            }
                        } else if (bits == 24) {
                            // BMP rows store BGR.
                            index = ((uint32_t)row[bx * 3 + 2] << 16) | ((uint32_t)row[bx * 3 + 1] << 8) |
                                row[bx * 3];
                        } else if (bits == 32 && odb->bitfield_compressed) {
                            uint32_t v;
                            memcpy(&v, row + (size_t)bx * 4, 4);
                            index = v & 0x00ffffff;
                        } else {
                            memcpy(&index, row + (size_t)bx * 4, 4);
                        }
                    }
                }
            } else {
                index = common_hal_displayio_bitmap_get_pixel(ext->bitmap,
                    ext->src_x0 + x, ext->src_y0 + y);
            }
            displayio_input_pixel_t input_pixel = {
                .pixel = index,
                .x = x,
                .y = y,
                .tile = 0,
                .tile_x = x,
                .tile_y = y,
            };
            displayio_output_pixel_t output_pixel = { .pixel = 0, .opaque = true };
            if (ext->pixel_shader == mp_const_none) {
                // Legacy passthrough: the raw value is the output pixel.
                output_pixel.pixel = index;
            } else if (mp_obj_is_type(ext->pixel_shader, &displayio_palette_type)) {
                displayio_palette_get_color(ext->pixel_shader, &cp_lvgl_colorspace,
                    &input_pixel, &output_pixel);
            } else if (mp_obj_is_type(ext->pixel_shader, &displayio_colorconverter_type)) {
                displayio_colorconverter_convert(ext->pixel_shader, &cp_lvgl_colorspace,
                    &input_pixel, &output_pixel);
            }

            if (!output_pixel.opaque) {
                // Legacy leaves uncovered pixels to the layers below; here the screen
                // background (black) shows through.
                if (target != LV_COLOR_FORMAT_I1) {
                    cp_lvgl_pack_transparent(target, out_row + (size_t)x * target_size);
                }
                continue;
            }

            if (target == LV_COLOR_FORMAT_I1) {
                // Pack one bit per pixel horizontally within the row, MSB first.
                uint8_t bit = output_pixel.pixel & 0x1;
                uint32_t shift = 7 - (x % 8);
                out_row[x / 8] |= bit << shift;
                continue;
            }

            uint8_t *out_px = out_row + (size_t)x * target_size;
            cp_lvgl_pack_pixel(output_pixel.pixel, depth, cp_lvgl_colorspace.reverse_bytes_in_word,
                target, out_px);
        }
    }

    if (odb_data != NULL) {
        lv_free(odb_data);
    }
    if (w == 16) {
        uint32_t px01 = common_hal_displayio_bitmap_get_pixel(ext->bitmap, 8, 8);
        (void)px01;
        displayio_input_pixel_t dbg_in = { .pixel = px01 };
        displayio_output_pixel_t dbg_out = { .opaque = true };
        if (mp_obj_is_type(ext->pixel_shader, &displayio_colorconverter_type)) {
            displayio_colorconverter_convert(ext->pixel_shader, &cp_lvgl_colorspace, &dbg_in, &dbg_out);
        } else if (mp_obj_is_type(ext->pixel_shader, &displayio_palette_type)) {
            displayio_palette_get_color(ext->pixel_shader, &cp_lvgl_colorspace, &dbg_in, &dbg_out);
        }
        printf("[DBG] gif dec in=%04x out=%06x; row1:", (unsigned)px01, (unsigned)dbg_out.pixel);
        for (int gx = 0; gx < 16; gx++) {
            uint32_t gv = common_hal_displayio_bitmap_get_pixel(ext->bitmap, gx, 1);
            displayio_input_pixel_t di2 = { .pixel = gv };
            displayio_output_pixel_t do2 = { .pixel = 0xdeadbeef, .opaque = true };
            if (mp_obj_is_type(ext->pixel_shader, &displayio_colorconverter_type)) {
                displayio_colorconverter_convert(ext->pixel_shader, &cp_lvgl_colorspace, &di2, &do2);
            } else if (mp_obj_is_type(ext->pixel_shader, &displayio_palette_type)) {
                displayio_palette_get_color(ext->pixel_shader, &cp_lvgl_colorspace, &di2, &do2);
            }
            printf(" %04x>%04x", (unsigned)gv, (unsigned)do2.pixel);
        }
        printf("\n");
    }
    // Our decoded buffers are already in premultiplied form (opaque pixels carry a = 255,
    // transparent ones are zeroed), so mark them as such. That lets LVGL skip its own
    // lossy premultiply pass when routing the image through transform layers (group
    // scaling), which would otherwise corrupt channel values by one bit.
    decoded->header.flags |= LV_IMAGE_FLAGS_PREMULTIPLIED;

    dsc->decoded = decoded;
    dsc->header = decoded->header;
    return LV_RESULT_OK;
}

void displayio_lvgl_register_image_decoder(void) {
    if (cp_lvgl_decoder != NULL) {
        return;
    }
    cp_lvgl_decoder = lv_image_decoder_create();
    lv_image_decoder_set_info_cb(cp_lvgl_decoder, cp_lvgl_decoder_info);
    lv_image_decoder_set_open_cb(cp_lvgl_decoder, cp_lvgl_decoder_open);
    // lv_image_decoder_create inserts at the head of the decoder list, so this runs
    // before the built-in decoders.
}

// Drop cached decodes for a descriptor after its source data changed.
void displayio_lvgl_dsc_cache_drop(displayio_lvgl_image_dsc_t *dsc) {
    lv_image_cache_drop(dsc);
    lv_image_header_cache_drop(dsc);
}

#endif  // CIRCUITPY_DISPLAYIO_LVGL
