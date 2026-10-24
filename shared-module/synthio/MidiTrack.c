// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2021 Artyom Skrobov
//
// SPDX-License-Identifier: MIT

#include "py/runtime.h"
#include "shared-bindings/synthio/MidiTrack.h"
#include "shared-bindings/audiocore/__init__.h"


static void record_midi_stream_error(synthio_miditrack_obj_t *self) {
    // keep the first error; later ones are usually fallout from this one
    if (self->error_location < 0) {
        self->error_location = self->pos;
    }
    self->pos = self->track.len;
}

// Check that `count` more bytes are available, ending the track if not.
static bool need_bytes(synthio_miditrack_obj_t *self, size_t count) {
    if (count > self->track.len - self->pos) {
        record_midi_stream_error(self);
        return false;
    }
    return true;
}

// A duration can be longer than span.dur (a uint16_t) can hold, so the
// remainder is kept in `pending` and handed over a chunk at a time.
static void set_remaining(synthio_miditrack_obj_t *self, uint32_t samples) {
    uint16_t chunk = samples > UINT16_MAX ? UINT16_MAX : (uint16_t)samples;
    self->synth.span.dur = chunk;
    self->pending = samples - chunk;
}

// Returns the note in the low 8 bits and the velocity in the next 8, or -1
// if the pair could not be read.
static int parse_note(synthio_miditrack_obj_t *self) {
    uint8_t *buffer = self->track.buf;
    if (!need_bytes(self, 2)) {
        return -1;
    }
    uint8_t note = buffer[self->pos++];
    uint8_t velocity = buffer[self->pos++];
    if (note > 127 || velocity > 127) {
        record_midi_stream_error(self);
        return -1;
    }
    return note | (velocity << 8);
}

// MIDI variable-length quantity: up to 4 bytes of 7 bits each.
static uint32_t decode_varint(synthio_miditrack_obj_t *self) {
    uint8_t *buffer = self->track.buf;
    uint32_t value = 0;
    for (size_t i = 0; i < 4; i++) {
        if (!need_bytes(self, 1)) {
            return 0;
        }
        uint8_t c = buffer[self->pos++];
        value = (value << 7) | (c & 0x7f);
        if (!(c & 0x80)) {
            return value;
        }
    }
    // a fifth continuation byte cannot be part of a valid quantity
    record_midi_stream_error(self);
    return 0;
}

static void decode_duration(synthio_miditrack_obj_t *self) {
    uint32_t delta = decode_varint(self);
    uint64_t samples = (uint64_t)delta * self->synth.base.sample_rate / self->tempo;
    set_remaining(self, samples > UINT32_MAX ? UINT32_MAX : (uint32_t)samples);
}

// Meta (0xFF) and system exclusive (0xF0/0xF7) events both carry an explicit
// length, so they can be skipped exactly rather than guessed at.
static void decode_system_event(synthio_miditrack_obj_t *self, uint8_t status) {
    uint8_t *buffer = self->track.buf;
    if (status == 0xFF) {
        if (!need_bytes(self, 1)) {
            return;
        }
        uint8_t type = buffer[self->pos++];
        uint32_t length = decode_varint(self);
        if (!need_bytes(self, length)) {
            return;
        }
        if (type == 0x2F) { // End of Track
            self->pos = self->track.len;
            return;
        }
        if (type == 0x51 && length == 3 && self->ticks_per_beat != 0) { // Set Tempo
            uint32_t us_per_beat = (buffer[self->pos] << 16) |
                (buffer[self->pos + 1] << 8) | buffer[self->pos + 2];
            if (us_per_beat != 0) {
                uint32_t ticks = (uint32_t)(((uint64_t)self->ticks_per_beat * 1000000) / us_per_beat);
                if (ticks != 0) {
                    self->tempo = ticks;
                }
            }
        }
        self->pos += length;
    } else if (status == 0xF0 || status == 0xF7) {
        uint32_t length = decode_varint(self);
        if (!need_bytes(self, length)) {
            return;
        }
        self->pos += length;
    } else {
        // system common and real-time bytes have no meaning in a file
        record_midi_stream_error(self);
    }
}

// invariant: pointing at a MIDI message
static void decode_until_pause(synthio_miditrack_obj_t *self) {
    uint8_t *buffer = self->track.buf;
    size_t len = self->track.len;
    do {
        if (self->pos >= len) {
            break;
        }
        uint8_t status = buffer[self->pos];
        if (status & 0x80) {
            self->pos++;
            // a channel message arms running status; a system one cancels it
            self->running_status = (status < 0xF0) ? status : 0;
        } else {
            // running status: the status byte is omitted and the previous
            // channel message is implied
            status = self->running_status;
            if (status == 0) {
                record_midi_stream_error(self);
                break;
            }
        }
        switch (status >> 4) {
            case 8:   // Note Off
            case 9: { // Note On, but a velocity of zero means Note Off
                int parsed = parse_note(self);
                if (parsed >= 0) {
                    mp_obj_t note = MP_OBJ_NEW_SMALL_INT(parsed & 0xff);
                    if ((status >> 4) == 9 && (parsed >> 8) != 0) {
                        synthio_span_change_note(&self->synth, SYNTHIO_SILENCE, note);
                    } else {
                        synthio_span_change_note(&self->synth, note, SYNTHIO_SILENCE);
                    }
                }
                break;
            }
            case 10:
            case 11:
            case 14: // two data bytes to ignore
                parse_note(self);
                break;
            case 12:
            case 13: // one data byte to ignore
                if (need_bytes(self, 1) && buffer[self->pos++] > 127) {
                    record_midi_stream_error(self);
                }
                break;
            default: // 15: meta or system exclusive
                decode_system_event(self, status);
                break;
        }
        if (self->pos < len) {
            decode_duration(self);
        }
    } while (self->pos < len && self->synth.span.dur == 0 && self->pending == 0);
}

static void start_parse(synthio_miditrack_obj_t *self) {
    self->pos = 0;
    self->error_location = -1;
    self->pending = 0;
    self->running_status = 0;
    // a tempo map applies from the top of the track, so rewind it too
    self->tempo = self->base_tempo;
    decode_duration(self);
    if (self->synth.span.dur == 0 && self->pending == 0) {
        // the usual case: the file starts with some MIDI event, not a delay
        decode_until_pause(self);
    }
}

void common_hal_synthio_miditrack_construct(synthio_miditrack_obj_t *self,
    const uint8_t *buffer, uint32_t len, uint32_t tempo, uint32_t ticks_per_beat,
    uint32_t sample_rate, mp_obj_t waveform_obj, mp_obj_t filter_obj,
    mp_obj_t envelope_obj) {

    self->tempo = self->base_tempo = tempo;
    self->ticks_per_beat = ticks_per_beat;
    self->track.buf = (void *)buffer;
    self->track.len = len;

    synthio_synth_init(&self->synth, sample_rate, 1, waveform_obj, envelope_obj);

    start_parse(self);
}

void common_hal_synthio_miditrack_deinit(synthio_miditrack_obj_t *self) {
    synthio_synth_deinit(&self->synth);
}

mp_int_t common_hal_synthio_miditrack_get_error_location(synthio_miditrack_obj_t *self) {
    return self->error_location;
}

mp_int_t common_hal_synthio_miditrack_get_tempo(synthio_miditrack_obj_t *self) {
    return self->tempo;
}

void common_hal_synthio_miditrack_set_tempo(synthio_miditrack_obj_t *self, mp_int_t value) {
    mp_int_t val = mp_arg_validate_int_min(value, 1, MP_QSTR_tempo);
    uint64_t remaining = (uint64_t)self->synth.span.dur + self->pending;
    remaining = remaining * self->tempo / val;
    set_remaining(self, remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining);
    // becomes the new baseline, so that looping does not undo it
    self->tempo = self->base_tempo = val;
}

void synthio_miditrack_reset_buffer(synthio_miditrack_obj_t *self,
    bool single_channel_output, uint8_t channel) {
    synthio_synth_reset_buffer(&self->synth, single_channel_output, channel);
    start_parse(self);
}

audioio_get_buffer_result_t synthio_miditrack_get_buffer(synthio_miditrack_obj_t *self,
    bool single_channel_output, uint8_t channel, uint8_t **buffer, uint32_t *buffer_length) {
    if (audiosample_deinited(&self->synth.base)) {
        *buffer_length = 0;
        return GET_BUFFER_ERROR;
    }

    synthio_synth_synthesize(&self->synth, buffer, buffer_length, single_channel_output ? 0 : channel);
    if (self->synth.span.dur == 0) {
        if (self->pending != 0) {
            // still inside a gap too long for a single span
            set_remaining(self, self->pending);
        } else if (self->pos == self->track.len) {
            return GET_BUFFER_DONE;
        } else {
            decode_until_pause(self);
        }
    }
    return GET_BUFFER_MORE_DATA;
}
