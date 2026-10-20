// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Tim Cocks for Adafruit Industries
//
// SPDX-License-Identifier: MIT
#include "shared-bindings/audiotap/Tap.h"
#include "shared-bindings/audiocore/__init__.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "py/runtime.h"

void common_hal_audiotap_tap_construct(audiotap_tap_obj_t *self, uint32_t capacity,
    uint32_t buffer_size, uint8_t bits_per_sample, bool samples_signed,
    uint8_t channel_count, uint32_t sample_rate) {

    self->base.bits_per_sample = bits_per_sample;
    self->base.samples_signed = samples_signed;
    self->base.channel_count = channel_count;
    self->base.sample_rate = sample_rate;
    self->base.single_buffer = false;

    // Whole frames only, so that a stereo pair is never split between buffers
    // or between the ring's end and its start.
    const uint32_t frame_bytes = (bits_per_sample / 8) * channel_count;
    self->buffer_len = MAX(frame_bytes, buffer_size - buffer_size % frame_bytes);
    self->base.max_buffer_length = self->buffer_len;

    self->buffer[0] = m_malloc_without_collect(self->buffer_len);
    memset(self->buffer[0], 0, self->buffer_len);
    self->buffer[1] = m_malloc_without_collect(self->buffer_len);
    memset(self->buffer[1], 0, self->buffer_len);
    self->last_buf_idx = 1;

    self->sample = NULL;
    self->sample_remaining_buffer = NULL;
    self->sample_buffer_length = 0;
    self->loop = false;
    self->more_data = false;

    self->capacity = capacity - capacity % channel_count;
    self->ring = NULL;
    if (self->capacity > 0) {
        self->ring = m_malloc_without_collect(self->capacity * (bits_per_sample / 8));
    }
    self->write_pos = 0;
    self->read_pos = 0;
    self->overflow = false;

    self->peak = 0;
    self->sum_squares = 0;
    self->level_count = 0;
    self->levels_reset = false;
}

void common_hal_audiotap_tap_deinit(audiotap_tap_obj_t *self) {
    audiosample_mark_deinit(&self->base);
    self->sample = NULL;
    self->buffer[0] = NULL;
    self->buffer[1] = NULL;
    self->ring = NULL;
    self->capacity = 0;
}

void audiotap_tap_reset_buffer(audiotap_tap_obj_t *self,
    bool single_channel_output,
    uint8_t channel) {
    (void)single_channel_output;
    (void)channel;
    if (self->buffer[0] == NULL) {
        return;
    }
    // The ring belongs to the reader; what it holds stays readable.
    memset(self->buffer[0], 0, self->buffer_len);
    memset(self->buffer[1], 0, self->buffer_len);
}

bool common_hal_audiotap_tap_get_playing(audiotap_tap_obj_t *self) {
    return self->sample != NULL;
}

void common_hal_audiotap_tap_play(audiotap_tap_obj_t *self, mp_obj_t sample, bool loop) {
    audiosample_must_match(&self->base, sample, false);

    self->sample = sample;
    self->loop = loop;

    audiosample_reset_buffer(self->sample, false, 0);
    audioio_get_buffer_result_t result = audiosample_get_buffer(self->sample, false, 0,
        (uint8_t **)&self->sample_remaining_buffer, &self->sample_buffer_length);
    // Track the remaining length in samples.
    self->sample_buffer_length /= (self->base.bits_per_sample / 8);
    self->more_data = result == GET_BUFFER_MORE_DATA;
}

void common_hal_audiotap_tap_stop(audiotap_tap_obj_t *self) {
    self->sample = NULL;
}

uint32_t common_hal_audiotap_tap_get_capacity(audiotap_tap_obj_t *self) {
    return self->capacity;
}

// Positions run from 0 to 2 * capacity.
static uint32_t ring_used(uint32_t capacity, uint32_t write, uint32_t read) {
    return write >= read ? write - read : write + 2 * capacity - read;
}

static uint32_t ring_index(uint32_t capacity, uint32_t pos) {
    return pos >= capacity ? pos - capacity : pos;
}

static uint32_t ring_advance(uint32_t capacity, uint32_t pos, uint32_t count) {
    pos += count;
    return pos >= 2 * capacity ? pos - 2 * capacity : pos;
}

// The sample at i, as a signed 16 bit value.
static int32_t sample_at(audiotap_tap_obj_t *self, const uint8_t *data, uint32_t i) {
    if (MP_LIKELY(self->base.bits_per_sample == 16)) {
        uint16_t raw = ((const uint16_t *)data)[i];
        return self->base.samples_signed ? (int16_t)raw : (int16_t)(raw ^ 0x8000);
    }
    uint8_t raw = data[i];
    return (self->base.samples_signed ? (int8_t)raw : (int8_t)(raw ^ 0x80)) * 256;
}

// Called by whatever plays the tap, for each run of samples from the source.
// It may be an interrupt, so it never allocates and changes only write_pos and
// the levels.
static void tap_capture(audiotap_tap_obj_t *self, const uint8_t *data, uint32_t count) {
    if (count == 0) {
        return;
    }

    if (self->levels_reset) {
        self->peak = 0;
        self->sum_squares = 0;
        self->level_count = 0;
        self->levels_reset = false;
    }
    uint32_t peak = self->peak;
    uint64_t sum = 0;
    for (uint32_t i = 0; i < count; i++) {
        int32_t value = sample_at(self, data, i);
        uint32_t magnitude = value < 0 ? -value : value;
        if (magnitude > peak) {
            peak = magnitude;
        }
        sum += (uint64_t)(value * value);
    }
    self->peak = peak;
    self->sum_squares += (float)sum / (32768.0f * 32768.0f);
    self->level_count += count;

    if (self->ring == NULL) {
        return;
    }
    const uint32_t capacity = self->capacity;
    const uint32_t sample_bytes = self->base.bits_per_sample / 8;
    uint32_t write = self->write_pos;
    uint32_t free = capacity - ring_used(capacity, write, self->read_pos);
    if (count > free) {
        // The reader fell behind. Moving read_pos to keep the newest audio
        // would race with it, so drop what doesn't fit.
        count = free;
        self->overflow = true;
    }
    while (count > 0) {
        uint32_t index = ring_index(capacity, write);
        uint32_t run = MIN(count, capacity - index);
        memcpy(self->ring + index * sample_bytes, data, run * sample_bytes);
        data += run * sample_bytes;
        count -= run;
        write = ring_advance(capacity, write, run);
    }
    // The samples must be in the ring before the reader can see them.
    __sync_synchronize();
    self->write_pos = write;
}

uint32_t common_hal_audiotap_tap_get_available(audiotap_tap_obj_t *self) {
    if (self->ring == NULL) {
        return 0;
    }
    return ring_used(self->capacity, self->write_pos, self->read_pos);
}

uint32_t common_hal_audiotap_tap_readinto(audiotap_tap_obj_t *self, uint8_t *buffer, uint32_t length) {
    if (self->ring == NULL) {
        return 0;
    }
    const uint32_t capacity = self->capacity;
    const uint32_t sample_bytes = self->base.bits_per_sample / 8;
    uint32_t read = self->read_pos;
    uint32_t write = self->write_pos;
    __sync_synchronize();
    uint32_t count = MIN(length - length % self->base.channel_count,
        ring_used(capacity, write, read));
    uint32_t done = 0;
    while (done < count) {
        uint32_t index = ring_index(capacity, read);
        uint32_t run = MIN(count - done, capacity - index);
        memcpy(buffer + done * sample_bytes, self->ring + index * sample_bytes, run * sample_bytes);
        done += run;
        read = ring_advance(capacity, read, run);
    }
    // Copy out before the writer may reuse the space.
    __sync_synchronize();
    self->read_pos = read;
    return count;
}

void common_hal_audiotap_tap_clear(audiotap_tap_obj_t *self) {
    self->read_pos = self->write_pos;
}

bool common_hal_audiotap_tap_get_overflow(audiotap_tap_obj_t *self) {
    bool overflow = self->overflow;
    self->overflow = false;
    return overflow;
}

void common_hal_audiotap_tap_get_levels(audiotap_tap_obj_t *self, mp_float_t *peak, mp_float_t *rms) {
    if (self->levels_reset) {
        // No audio passed since the last call.
        *peak = 0;
        *rms = 0;
        return;
    }
    uint32_t count = self->level_count;
    float sum_squares = self->sum_squares;
    *peak = (mp_float_t)self->peak / MICROPY_FLOAT_CONST(32768.0);
    self->levels_reset = true;
    *rms = count > 0 ? MICROPY_FLOAT_C_FUN(sqrt)((mp_float_t)sum_squares / count) : 0;
}

static void fill_silence(audiotap_tap_obj_t *self, uint8_t *out, uint32_t samples) {
    if (self->base.samples_signed) {
        memset(out, 0, samples * (self->base.bits_per_sample / 8));
    } else if (self->base.bits_per_sample == 16) {
        uint16_t *words = (uint16_t *)out;
        while (samples--) {
            *words++ = 0x8000;
        }
    } else {
        memset(out, 0x80, samples);
    }
}

audioio_get_buffer_result_t audiotap_tap_get_buffer(audiotap_tap_obj_t *self, bool single_channel_output, uint8_t channel,
    uint8_t **buffer, uint32_t *buffer_length) {
    (void)single_channel_output;
    (void)channel;

    if (self->buffer[0] == NULL) {
        // Deinitialized while something still plays it.
        *buffer = NULL;
        *buffer_length = 0;
        return GET_BUFFER_DONE;
    }

    self->last_buf_idx = !self->last_buf_idx;
    uint8_t *out = (uint8_t *)self->buffer[self->last_buf_idx];
    const uint32_t sample_bytes = self->base.bits_per_sample / 8;
    uint32_t length = self->buffer_len / sample_bytes;

    while (length != 0) {
        if (self->sample_buffer_length == 0) {
            if (!self->more_data) {
                if (self->loop && self->sample) {
                    audiosample_reset_buffer(self->sample, false, 0);
                } else {
                    self->sample = NULL;
                }
            }
            if (self->sample) {
                audioio_get_buffer_result_t result = audiosample_get_buffer(self->sample, false, 0,
                    (uint8_t **)&self->sample_remaining_buffer, &self->sample_buffer_length);
                if (result == GET_BUFFER_ERROR) {
                    self->sample = NULL;
                    self->sample_buffer_length = 0;
                    self->more_data = false;
                } else {
                    self->sample_buffer_length /= sample_bytes;
                    self->more_data = result == GET_BUFFER_MORE_DATA;
                    if (self->sample_buffer_length == 0 && self->more_data) {
                        // The source has nothing yet. Play silence rather
                        // than ask again in a loop.
                        fill_silence(self, out, length);
                        break;
                    }
                }
            }
        }

        if (self->sample == NULL) {
            fill_silence(self, out, length);
            length = 0;
        } else {
            uint32_t n = MIN(self->sample_buffer_length, length);
            memcpy(out, self->sample_remaining_buffer, n * sample_bytes);
            tap_capture(self, out, n);
            out += n * sample_bytes;
            length -= n;
            self->sample_remaining_buffer += n * sample_bytes;
            self->sample_buffer_length -= n;
        }
    }

    *buffer = (uint8_t *)self->buffer[self->last_buf_idx];
    *buffer_length = self->buffer_len;
    // Like the effects, a tap plays silence after its sample ends.
    return GET_BUFFER_MORE_DATA;
}
