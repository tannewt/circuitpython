// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2025 Jeff Epler for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>

#include "py/obj.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#include "esp_cam_ctlr.h"

#include "shared-bindings/mipicsi/__init__.h"

// The number of framebuffers supported per camera object.
#define MIPICSI_MAX_FRAMEBUFFERS (4)

typedef struct mipicsi_camera_obj mipicsi_camera_obj_t;

// Buffer states. Transitions:
//   FREE --(ISR: get_new_trans)--> QUEUED --(ISR: trans_finished)--> FILLED
//   FILLED --(take)--> READING --(next take)--> FREE
// If no buffer is FREE when the CSI receiver needs one, the driver's private
// backup buffer is used and the completed frame is discarded.
typedef enum {
    MIPICSI_BUFFER_FREE,
    MIPICSI_BUFFER_QUEUED,
    MIPICSI_BUFFER_FILLED,
    MIPICSI_BUFFER_READING,
} mipicsi_buffer_state_t;

struct mipicsi_camera_obj {
    mp_obj_base_t base;
    esp_cam_ctlr_handle_t handle;

    uint8_t *buffers[MIPICSI_MAX_FRAMEBUFFERS];
    volatile mipicsi_buffer_state_t buffer_state[MIPICSI_MAX_FRAMEBUFFERS];
    // Index of the next buffer to try handing to the driver.
    volatile uint8_t next_buffer;
    // Number of buffers in state MIPICSI_BUFFER_FILLED.
    volatile uint8_t pending_count;
    // Index of the newest FILLED buffer, only meaningful when pending_count > 0.
    volatile uint8_t latest_buffer;
    // Index of the buffer being read by Python, or -1.
    volatile int8_t reading_buffer;

    portMUX_TYPE spinlock;

    mp_int_t h_res;
    mp_int_t v_res;
    mp_int_t framebuffer_count;
    mipicsi_pixel_format_t pixel_format;
    size_t buffer_len; // Length handed to the driver, includes cache-alignment padding.
};
