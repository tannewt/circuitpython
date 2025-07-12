// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2025 Cooper Dalrymple
//
// SPDX-License-Identifier: MIT
#pragma once

#include "py/obj.h"

#include "shared-module/audiocore/__init__.h"
#include "shared-module/synthio/__init__.h"
#include "shared-module/synthio/block.h"

extern const mp_obj_type_t audiodelays_multi_tap_delay_type;

typedef struct {
    audiosample_base_t base;
    uint32_t max_delay_ms;
    mp_float_t delay_ms;
    mp_float_t sample_ms;
    synthio_block_slot_t decay;
    synthio_block_slot_t mix;

    mp_float_t *tap_positions;
    mp_float_t *tap_levels;
    uint32_t *tap_offsets;
    size_t tap_len;

    int8_t *buffer[2];
    uint8_t last_buf_idx;
    uint32_t buffer_len; // max buffer in bytes

    uint8_t *sample_remaining_buffer;
    uint32_t sample_buffer_length;

    bool loop;
    bool more_data;

    int8_t *delay_buffer;
    uint32_t delay_buffer_len; // bytes
    uint32_t max_delay_buffer_len; // bytes
    uint32_t delay_buffer_pos;
    uint32_t delay_buffer_right_pos;

    mp_obj_t sample;
} audiodelays_multi_tap_delay_obj_t;

void validate_tap_value(mp_obj_t item, qstr arg_name);
mp_float_t get_tap_value(mp_obj_t item);
void recalculate_tap_offsets(audiodelays_multi_tap_delay_obj_t *self);

void audiodelays_multi_tap_delay_reset_buffer(audiodelays_multi_tap_delay_obj_t *self,
    bool single_channel_output,
    uint8_t channel);

audioio_get_buffer_result_t audiodelays_multi_tap_delay_get_buffer(audiodelays_multi_tap_delay_obj_t *self,
    bool single_channel_output,
    uint8_t channel,
    uint8_t **buffer,
    uint32_t *buffer_length);  // length in bytes
