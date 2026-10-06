// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include <string.h>

#include "py/runtime.h"

#include "bindings/picodvi/AudioOut.h"
#include "shared-module/audiocore/__init__.h"

// Convert up to space frames of the current sample buffer into the stage.
static void audioout_convert(picodvi_audioout_obj_t *self, size_t space) {
    audiosample_base_t *base = audiosample_cast_obj(self->sample);
    size_t n = MIN(self->source_frames, space);
    int16_t *out = &self->stage[self->stage_frames * 2];
    const uint8_t *in = self->source;
    bool stereo = base->channel_count == 2;
    if (base->bits_per_sample == 8) {
        if (base->samples_signed) {
            if (stereo) {
                audiosample_convert_s8s_s16s(out, (const int8_t *)in, n);
            } else {
                audiosample_convert_s8m_s16s(out, (const int8_t *)in, n);
            }
        } else {
            if (stereo) {
                audiosample_convert_u8s_s16s(out, in, n);
            } else {
                audiosample_convert_u8m_s16s(out, in, n);
            }
        }
    } else {
        if (base->samples_signed) {
            if (stereo) {
                memcpy(out, in, n * 4);
            } else {
                audiosample_convert_s16m_s16s(out, (const int16_t *)in, n);
            }
        } else {
            if (stereo) {
                audiosample_convert_u16s_s16s(out, (const uint16_t *)in, n);
            } else {
                audiosample_convert_u16m_s16s(out, (const uint16_t *)in, n);
            }
        }
    }
    self->source += n * self->bytes_per_frame;
    self->source_frames -= n;
    self->stage_frames += n;
}

// Top up the stage from the sample, getting new buffers and looping as needed.
static void audioout_stage(picodvi_audioout_obj_t *self) {
    while (!self->source_done && self->stage_frames < PICODVI_AUDIOOUT_STAGE_FRAMES) {
        if (self->source_frames == 0) {
            if (self->source_last) {
                if (!self->loop) {
                    self->source_done = true;
                    return;
                }
                audiosample_reset_buffer(self->sample, false, 0);
            }
            uint8_t *buffer;
            uint32_t length;
            audioio_get_buffer_result_t result = audiosample_get_buffer(self->sample, false, 0, &buffer, &length);
            if (result == GET_BUFFER_ERROR) {
                self->source_done = true;
                return;
            }
            self->source = buffer;
            self->source_frames = length / self->bytes_per_frame;
            self->source_last = result == GET_BUFFER_DONE;
            continue;
        }
        audioout_convert(self, PICODVI_AUDIOOUT_STAGE_FRAMES - self->stage_frames);
    }
}

// Runs in the background after every frame, and once from play() and
// resume(). Fills every free bank from the stage.
static void audioout_refill(void *data) {
    picodvi_audioout_obj_t *self = data;
    if (self->sample == MP_OBJ_NULL || self->paused || self->refilling) {
        return;
    }
    self->refilling = true;
    while (true) {
        audioout_stage(self);
        if (self->stage_frames == 0) {
            break;
        }
        size_t used = picodvi_framebuffer_audio_fill(self->framebuffer, self->stage, self->stage_frames);
        if (used == 0) {
            break;
        }
        self->stage_frames -= used;
        memmove(self->stage, &self->stage[used * 2], self->stage_frames * 4);
    }
    self->refilling = false;
}

void common_hal_picodvi_audioout_construct(picodvi_audioout_obj_t *self, picodvi_framebuffer_obj_t *framebuffer) {
    picodvi_framebuffer_audio_reserve(framebuffer);
    if (!picodvi_framebuffer_audio_set_refill(framebuffer, audioout_refill, self)) {
        mp_raise_RuntimeError_varg(MP_ERROR_TEXT("%q in use"), MP_QSTR_framebuffer);
    }
    self->framebuffer = framebuffer;
    self->sample = MP_OBJ_NULL;
    // Keep this object alive while it is attached, as the frame interrupt
    // schedules refills for it.
    MP_STATE_PORT(picodvi_audioout) = MP_OBJ_FROM_PTR(self);
}

void picodvi_audioout_framebuffer_deinited(void *data) {
    picodvi_audioout_obj_t *self = data;
    self->sample = MP_OBJ_NULL;
    self->framebuffer = NULL;
    MP_STATE_PORT(picodvi_audioout) = MP_OBJ_NULL;
}

void picodvi_audioout_reset(void) {
    mp_obj_t audioout = MP_STATE_PORT(picodvi_audioout);
    if (audioout != MP_OBJ_NULL) {
        common_hal_picodvi_audioout_deinit(MP_OBJ_TO_PTR(audioout));
    }
}

bool common_hal_picodvi_audioout_deinited(picodvi_audioout_obj_t *self) {
    return self->framebuffer == NULL;
}

void common_hal_picodvi_audioout_deinit(picodvi_audioout_obj_t *self) {
    if (common_hal_picodvi_audioout_deinited(self)) {
        return;
    }
    common_hal_picodvi_audioout_stop(self);
    picodvi_framebuffer_audio_release(self->framebuffer);
    self->framebuffer = NULL;
    MP_STATE_PORT(picodvi_audioout) = MP_OBJ_NULL;
}

void common_hal_picodvi_audioout_play(picodvi_audioout_obj_t *self, mp_obj_t sample, bool loop) {
    audiosample_base_t *base = audiosample_check(sample);
    mp_arg_validate_int(base->sample_rate, 48000, MP_QSTR_sample_rate);
    mp_arg_validate_int_range(base->channel_count, 1, 2, MP_QSTR_channel_count);
    if (base->bits_per_sample != 8 && base->bits_per_sample != 16) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("Invalid %q"), MP_QSTR_bits_per_sample);
    }
    common_hal_picodvi_audioout_stop(self);
    audiosample_reset_buffer(sample, false, 0);
    self->bytes_per_frame = base->channel_count * base->bits_per_sample / 8;
    self->source_frames = 0;
    self->source_last = false;
    self->source_done = false;
    self->stage_frames = 0;
    self->loop = loop;
    self->paused = false;
    self->refilling = false;
    self->sample = sample;
    audioout_refill(self);
}

void common_hal_picodvi_audioout_stop(picodvi_audioout_obj_t *self) {
    self->sample = MP_OBJ_NULL;
    self->paused = false;
    picodvi_framebuffer_audio_stop(self->framebuffer);
}

bool common_hal_picodvi_audioout_get_playing(picodvi_audioout_obj_t *self) {
    if (self->sample == MP_OBJ_NULL) {
        return false;
    }
    return !self->source_done || self->stage_frames > 0 ||
           picodvi_framebuffer_audio_pending(self->framebuffer) > 0;
}

void common_hal_picodvi_audioout_pause(picodvi_audioout_obj_t *self) {
    self->paused = true;
}

void common_hal_picodvi_audioout_resume(picodvi_audioout_obj_t *self) {
    self->paused = false;
    audioout_refill(self);
}

bool common_hal_picodvi_audioout_get_paused(picodvi_audioout_obj_t *self) {
    return self->paused;
}

MP_REGISTER_ROOT_POINTER(mp_obj_t picodvi_audioout);
