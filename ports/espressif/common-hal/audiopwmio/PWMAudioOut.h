// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Tyeth Gundry
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common-hal/microcontroller/Pin.h"
#include "common-hal/pwmio/PWMOut.h"

#include "py/obj.h"

#include "driver/gptimer.h"

// The LEDC carrier is APB_CLK (80 MHz) / 2**PWM_BITS. 10 bits puts the carrier
// at 78.125 kHz: comfortably above hearing, while leaving 10 bits of amplitude
// resolution. Going finer drags the carrier down into the audible band.
#define PWM_BITS (10)
#define SAMPLE_BITS_TO_DISCARD (16 - PWM_BITS)

typedef struct {
    mp_obj_base_t base;

    pwmio_pwmout_obj_t left_pwm;
    pwmio_pwmout_obj_t right_pwm;
    gptimer_handle_t timer;

    mp_obj_t sample;

    // Two device buffers of interleaved (left, right) duty values, already
    // scaled to PWM_BITS. The ISR drains one while audiopwmout_background()
    // refills the other.
    uint16_t *buffers[2];
    size_t buffer_size[2];     // allocated size in bytes
    size_t buffer_frames[2];   // valid frames currently in the buffer

    volatile size_t playing_frame;   // read cursor within the active buffer
    volatile uint8_t playing_buffer; // which buffer the ISR is draining
    volatile bool buffer_ready[2];   // buffer holds data the ISR has not used
    volatile bool last_buffer[2];    // this buffer ends the sample
    volatile bool stop_queued;       // the ISR reached the end of the sample

    uint16_t quiescent_value;        // scaled to PWM_BITS

    uint8_t sample_channel_count;
    uint8_t bytes_per_sample;

    bool stereo;                     // right channel in use
    bool signed_to_unsigned;
    bool single_buffer;
    bool playing;
    bool paused;
    bool loop;
} audiopwmio_pwmaudioout_obj_t;

// Called from port_background_task(): refills buffers the ISR has drained.
void audiopwmout_background(void);

// Called from reset_port(): stops any playback left running by the VM.
void audiopwmout_reset(void);
