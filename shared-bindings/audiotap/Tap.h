// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Tim Cocks for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

#include "shared-module/audiotap/Tap.h"

extern const mp_obj_type_t audiotap_tap_type;

void common_hal_audiotap_tap_construct(audiotap_tap_obj_t *self, uint32_t capacity,
    uint32_t buffer_size, uint8_t bits_per_sample, bool samples_signed,
    uint8_t channel_count, uint32_t sample_rate);

void common_hal_audiotap_tap_deinit(audiotap_tap_obj_t *self);

bool common_hal_audiotap_tap_get_playing(audiotap_tap_obj_t *self);
void common_hal_audiotap_tap_play(audiotap_tap_obj_t *self, mp_obj_t sample, bool loop);
void common_hal_audiotap_tap_stop(audiotap_tap_obj_t *self);

uint32_t common_hal_audiotap_tap_get_capacity(audiotap_tap_obj_t *self);
uint32_t common_hal_audiotap_tap_get_available(audiotap_tap_obj_t *self);
// Copies at most length samples, in whole frames. Returns the number copied.
uint32_t common_hal_audiotap_tap_readinto(audiotap_tap_obj_t *self, uint8_t *buffer, uint32_t length);
void common_hal_audiotap_tap_clear(audiotap_tap_obj_t *self);
bool common_hal_audiotap_tap_get_overflow(audiotap_tap_obj_t *self);
void common_hal_audiotap_tap_get_levels(audiotap_tap_obj_t *self, mp_float_t *peak, mp_float_t *rms);
