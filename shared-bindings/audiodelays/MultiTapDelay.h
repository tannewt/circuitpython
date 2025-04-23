// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2025 Cooper Dalrymple
//
// SPDX-License-Identifier: MIT

#pragma once

#include "shared-module/audiodelays/MultiTapDelay.h"

extern const mp_obj_type_t audiodelays_multi_tap_delay_type;

void common_hal_audiodelays_multi_tap_delay_construct(audiodelays_multi_tap_delay_obj_t *self, uint32_t max_delay_ms,
    mp_obj_t delay_ms, mp_obj_t decay, mp_obj_t mix, mp_obj_t taps,
    uint32_t buffer_size, uint8_t bits_per_sample, bool samples_signed,
    uint8_t channel_count, uint32_t sample_rate);

void common_hal_audiodelays_multi_tap_delay_deinit(audiodelays_multi_tap_delay_obj_t *self);

mp_float_t common_hal_audiodelays_multi_tap_delay_get_delay_ms(audiodelays_multi_tap_delay_obj_t *self);
void common_hal_audiodelays_multi_tap_delay_set_delay_ms(audiodelays_multi_tap_delay_obj_t *self, mp_obj_t delay_ms);

mp_obj_t common_hal_audiodelays_multi_tap_delay_get_decay(audiodelays_multi_tap_delay_obj_t *self);
void common_hal_audiodelays_multi_tap_delay_set_decay(audiodelays_multi_tap_delay_obj_t *self, mp_obj_t decay);

mp_obj_t common_hal_audiodelays_multi_tap_delay_get_mix(audiodelays_multi_tap_delay_obj_t *self);
void common_hal_audiodelays_multi_tap_delay_set_mix(audiodelays_multi_tap_delay_obj_t *self, mp_obj_t mix);

mp_obj_t common_hal_audiodelays_multi_tap_delay_get_taps(audiodelays_multi_tap_delay_obj_t *self);
void common_hal_audiodelays_multi_tap_delay_set_taps(audiodelays_multi_tap_delay_obj_t *self, mp_obj_t taps);

bool common_hal_audiodelays_multi_tap_delay_get_playing(audiodelays_multi_tap_delay_obj_t *self);
void common_hal_audiodelays_multi_tap_delay_play(audiodelays_multi_tap_delay_obj_t *self, mp_obj_t sample, bool loop);
void common_hal_audiodelays_multi_tap_delay_stop(audiodelays_multi_tap_delay_obj_t *self);
