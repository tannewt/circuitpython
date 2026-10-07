// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2025 Jeff Epler for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"

#include "common-hal/mipicsi/Camera.h"
#include "shared-bindings/mipicsi/__init__.h"

extern const mp_obj_type_t mipicsi_camera_type;

void common_hal_mipicsi_camera_construct(
    mipicsi_camera_obj_t *self,
    mp_int_t h_res,
    mp_int_t v_res,
    mipicsi_pixel_format_t pixel_format,
    mp_int_t data_lanes,
    mp_int_t lane_bit_rate_mbps,
    mp_int_t framebuffer_count);

void common_hal_mipicsi_camera_deinit(mipicsi_camera_obj_t *self);
bool common_hal_mipicsi_camera_deinited(mipicsi_camera_obj_t *self);

// Returns the newest filled frame buffer, or NULL if no frame arrived within
// timeout_ms. The returned buffer remains valid until the next call to take().
const void *common_hal_mipicsi_camera_take(mipicsi_camera_obj_t *self, int timeout_ms);

bool common_hal_mipicsi_camera_available(mipicsi_camera_obj_t *self);

mipicsi_pixel_format_t common_hal_mipicsi_camera_get_pixel_format(mipicsi_camera_obj_t *self);
mp_int_t common_hal_mipicsi_camera_get_width(mipicsi_camera_obj_t *self);
mp_int_t common_hal_mipicsi_camera_get_height(mipicsi_camera_obj_t *self);
mp_int_t common_hal_mipicsi_camera_get_framebuffer_count(mipicsi_camera_obj_t *self);
mp_int_t common_hal_mipicsi_camera_get_bits_per_pixel(mipicsi_camera_obj_t *self);
