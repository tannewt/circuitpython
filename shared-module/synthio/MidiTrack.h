// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2021 Artyom Skrobov
//
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"

#include "shared-module/synthio/__init__.h"

typedef struct {
    synthio_synth_t synth;
    mp_buffer_info_t track;
    // invariant: after initial startup, pos always points just after an encoded duration, i.e., at a midi message (or at EOF)
    size_t pos;
    mp_int_t error_location;
    // current tempo in ticks per second, and the value to rewind it to
    uint32_t tempo;
    uint32_t base_tempo;
    // samples of the current delay that did not fit in span.dur
    uint32_t pending;
    // ticks per quarter note (15 bits in the file header), or 0 when the
    // timing is absolute (SMPTE) and Set Tempo events do not apply
    uint16_t ticks_per_beat;
    // last channel status byte, for events that omit it (running status)
    uint8_t running_status;
} synthio_miditrack_obj_t;


// These are not available from Python because it may be called in an interrupt.
void synthio_miditrack_reset_buffer(synthio_miditrack_obj_t *self,
    bool single_channel_output,
    uint8_t channel);

audioio_get_buffer_result_t synthio_miditrack_get_buffer(synthio_miditrack_obj_t *self,
    bool single_channel_output,
    uint8_t channel,
    uint8_t **buffer,
    uint32_t *buffer_length); // length in bytes
