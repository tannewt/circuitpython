// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Vladimir Smitka
//
// SPDX-License-Identifier: MIT

#include "shared-module/picogame/Display.h"

#include "py/runtime.h"
#include "shared-module/picogame/__init__.h"
#include "shared-module/displayio/display_core.h"
#include "shared-bindings/busio/SPI.h"
#include "shared-bindings/displayio/__init__.h"
#include "shared-bindings/fourwire/FourWire.h"

void common_hal_picogame_display_construct(picogame_display_obj_t *self,
    busdisplay_busdisplay_obj_t *display, bool rgb444) {
    self->display = display;
    #if CIRCUITPY_PICOGAME_RGB444
    self->rgb444 = rgb444;
    #else
    if (rgb444) {
        mp_raise_NotImplementedError(MP_ERROR_TEXT("Operation or feature not supported"));
    }
    self->rgb444 = false;
    #endif

    // The strips go straight to the SPI bus; only FourWire buses are supported.
    if (!mp_obj_is_type(display->bus.bus, &fourwire_fourwire_type)) {
        mp_raise_TypeError_varg(MP_ERROR_TEXT("%q must be of type %q, not %q"),
            MP_QSTR_display, MP_QSTR_FourWire, mp_obj_get_type(display->bus.bus)->name);
    }

    #if CIRCUITPY_PICOGAME_RGB444
    // Tell the panel which pixel format we'll send (COLMOD). Asserting it here also recovers from
    // a previous program that left the panel in the other format (the setting survives soft reset).
    picogame_set_pixel_format(display, rgb444);
    #endif
}

void common_hal_picogame_display_render(picogame_display_obj_t *self,
    mp_obj_t *items, uint8_t *kinds, size_t n,
    uint16_t *buf_a, uint16_t *buf_b, size_t buf_pixels,
    int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t background,
    int ox, int oy) {

    busdisplay_busdisplay_obj_t *display = self->display;
    fourwire_fourwire_obj_t *fw = MP_OBJ_TO_PTR(display->bus.bus);
    busio_spi_obj_t *bus = fw->bus;

    // RGB444 packs 2 px into 3 bytes, so each strip row must be an even number of pixels
    // (whole bytes). Widen the region to even bounds; the extra <=1 px per side just repaints.
    #if CIRCUITPY_PICOGAME_RGB444
    if (self->rgb444) {
        x0 &= ~1;
        x1 = (x1 + 1) & ~1;
        if (x1 > display->core.width) {
            x1 = display->core.width;
        }
    }
    #endif

    // Compute geometry + open the GRAM window (set region, begin transaction, RAMWR).
    int region_w, strip_h;
    int cx0 = x0, cy0 = y0, cx1 = x1, cy1 = y1;   // strip_begin clamps these to the panel in place
    if (!picogame_strip_begin(display, &cx0, &cy0, &cx1, &cy1, buf_pixels, &region_w, &strip_h)) {
        return;
    }

    uint16_t *bufs[2] = { buf_a, buf_b };
    int cur = 0;
    bool first = true;
    #if CIRCUITPY_PICOGAME_RGB444
    const bool rgb444 = self->rgb444;        // hoist: invariant across all strips
    #endif

    // A StripDraw callback may latch a BaseException (Ctrl-C / auto-reload). Like the portable
    // picogame_render_region, we must re-raise it -- but only AFTER the strip in flight is sent
    // and the bus transaction closes, so hold it here and propagate below.
    mp_obj_t pending = MP_OBJ_NULL;

    for (int sy = cy0; sy < cy1; sy += strip_h) {
        int sh = picogame_imin(strip_h, cy1 - sy);
        uint16_t *buf = bufs[cur];

        // Blit this strip while the *other* buffer is still being sent.
        pending = picogame_blit_strip_layers(buf, region_w, sy, sh, cx0, items, kinds, n, background, ox, oy);

        // RGB444: pack the just-blitted RGB565 strip in place (2 px -> 3 bytes) before sending.
        #if CIRCUITPY_PICOGAME_RGB444
        size_t nbytes = rgb444
            ? picogame_pack_rgb444(buf, (size_t)region_w * sh)
            : (size_t)region_w * sh * 2;
        #else
        size_t nbytes = (size_t)region_w * sh * 2;
        #endif

        if (first) {
            // The first strip goes through the display bus: it drives DC high for data, which
            // then stays high for the strips sent straight to the SPI bus.
            display->bus.send(display->bus.bus, DISPLAY_DATA,
                CHIP_SELECT_UNTOUCHED, (uint8_t *)buf, nbytes);
            first = false;
        } else {
            // Ends the previous strip's transfer first, so its buffer is free for the next blit.
            common_hal_busio_spi_write_start(bus, (uint8_t *)buf, nbytes, &self->sent);
        }
        cur ^= 1;
        if (pending != MP_OBJ_NULL) {   // callback interrupted: this strip is queued, now stop + flush
            break;
        }
    }

    // The last strip must be on the wire before CS rises, as busdisplay's refresh does it.
    displayio_display_bus_flush(&display->bus);
    displayio_display_bus_end_transaction(&display->bus);

    if (pending != MP_OBJ_NULL) {        // bus now closed -> safe to re-raise (Ctrl-C / reload)
        nlr_raise(MP_OBJ_TO_PTR(pending));
    }
}
