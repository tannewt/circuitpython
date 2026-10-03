#pragma once

/*
 * This file is part of the Micro Python project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2023 Scott Shawcroft for Adafruit Industries
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

#include "py/obj.h"

struct dvi_audio_state;

typedef struct {
    mp_obj_base_t base;
    struct dvi_audio_state *dvi_audio;
    uint32_t *framebuffer;
    size_t framebuffer_len; // in words
    uint32_t *dma_commands;
    size_t dma_commands_len; // in words
    mp_uint_t width;
    mp_uint_t height;
    mp_uint_t output_width;
    uint16_t pitch; // Number of words between rows. (May be more than a width's worth.)
    uint8_t color_depth;
    int dma_pixel_channel;
    int dma_command_channel;
} picodvi_framebuffer_obj_t;

#if CIRCUITPY_PICODVI_AUDIOOUT
// Audio over DVI, used by picodvi.AudioOut.

// Allocates the audio state and starts sending audio signaling and silence.
// Does nothing if already reserved. Raises ValueError if the output is not
// 640 pixels wide, or MemoryError, leaving nothing allocated.
void picodvi_framebuffer_audio_reserve(picodvi_framebuffer_obj_t *self);

// Stops audio and frees the audio state, waiting up to two frames for DMA to
// stop reading it. Does not call the refill or its deinited callback.
void picodvi_framebuffer_audio_release(picodvi_framebuffer_obj_t *self);

// Asks for fun(data) to run in the background after every frame. Returns
// false, changing nothing, if audio is not reserved or a refill is already
// set. If the framebuffer is deinitialized first,
// picodvi_audioout_framebuffer_deinited(data) is called.
bool picodvi_framebuffer_audio_set_refill(picodvi_framebuffer_obj_t *self, void (*fun)(void *data), void *data);

// Encodes up to one frame of 16-bit stereo samples, interleaved left, right,
// into a free bank. Returns the number of sample frames used, or 0 if audio
// is not reserved or no bank is free. Never allocates or raises.
size_t picodvi_framebuffer_audio_fill(picodvi_framebuffer_obj_t *self, const int16_t *samples, size_t available);

// Returns the number of sample frames queued or playing, 0 if not reserved.
size_t picodvi_framebuffer_audio_pending(picodvi_framebuffer_obj_t *self);

// Drops queued banks, so silence follows the frame now playing.
void picodvi_framebuffer_audio_stop(picodvi_framebuffer_obj_t *self);
#endif
