// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Tyeth Gundry
//
// SPDX-License-Identifier: MIT

// PWM audio for the ESP32 family.
//
// LEDC has no DMA path for its duty registers, and the ESP32-S2's I2S has no
// PDM transmitter, so samples are pushed one at a time from a GPTimer alarm
// ISR running at the sample rate. Two device buffers are used: the ISR drains
// one while audiopwmout_background() refills the other.
//
// The ISR deliberately touches nothing but this object's own volatile state.
// In particular it must not queue a background callback: that takes a FreeRTOS
// critical section, which aborts when called from ISR context on this port.

#include "common-hal/audiopwmio/PWMAudioOut.h"

#include <stdint.h>
#include <string.h>

#include "py/gc.h"
#include "py/runtime.h"

#include "shared-bindings/audiopwmio/PWMAudioOut.h"
#include "shared-bindings/microcontroller/Pin.h"
#include "shared-bindings/pwmio/PWMOut.h"
#include "shared-module/audiocore/__init__.h"

#include "driver/gptimer.h"
#include "esp_attr.h"
#include "hal/ledc_ll.h"
#include "soc/soc.h"

// Carrier frequency: 80 MHz / 1024 = 78125 Hz. pwmio derives PWM_BITS back out
// of this when it picks a duty resolution, so the two stay in step.
#define PWM_FREQUENCY (APB_CLK_FREQ >> PWM_BITS)

// GPTimer tick rate. The timer group divider must be at least 2, so 40 MHz is
// the finest available, and it keeps the sample-rate error below a cent
// (44100 Hz -> 907 ticks -> 44101.4 Hz).
#define TIMER_RESOLUTION_HZ (APB_CLK_FREQ / 2)

// Each object needs its own LEDC timer, so that bounds how many can play.
static audiopwmio_pwmaudioout_obj_t *active_audio[SOC_LEDC_TIMER_NUM];

// Write one duty value to a LEDC channel. This mirrors what ledc_update_duty()
// does, minus the argument checks and the spinlock, so it is cheap enough to
// run at the sample rate. The fade parameters it relies on (scale 0, so the
// duty is latched rather than ramped) are left set up by pwmio's construct.
static inline void IRAM_ATTR set_duty(uint8_t channel, uint32_t duty) {
    ledc_ll_set_duty_int_part(&LEDC, LEDC_LOW_SPEED_MODE, channel, duty);
    ledc_ll_set_sig_out_en(&LEDC, LEDC_LOW_SPEED_MODE, channel, true);
    ledc_ll_set_duty_start(&LEDC, LEDC_LOW_SPEED_MODE, channel);
    ledc_ll_ls_channel_update(&LEDC, LEDC_LOW_SPEED_MODE, channel);
}

static bool IRAM_ATTR timer_alarm_cb(gptimer_handle_t timer,
    const gptimer_alarm_event_data_t *edata, void *user_ctx) {
    audiopwmio_pwmaudioout_obj_t *self = (audiopwmio_pwmaudioout_obj_t *)user_ctx;
    (void)timer;
    (void)edata;

    uint8_t buf_index = self->playing_buffer;
    if (!self->buffer_ready[buf_index]) {
        // Underrun, or the sample ended. Hold the quiescent level rather than
        // leaving the last sample on the pin, which would be a DC offset.
        set_duty(self->left_pwm.chan_handle.channel, self->quiescent_value);
        if (self->stereo) {
            set_duty(self->right_pwm.chan_handle.channel, self->quiescent_value);
        }
        return false;
    }

    size_t frame = self->playing_frame;
    const uint16_t *buf = self->buffers[buf_index];

    set_duty(self->left_pwm.chan_handle.channel, buf[2 * frame]);
    if (self->stereo) {
        set_duty(self->right_pwm.chan_handle.channel, buf[2 * frame + 1]);
    }

    frame++;
    if (frame >= self->buffer_frames[buf_index]) {
        if (self->single_buffer && self->loop) {
            // The whole sample fits in one buffer and repeats forever, so wrap
            // in place. This keeps looping RawSamples glitch-free: no refill,
            // and no dependency on how promptly the background task runs.
            self->playing_frame = 0;
            return false;
        }
        // This buffer is spent. Flag it; the background task does the refill.
        self->buffer_ready[buf_index] = false;
        if (self->last_buffer[buf_index]) {
            self->stop_queued = true;
        }
        self->playing_frame = 0;
        if (!self->single_buffer) {
            self->playing_buffer = buf_index ^ 1;
        }
    } else {
        self->playing_frame = frame;
    }

    return false;
}

// Convert one source buffer into device buffer `buf_index`, scaling samples to
// PWM_BITS and always writing interleaved (left, right) pairs.
static void fill_buffer(audiopwmio_pwmaudioout_obj_t *self, uint8_t buf_index) {
    uint8_t *source;
    uint32_t source_length;
    audioio_get_buffer_result_t result =
        audiosample_get_buffer(self->sample, false, 0, &source, &source_length);

    if (result == GET_BUFFER_ERROR) {
        self->stop_queued = true;
        return;
    }

    size_t frames = source_length / self->bytes_per_sample / self->sample_channel_count;
    if (frames * 2 * sizeof(uint16_t) > self->buffer_size[buf_index]) {
        // Should not happen: play() sized the buffers from max_buffer_length.
        frames = self->buffer_size[buf_index] / (2 * sizeof(uint16_t));
    }

    uint16_t *dest = self->buffers[buf_index];
    if (self->bytes_per_sample == 1) {
        uint8_t xor_mask = self->signed_to_unsigned ? 0x80 : 0;
        for (size_t i = 0; i < frames; i++) {
            uint16_t left = (uint16_t)((source[i * self->sample_channel_count] ^ xor_mask) << (PWM_BITS - 8));
            uint16_t right = left;
            if (self->sample_channel_count == 2) {
                right = (uint16_t)((source[i * 2 + 1] ^ xor_mask) << (PWM_BITS - 8));
            }
            *dest++ = left;
            *dest++ = right;
        }
    } else {
        uint16_t xor_mask = self->signed_to_unsigned ? 0x8000 : 0;
        // Cast via void * : the sample buffer is 16-bit aligned for 16-bit
        // samples, but the protocol hands it over as uint8_t *.
        const uint16_t *source16 = (const uint16_t *)(const void *)source;
        for (size_t i = 0; i < frames; i++) {
            uint16_t left = (uint16_t)((source16[i * self->sample_channel_count] ^ xor_mask) >> SAMPLE_BITS_TO_DISCARD);
            uint16_t right = left;
            if (self->sample_channel_count == 2) {
                right = (uint16_t)((source16[i * 2 + 1] ^ xor_mask) >> SAMPLE_BITS_TO_DISCARD);
            }
            *dest++ = left;
            *dest++ = right;
        }
    }

    self->buffer_frames[buf_index] = frames;
    self->last_buffer[buf_index] = false;

    if (result == GET_BUFFER_DONE) {
        if (self->loop) {
            audiosample_reset_buffer(self->sample, false, 0);
        } else {
            self->last_buffer[buf_index] = true;
        }
    }

    // Publish last, so the ISR never sees a half-filled buffer.
    __asm__ volatile ("" ::: "memory");
    self->buffer_ready[buf_index] = (frames > 0);
    if (frames == 0 && result == GET_BUFFER_DONE && !self->loop) {
        self->stop_queued = true;
    }
}

// Tear down the timer without touching the heap, so this is also safe to call
// while the VM is being reset.
static void stop_timer(audiopwmio_pwmaudioout_obj_t *self) {
    if (self->timer != NULL) {
        gptimer_stop(self->timer);
        gptimer_disable(self->timer);
        gptimer_del_timer(self->timer);
        self->timer = NULL;
    }
    for (size_t i = 0; i < MP_ARRAY_SIZE(active_audio); i++) {
        if (active_audio[i] == self) {
            active_audio[i] = NULL;
        }
    }
}

static void free_buffers(audiopwmio_pwmaudioout_obj_t *self) {
    for (uint8_t i = 0; i < 2; i++) {
        if (self->buffers[i] != NULL) {
            #if MICROPY_MALLOC_USES_ALLOCATED_SIZE
            m_free(self->buffers[i], self->buffer_size[i]);
            #else
            m_free(self->buffers[i]);
            #endif
            self->buffers[i] = NULL;
        }
        self->buffer_size[i] = 0;
        self->buffer_frames[i] = 0;
        self->buffer_ready[i] = false;
        self->last_buffer[i] = false;
    }
}

void audiopwmout_background(void) {
    for (size_t i = 0; i < MP_ARRAY_SIZE(active_audio); i++) {
        audiopwmio_pwmaudioout_obj_t *self = active_audio[i];
        if (self == NULL || !self->playing) {
            continue;
        }
        if (self->stop_queued) {
            common_hal_audiopwmio_pwmaudioout_stop(self);
            continue;
        }
        if (self->paused) {
            continue;
        }
        for (uint8_t buf = 0; buf < 2; buf++) {
            if (!self->buffer_ready[buf] && !self->stop_queued) {
                fill_buffer(self, buf);
            }
            if (self->single_buffer) {
                // A single-buffer sample hands back the same memory every
                // time, so there is nothing to gain from a second buffer.
                break;
            }
        }
    }
}

void audiopwmout_reset(void) {
    for (size_t i = 0; i < MP_ARRAY_SIZE(active_audio); i++) {
        if (active_audio[i] != NULL) {
            active_audio[i]->playing = false;
            stop_timer(active_audio[i]);
        }
    }
}

// Caller validates that pins are free.
void common_hal_audiopwmio_pwmaudioout_construct(audiopwmio_pwmaudioout_obj_t *self,
    const mcu_pin_obj_t *left_channel, const mcu_pin_obj_t *right_channel, uint16_t quiescent_value) {

    self->stereo = right_channel != NULL;
    self->left_pwm.deinited = true;
    self->right_pwm.deinited = true;
    self->timer = NULL;
    self->sample = NULL;
    self->playing = false;
    self->paused = false;

    // variable_frequency is false so that the two channels of a stereo pair can
    // share one LEDC timer. At PWM_FREQUENCY pwmio picks a PWM_BITS-wide duty.
    pwmout_result_t result = common_hal_pwmio_pwmout_construct(
        &self->left_pwm, left_channel, 0, PWM_FREQUENCY, false);
    if (result == PWMOUT_OK && self->stereo) {
        result = common_hal_pwmio_pwmout_construct(
            &self->right_pwm, right_channel, 0, PWM_FREQUENCY, false);
        if (result != PWMOUT_OK) {
            common_hal_pwmio_pwmout_deinit(&self->left_pwm);
        }
    }
    if (result != PWMOUT_OK) {
        common_hal_pwmio_pwmout_raise_error(result);
    }

    self->quiescent_value = quiescent_value >> SAMPLE_BITS_TO_DISCARD;
    common_hal_pwmio_pwmout_set_duty_cycle(&self->left_pwm, quiescent_value);
    if (self->stereo) {
        common_hal_pwmio_pwmout_set_duty_cycle(&self->right_pwm, quiescent_value);
    }
}

bool common_hal_audiopwmio_pwmaudioout_deinited(audiopwmio_pwmaudioout_obj_t *self) {
    return common_hal_pwmio_pwmout_deinited(&self->left_pwm);
}

void common_hal_audiopwmio_pwmaudioout_deinit(audiopwmio_pwmaudioout_obj_t *self) {
    if (common_hal_audiopwmio_pwmaudioout_deinited(self)) {
        return;
    }

    common_hal_audiopwmio_pwmaudioout_stop(self);

    common_hal_pwmio_pwmout_deinit(&self->left_pwm);
    if (self->stereo) {
        common_hal_pwmio_pwmout_deinit(&self->right_pwm);
    }
}

void common_hal_audiopwmio_pwmaudioout_play(audiopwmio_pwmaudioout_obj_t *self,
    mp_obj_t sample, bool loop) {

    if (common_hal_audiopwmio_pwmaudioout_get_playing(self)) {
        common_hal_audiopwmio_pwmaudioout_stop(self);
    }

    audiosample_base_t *base = audiosample_check(sample);

    uint32_t sample_rate = audiosample_get_sample_rate(base);
    if (sample_rate == 0 || sample_rate > TIMER_RESOLUTION_HZ) {
        mp_arg_error_invalid(MP_QSTR_sample_rate);
    }

    self->sample = sample;
    self->loop = loop;
    self->bytes_per_sample = audiosample_get_bits_per_sample(base) / 8;
    self->sample_channel_count = audiosample_get_channel_count(base);

    uint32_t max_buffer_length;
    uint8_t spacing;
    audiosample_get_buffer_structure(base, false, &self->single_buffer,
        &self->signed_to_unsigned, &max_buffer_length, &spacing);

    // Two device buffers of interleaved 16-bit pairs.
    size_t frames = max_buffer_length / self->bytes_per_sample / self->sample_channel_count;
    size_t buffer_size = frames * 2 * sizeof(uint16_t);
    if (buffer_size == 0) {
        mp_arg_error_invalid(MP_QSTR_sample);
    }

    // Claim a slot before allocating, so a failure here frees nothing.
    size_t slot = MP_ARRAY_SIZE(active_audio);
    for (size_t i = 0; i < MP_ARRAY_SIZE(active_audio); i++) {
        if (active_audio[i] == NULL) {
            slot = i;
            break;
        }
    }
    if (slot == MP_ARRAY_SIZE(active_audio)) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("Failed to start async audio"));
    }

    free_buffers(self);
    for (uint8_t i = 0; i < 2; i++) {
        self->buffers[i] = m_malloc_without_collect(buffer_size);
        self->buffer_size[i] = buffer_size;
        if (self->single_buffer) {
            break;
        }
    }

    audiosample_reset_buffer(sample, false, 0);

    self->playing_buffer = 0;
    self->playing_frame = 0;
    self->stop_queued = false;
    self->paused = false;
    self->playing = true;

    // Prime both buffers before the timer starts.
    fill_buffer(self, 0);
    if (!self->single_buffer && !self->stop_queued) {
        fill_buffer(self, 1);
    }
    if (self->stop_queued) {
        // Nothing to play (empty or broken sample). Don't start the timer, or
        // there would be nothing to shut it down again.
        common_hal_audiopwmio_pwmaudioout_stop(self);
        return;
    }

    gptimer_config_t timer_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = TIMER_RESOLUTION_HZ,
        .intr_priority = 0,
    };
    gptimer_event_callbacks_t callbacks = {
        .on_alarm = timer_alarm_cb,
    };
    // CircuitPython builds with CONFIG_GPTIMER_CTRL_FUNC_IN_IRAM, under which
    // gptimer_set_alarm_action() rejects a config that is not in internal RAM.
    // The IDF docs call for a static instance here, so give it one.
    static gptimer_alarm_config_t alarm_config;
    alarm_config.alarm_count = TIMER_RESOLUTION_HZ / sample_rate;
    alarm_config.reload_count = 0;
    alarm_config.flags.auto_reload_on_alarm = true;

    active_audio[slot] = self;
    if (gptimer_new_timer(&timer_config, &self->timer) != ESP_OK ||
        gptimer_register_event_callbacks(self->timer, &callbacks, self) != ESP_OK ||
        gptimer_set_alarm_action(self->timer, &alarm_config) != ESP_OK ||
        gptimer_enable(self->timer) != ESP_OK ||
        gptimer_start(self->timer) != ESP_OK) {
        // Leave nothing half-started: a stuck `playing` would hang any
        // `while playing:` loop forever.
        self->playing = false;
        stop_timer(self);
        free_buffers(self);
        mp_raise_RuntimeError(MP_ERROR_TEXT("Failed to start async audio"));
    }
}

void common_hal_audiopwmio_pwmaudioout_stop(audiopwmio_pwmaudioout_obj_t *self) {
    stop_timer(self);

    self->playing = false;
    self->paused = false;
    self->stop_queued = false;
    self->sample = NULL;

    // Park the output at the quiescent level so the speaker does not sit at a
    // DC offset between samples.
    if (!common_hal_pwmio_pwmout_deinited(&self->left_pwm)) {
        common_hal_pwmio_pwmout_set_duty_cycle(&self->left_pwm,
            self->quiescent_value << SAMPLE_BITS_TO_DISCARD);
    }
    if (self->stereo && !common_hal_pwmio_pwmout_deinited(&self->right_pwm)) {
        common_hal_pwmio_pwmout_set_duty_cycle(&self->right_pwm,
            self->quiescent_value << SAMPLE_BITS_TO_DISCARD);
    }

    free_buffers(self);
}

bool common_hal_audiopwmio_pwmaudioout_get_playing(audiopwmio_pwmaudioout_obj_t *self) {
    return self->playing;
}

void common_hal_audiopwmio_pwmaudioout_pause(audiopwmio_pwmaudioout_obj_t *self) {
    if (!self->playing || self->paused || self->timer == NULL) {
        return;
    }
    gptimer_stop(self->timer);
    self->paused = true;
}

void common_hal_audiopwmio_pwmaudioout_resume(audiopwmio_pwmaudioout_obj_t *self) {
    if (!self->paused || self->timer == NULL) {
        return;
    }
    self->paused = false;
    gptimer_start(self->timer);
}

bool common_hal_audiopwmio_pwmaudioout_get_paused(audiopwmio_pwmaudioout_obj_t *self) {
    return self->paused;
}
