/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2017 Radomir Dopieralski
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "Layer.h"
#include "Text.h"
#include "__init__.h"
#include "shared-bindings/_stage/Layer.h"
#include "shared-bindings/_stage/Text.h"


bool render_stage(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
        mp_obj_t *layers, size_t layers_size,
        uint16_t *buffer, size_t buffer_size,
        busio_spi_obj_t *spi, uint8_t display_scale) {

    size_t index = 0;
    size_t row_size = (x1 - x0) * display_scale;
    for (uint16_t y = y0; y < y1; ++y) {
        for (uint8_t subrow = 0; subrow < display_scale; subrow++) {
            for (uint16_t x = x0; x < x1; ++x) {
                // We've already computed this pixel once a row ago.
                if (subrow > 0 && index >= row_size) {
                    for (uint8_t subpixel = 0; subpixel < display_scale; subpixel++) {
                        buffer[index + subpixel] = buffer[index - row_size];
                    }
                } else {
                    uint16_t transparent = 0;
                    uint16_t c = transparent;
                    for (size_t layer = 0; layer < layers_size; ++layer) {
                        layer_obj_t *obj = MP_OBJ_TO_PTR(layers[layer]);
                        if (obj->base.type == &mp_type_layer) {
                            c = get_layer_pixel(obj, x, y);
                        } else if (obj->base.type == &mp_type_text) {
                            c = get_text_pixel((text_obj_t *)obj, x, y);
                        }
                        if (c != transparent && c != TRANSPARENT) {
                            break;
                        }
                    }
                    for (uint8_t subpixel = 0; subpixel < display_scale; subpixel++) {
                        buffer[index + subpixel] = c;
                    }
                }

                index += display_scale;
                // The buffer is full, send it.
                if (index >= buffer_size) {
                    if (!common_hal_busio_spi_write(spi,
                            ((uint8_t*)buffer), buffer_size * 2)) {
                        return false;
                    }
                    index = 0;
                }
            }
        }
    }
    // Send the remaining data.
    if (index) {
        if (!common_hal_busio_spi_write(spi, ((uint8_t*)buffer), index * 2)) {
            return false;
        }
    }
    return true;
}
