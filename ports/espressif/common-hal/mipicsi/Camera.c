// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2025 Jeff Epler for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/runtime.h"

#include "common-hal/mipicsi/Camera.h"
#include "shared-bindings/mipicsi/Camera.h"
#include "shared-bindings/mipicsi/__init__.h"
#include "bindings/espidf/__init__.h"

#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_private/esp_cache_private.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_csi.h"
#include "hal/color_hal.h"

// MIPI CSI receives data from a fixed, dedicated set of pins, so no pin
// configuration or claiming is done here.

static cam_ctlr_color_t mipicsi_color_for(mipicsi_pixel_format_t format) {
    switch (format) {
        case MIPICSI_FORMAT_RAW8:
            return CAM_CTLR_COLOR_RAW8;
        case MIPICSI_FORMAT_RAW10:
            return CAM_CTLR_COLOR_RAW10;
        case MIPICSI_FORMAT_RAW12:
            return CAM_CTLR_COLOR_RAW12;
        case MIPICSI_FORMAT_RGB888:
            return CAM_CTLR_COLOR_RGB888;
        case MIPICSI_FORMAT_YUV422:
            return CAM_CTLR_COLOR_YUV422_YUYV;
        case MIPICSI_FORMAT_GRAYSCALE:
            return CAM_CTLR_COLOR_GRAY8;
        case MIPICSI_FORMAT_RGB565:
        default:
            return CAM_CTLR_COLOR_RGB565;
    }
}

static mp_int_t mipicsi_bits_per_pixel_for(mipicsi_pixel_format_t format) {
    switch (format) {
        case MIPICSI_FORMAT_RAW10:
            return 10;
        case MIPICSI_FORMAT_RAW12:
            return 12;
        case MIPICSI_FORMAT_RGB888:
            return 24;
        case MIPICSI_FORMAT_RGB565:
        case MIPICSI_FORMAT_YUV422:
            return 16;
        case MIPICSI_FORMAT_RAW8:
        case MIPICSI_FORMAT_GRAYSCALE:
        default:
            return 8;
    }
}

// Called by the driver (in ISR context) to get the next buffer to DMA into.
static IRAM_ATTR bool mipicsi_on_get_new_trans(
    esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data) {
    mipicsi_camera_obj_t *self = user_data;

    portENTER_CRITICAL_ISR(&self->spinlock);
    for (mp_int_t i = 0; i < self->framebuffer_count; i++) {
        mp_int_t idx = (self->next_buffer + i) % self->framebuffer_count;
        if (self->buffer_state[idx] == MIPICSI_BUFFER_FREE) {
            self->buffer_state[idx] = MIPICSI_BUFFER_QUEUED;
            self->next_buffer = (idx + 1) % self->framebuffer_count;
            trans->buffer = self->buffers[idx];
            trans->buflen = self->buffer_len;
            break;
        }
    }
    portEXIT_CRITICAL_ISR(&self->spinlock);

    // If no buffer was free, trans->buffer is NULL and the driver will capture
    // into its private backup buffer. The completed frame is discarded in
    // mipicsi_on_trans_finished().
    return false;
}

// Called by the driver (in ISR context) when a frame has been written.
static IRAM_ATTR bool mipicsi_on_trans_finished(
    esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data) {
    mipicsi_camera_obj_t *self = user_data;

    portENTER_CRITICAL_ISR(&self->spinlock);
    for (mp_int_t i = 0; i < self->framebuffer_count; i++) {
        if (self->buffers[i] == trans->buffer) {
            self->buffer_state[i] = MIPICSI_BUFFER_FILLED;
            self->latest_buffer = i;
            self->pending_count++;
            break;
        }
    }
    portEXIT_CRITICAL_ISR(&self->spinlock);

    // Frames that went into the driver's private backup buffer are discarded.
    return false;
}

void common_hal_mipicsi_camera_construct(
    mipicsi_camera_obj_t *self,
    mp_int_t h_res,
    mp_int_t v_res,
    mipicsi_pixel_format_t pixel_format,
    mp_int_t data_lanes,
    mp_int_t lane_bit_rate_mbps,
    mp_int_t framebuffer_count) {

    self->h_res = h_res;
    self->v_res = v_res;
    self->pixel_format = pixel_format;
    self->framebuffer_count = framebuffer_count;
    self->reading_buffer = -1;
    self->spinlock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;

    const cam_ctlr_color_t color = mipicsi_color_for(pixel_format);

    // The received images are not converted, so the input and output color
    // types are the same and color conversion is bypassed.
    esp_cam_ctlr_csi_config_t csi_config = {
        .ctlr_id = 0,
        .h_res = h_res,
        .v_res = v_res,
        .data_lane_num = data_lanes,
        .lane_bit_rate_mbps = lane_bit_rate_mbps,
        .input_data_color_type = color,
        .output_data_color_type = color,
        .queue_items = framebuffer_count,
    };

    CHECK_ESP_RESULT(esp_cam_new_csi_ctlr(&csi_config, &self->handle));

    // Allocate framebuffers, cache-line aligned as required by the driver's
    // DMA cache syncing. Prefer PSRAM; fall back to internal RAM.
    size_t alignment = 4;
    size_t fb_size_bytes = ((size_t)h_res * v_res * mipicsi_bits_per_pixel_for(pixel_format) + 7) / 8;
    CHECK_ESP_RESULT(esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &alignment));
    self->buffer_len = (fb_size_bytes + alignment - 1) / alignment * alignment;
    uint32_t caps[] = { MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT };
    for (size_t attempt = 0; attempt < MP_ARRAY_SIZE(caps); attempt++) {
        for (mp_int_t i = 0; i < framebuffer_count; i++) {
            self->buffers[i] = heap_caps_aligned_alloc(alignment, self->buffer_len, caps[attempt]);
            if (!self->buffers[i]) {
                break;
            }
            self->buffer_state[i] = MIPICSI_BUFFER_FREE;
        }
        if (self->buffers[framebuffer_count - 1]) {
            break;
        }
        // Allocation failed (typically no PSRAM present); free any partial
        // allocation and retry from internal RAM.
        for (mp_int_t i = 0; i < framebuffer_count; i++) {
            heap_caps_aligned_free(self->buffers[i]);
            self->buffers[i] = NULL;
        }
        if (attempt + 1 == MP_ARRAY_SIZE(caps)) {
            esp_cam_ctlr_del(self->handle);
            self->handle = NULL;
            mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("could not allocate framebuffers"));
        }
    }

    esp_cam_ctlr_evt_cbs_t cbs = {
        .on_get_new_trans = mipicsi_on_get_new_trans,
        .on_trans_finished = mipicsi_on_trans_finished,
    };
    esp_err_t result = esp_cam_ctlr_register_event_callbacks(self->handle, &cbs, self);
    if (result != ESP_OK) {
        common_hal_mipicsi_camera_deinit(self);
        raise_esp_error(result);
    }

    CHECK_ESP_RESULT(esp_cam_ctlr_enable(self->handle));
    CHECK_ESP_RESULT(esp_cam_ctlr_start(self->handle));
}

void common_hal_mipicsi_camera_deinit(mipicsi_camera_obj_t *self) {
    if (common_hal_mipicsi_camera_deinited(self)) {
        return;
    }

    esp_cam_ctlr_stop(self->handle);
    esp_cam_ctlr_disable(self->handle);
    esp_cam_ctlr_del(self->handle);
    self->handle = NULL;

    for (mp_int_t i = 0; i < self->framebuffer_count; i++) {
        heap_caps_aligned_free(self->buffers[i]);
        self->buffers[i] = NULL;
        self->buffer_state[i] = MIPICSI_BUFFER_FREE;
    }
    self->pending_count = 0;
    self->reading_buffer = -1;
}

bool common_hal_mipicsi_camera_deinited(mipicsi_camera_obj_t *self) {
    return self->handle == NULL;
}

bool common_hal_mipicsi_camera_available(mipicsi_camera_obj_t *self) {
    portENTER_CRITICAL(&self->spinlock);
    bool available = self->pending_count > 0;
    portEXIT_CRITICAL(&self->spinlock);
    return available;
}

const void *common_hal_mipicsi_camera_take(mipicsi_camera_obj_t *self, int timeout_ms) {
    mp_uint_t deadline = mp_hal_ticks_ms() + (mp_uint_t)timeout_ms;

    while (true) {
        mp_int_t idx = -1;

        portENTER_CRITICAL(&self->spinlock);
        // Return the previously "taken" buffer to the free pool.
        if (self->reading_buffer >= 0) {
            self->buffer_state[self->reading_buffer] = MIPICSI_BUFFER_FREE;
            self->reading_buffer = -1;
        }
        // Take the newest filled buffer.
        if (self->pending_count > 0) {
            idx = self->latest_buffer;
            self->buffer_state[idx] = MIPICSI_BUFFER_READING;
            self->reading_buffer = idx;
            self->pending_count--;
        }
        portEXIT_CRITICAL(&self->spinlock);

        if (idx >= 0) {
            return self->buffers[idx];
        }

        if (timeout_ms <= 0 || (int32_t)(deadline - mp_hal_ticks_ms()) <= 0) {
            return NULL;
        }
        mp_hal_delay_ms(1);
        RUN_BACKGROUND_TASKS;
    }
}

mipicsi_pixel_format_t common_hal_mipicsi_camera_get_pixel_format(mipicsi_camera_obj_t *self) {
    return self->pixel_format;
}

mp_int_t common_hal_mipicsi_camera_get_width(mipicsi_camera_obj_t *self) {
    return self->h_res;
}

mp_int_t common_hal_mipicsi_camera_get_height(mipicsi_camera_obj_t *self) {
    return self->v_res;
}

mp_int_t common_hal_mipicsi_camera_get_framebuffer_count(mipicsi_camera_obj_t *self) {
    return self->framebuffer_count;
}

mp_int_t common_hal_mipicsi_camera_get_bits_per_pixel(mipicsi_camera_obj_t *self) {
    return mipicsi_bits_per_pixel_for(self->pixel_format);
}
