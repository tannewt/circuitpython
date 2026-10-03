// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common-hal/picodvi/Framebuffer.h"

// More than one video frame of audio (800 frames at 48 kHz, 60 Hz), so a
// bank can always be filled completely.
#define PICODVI_AUDIOOUT_STAGE_FRAMES 1024

typedef struct {
    mp_obj_base_t base;
    picodvi_framebuffer_obj_t *framebuffer;
    mp_obj_t sample;
    // The sample buffer being converted, and how many frames remain in it.
    const uint8_t *source;
    size_t source_frames;
    bool source_last;
    bool source_done;
    uint8_t bytes_per_frame;
    bool loop;
    bool paused;
    // Converted 16-bit stereo frames waiting to be sent.
    size_t stage_frames;
    int16_t stage[PICODVI_AUDIOOUT_STAGE_FRAMES * 2];
} picodvi_audioout_obj_t;

// Called by the framebuffer when it frees its audio state.
void picodvi_audioout_framebuffer_deinited(void *self);
// Deinit any AudioOut before the VM heap goes away.
void picodvi_audioout_reset(void);
