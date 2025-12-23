// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2025 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "shared-bindings/audiobusio/I2SOut.h"

#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>

#include "bindings/zephyr_kernel/__init__.h"
#include "shared-bindings/microcontroller/Pin.h"
#include "shared-module/audiocore/__init__.h"
#include "py/runtime.h"

#if CIRCUITPY_AUDIOBUSIO_I2SOUT

#define AUDIO_THREAD_STACK_SIZE 2048
#define AUDIO_THREAD_PRIORITY 5

// Forward declarations
static void fill_buffer(audiobusio_i2sout_obj_t *self, uint8_t *buffer, size_t buffer_size);
static void audio_thread_func(void *self_in, void *unused1, void *unused2);

// Helper function for Zephyr-specific initialization from device tree
mp_obj_t common_hal_audiobusio_i2sout_construct_from_device(audiobusio_i2sout_obj_t *self, const struct device *i2s_device) {
    self->base.type = &audiobusio_i2sout_type;
    self->i2s_dev = i2s_device;
    self->left_justified = false;
    self->playing = false;
    self->paused = false;
    self->sample = NULL;
    self->current_buffer = NULL;
    self->next_buffer = NULL;
    self->slab_buffer = NULL;
    self->thread_stack = NULL;
    self->thread_id = NULL;
    self->block_size = 0;

    return MP_OBJ_FROM_PTR(self);
}

// Standard audiobusio construct - not used in Zephyr port (devices come from device tree)
void common_hal_audiobusio_i2sout_construct(audiobusio_i2sout_obj_t *self,
    const mcu_pin_obj_t *bit_clock, const mcu_pin_obj_t *word_select,
    const mcu_pin_obj_t *data, const mcu_pin_obj_t *main_clock, bool left_justified) {
    mp_raise_NotImplementedError_varg(MP_ERROR_TEXT("Use device tree to define %q devices"), MP_QSTR_I2S);
}

bool common_hal_audiobusio_i2sout_deinited(audiobusio_i2sout_obj_t *self) {
    return self->i2s_dev == NULL;
}

void common_hal_audiobusio_i2sout_deinit(audiobusio_i2sout_obj_t *self) {
    if (common_hal_audiobusio_i2sout_deinited(self)) {
        return;
    }

    // Stop playback
    common_hal_audiobusio_i2sout_stop(self);

    // Free thread stack
    if (self->thread_stack != NULL) {
        m_free(self->thread_stack);
        self->thread_stack = NULL;
    }

    // Free memory slab buffer
    if (self->slab_buffer != NULL) {
        m_free(self->slab_buffer);
        self->slab_buffer = NULL;
    }

    // Note: Pins and I2S device are managed by Zephyr, not released here
    self->i2s_dev = NULL;
}

static void fill_buffer(audiobusio_i2sout_obj_t *self, uint8_t *buffer, size_t buffer_size) {
    if (self->sample == NULL || self->paused) {
        // Fill with silence
        memset(buffer, 0, buffer_size);
        return;
    }

    uint32_t bytes_filled = 0;
    while (bytes_filled < buffer_size) {
        uint8_t *sample_buffer;
        uint32_t sample_buffer_length;

        audioio_get_buffer_result_t result = audiosample_get_buffer(
            self->sample, false, 0, &sample_buffer, &sample_buffer_length);

        if (result == GET_BUFFER_ERROR) {
            // Error getting buffer, stop playback
            self->stopping = true;
            memset(buffer + bytes_filled, 0, buffer_size - bytes_filled);
            return;
        }

        if (result == GET_BUFFER_DONE) {
            if (self->loop) {
                // Reset to beginning
                audiosample_reset_buffer(self->sample, false, 0);
            } else {
                // Done playing, fill rest with silence
                self->stopping = true;
                memset(buffer + bytes_filled, 0, buffer_size - bytes_filled);
                return;
            }
        }

        // Copy data to buffer
        uint32_t bytes_to_copy = sample_buffer_length;
        if (bytes_filled + bytes_to_copy > buffer_size) {
            bytes_to_copy = buffer_size - bytes_filled;
        }

        memcpy(buffer + bytes_filled, sample_buffer, bytes_to_copy);
        bytes_filled += bytes_to_copy;
    }
}

static void audio_thread_func(void *self_in, void *unused1, void *unused2) {
    audiobusio_i2sout_obj_t *self = (audiobusio_i2sout_obj_t *)self_in;

    while (self->playing) {
        // Wait if paused
        if (self->paused) {
            k_sleep(K_MSEC(10));
            continue;
        }

        // Check if we should stop
        if (self->stopping) {
            self->playing = false;
            self->stopping = false;
            break;
        }

        // Fill next buffer
        if (self->next_buffer != NULL) {
            fill_buffer(self, self->next_buffer, self->block_size);

            // Write to I2S
            int ret = i2s_buf_write(self->i2s_dev, self->next_buffer, self->block_size);
            if (ret < 0) {
                // Error writing, stop playback
                self->playing = false;
                break;
            }
        }
    }
}

void common_hal_audiobusio_i2sout_play(audiobusio_i2sout_obj_t *self,
    mp_obj_t sample, bool loop) {
    printk("common_hal_audiobusio_i2sout_play\n");

    // Stop any existing playback
    if (self->playing) {
        common_hal_audiobusio_i2sout_stop(self);
    }

    // Get sample information
    uint8_t bits_per_sample = audiosample_get_bits_per_sample(sample);
    uint32_t sample_rate = audiosample_get_sample_rate(sample);
    uint8_t channel_count = audiosample_get_channel_count(sample);

    // Validate sample parameters
    if (bits_per_sample != 8 && bits_per_sample != 16 && bits_per_sample != 24 && bits_per_sample != 32) {
        mp_raise_ValueError(MP_ERROR_TEXT("Unsupported bits per sample"));
    }

    if (channel_count != 1 && channel_count != 2) {
        mp_raise_ValueError(MP_ERROR_TEXT("Unsupported channel count"));
    }

    // Store sample parameters
    self->sample = sample;
    self->loop = loop;
    self->bytes_per_sample = bits_per_sample / 8;
    self->channel_count = channel_count;
    self->stopping = false;

    // Calculate buffer size (100ms of audio)
    self->block_size = self->bytes_per_sample * channel_count * sample_rate / 10;
    size_t block_size = self->block_size;
    uint32_t num_blocks = 4; // Use 4 blocks for buffering

    // Allocate memory slab buffer
    if (self->slab_buffer != NULL) {
        m_free(self->slab_buffer);
    }
    self->slab_buffer = m_malloc(block_size * num_blocks);
    if (self->slab_buffer == NULL) {
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("Failed to allocate I2S buffer"));
    }

    // Initialize memory slab
    k_mem_slab_init(&self->mem_slab, self->slab_buffer, block_size, num_blocks);

    // Allocate buffers from slab
    if (k_mem_slab_alloc(&self->mem_slab, (void **)&self->current_buffer, K_NO_WAIT) != 0) {
        m_free(self->slab_buffer);
        self->slab_buffer = NULL;
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("Failed to allocate I2S buffer"));
    }

    printk("Allocating next buffer from slab\n");
    if (k_mem_slab_alloc(&self->mem_slab, (void **)&self->next_buffer, K_NO_WAIT) != 0) {
        k_mem_slab_free(&self->mem_slab, (void *)self->current_buffer);
        m_free(self->slab_buffer);
        self->slab_buffer = NULL;
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("Failed to allocate I2S buffer"));
    }

    // Configure I2S
    struct i2s_config config;
    config.word_size = bits_per_sample;
    config.channels = channel_count;
    config.format = self->left_justified ? I2S_FMT_DATA_FORMAT_LEFT_JUSTIFIED : I2S_FMT_DATA_FORMAT_I2S;
    config.options = I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER;
    config.frame_clk_freq = sample_rate;
    config.mem_slab = &self->mem_slab;
    config.block_size = block_size;
    config.timeout = 1000; // Not a k_timeout_t. In milliseconds.

    printk("Configuring I2S\n");
    int ret = i2s_configure(self->i2s_dev, I2S_DIR_TX, &config);
    if (ret < 0) {
        printk("Failed to configure I2S\n");
        k_mem_slab_free(&self->mem_slab, (void *)self->current_buffer);
        k_mem_slab_free(&self->mem_slab, (void *)self->next_buffer);
        m_free(self->slab_buffer);
        self->slab_buffer = NULL;
        mp_raise_RuntimeError(MP_ERROR_TEXT("Failed to configure I2S"));
    }

    // Fill initial buffer
    printk("Filling initial buffer\n");
    fill_buffer(self, self->current_buffer, block_size);

    // Start I2S
    printk("Starting I2S\n");
    ret = i2s_trigger(self->i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
    if (ret < 0) {
        printk("Failed to start I2S %d\n", ret);
        k_mem_slab_free(&self->mem_slab, (void *)self->current_buffer);
        k_mem_slab_free(&self->mem_slab, (void *)self->next_buffer);
        m_free(self->slab_buffer);
        self->slab_buffer = NULL;
        raise_zephyr_error(ret);
        return;
    }

    // Write first buffer
    printk("Writing first buffer\n");
    ret = i2s_buf_write(self->i2s_dev, self->current_buffer, block_size);
    if (ret < 0) {
        printk("Failed to write to I2S %d\n", ret);
        printk("Stopping I2S\n");
        i2s_trigger(self->i2s_dev, I2S_DIR_TX, I2S_TRIGGER_STOP);
        k_mem_slab_free(&self->mem_slab, (void *)self->current_buffer);
        k_mem_slab_free(&self->mem_slab, (void *)self->next_buffer);
        m_free(self->slab_buffer);
        self->slab_buffer = NULL;
        mp_raise_RuntimeError(MP_ERROR_TEXT("Failed to write to I2S"));
    }

    self->playing = true;

    // Allocate thread stack
    printk("Allocating thread stack\n");
    self->thread_stack = m_malloc(AUDIO_THREAD_STACK_SIZE);
    if (self->thread_stack == NULL) {
        i2s_trigger(self->i2s_dev, I2S_DIR_TX, I2S_TRIGGER_STOP);
        k_mem_slab_free(&self->mem_slab, (void *)&self->current_buffer);
        k_mem_slab_free(&self->mem_slab, (void *)&self->next_buffer);
        m_free(self->slab_buffer);
        self->slab_buffer = NULL;
        self->playing = false;
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("Failed to allocate thread stack"));
    }

    // Create and start audio processing thread
    printk("Creating and starting audio processing thread\n");
    self->thread_id = k_thread_create(&self->thread, self->thread_stack,
        AUDIO_THREAD_STACK_SIZE,
        audio_thread_func,
        self, NULL, NULL,
        AUDIO_THREAD_PRIORITY, 0, K_NO_WAIT);
    if (self->thread_id == NULL) {
        i2s_trigger(self->i2s_dev, I2S_DIR_TX, I2S_TRIGGER_STOP);
        k_mem_slab_free(&self->mem_slab, (void *)&self->current_buffer);
        k_mem_slab_free(&self->mem_slab, (void *)&self->next_buffer);
        m_free(self->slab_buffer);
        self->slab_buffer = NULL;
        m_free(self->thread_stack);
        self->thread_stack = NULL;
        self->playing = false;
        mp_raise_RuntimeError(MP_ERROR_TEXT("Failed to create audio thread"));
    }
}

void common_hal_audiobusio_i2sout_stop(audiobusio_i2sout_obj_t *self) {
    if (!self->playing) {
        return;
    }

    self->playing = false;
    self->paused = false;
    self->stopping = false;

    // Wait for thread to finish
    if (self->thread_id != NULL) {
        k_thread_join(self->thread_id, K_FOREVER);
        self->thread_id = NULL;
    }

    // Stop I2S
    i2s_trigger(self->i2s_dev, I2S_DIR_TX, I2S_TRIGGER_STOP);

    // Free thread stack
    if (self->thread_stack != NULL) {
        m_free(self->thread_stack);
        self->thread_stack = NULL;
    }

    // Free buffers
    if (self->current_buffer != NULL) {
        k_mem_slab_free(&self->mem_slab, (void *)&self->current_buffer);
        self->current_buffer = NULL;
    }
    if (self->next_buffer != NULL) {
        k_mem_slab_free(&self->mem_slab, (void *)&self->next_buffer);
        self->next_buffer = NULL;
    }
    if (self->slab_buffer != NULL) {
        m_free(self->slab_buffer);
        self->slab_buffer = NULL;
    }

    self->sample = NULL;
}

bool common_hal_audiobusio_i2sout_get_playing(audiobusio_i2sout_obj_t *self) {
    return self->playing;
}

void common_hal_audiobusio_i2sout_pause(audiobusio_i2sout_obj_t *self) {
    if (!self->playing || self->paused) {
        return;
    }

    self->paused = true;
    // Note: Zephyr I2S doesn't have a native pause, so we just stop filling buffers
    // The I2S will keep running but will output silence
}

void common_hal_audiobusio_i2sout_resume(audiobusio_i2sout_obj_t *self) {
    if (!self->playing || !self->paused) {
        return;
    }

    self->paused = false;
    // Thread will automatically resume filling buffers
}

bool common_hal_audiobusio_i2sout_get_paused(audiobusio_i2sout_obj_t *self) {
    return self->paused;
}

#endif // CIRCUITPY_AUDIOBUSIO_I2SOUT
