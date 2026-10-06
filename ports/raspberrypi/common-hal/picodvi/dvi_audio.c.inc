// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Adafruit Industries
//
// SPDX-License-Identifier: MIT

// 48 kHz stereo DVI audio for the RP2350 picodvi Framebuffer.
// No interrupt-time allocation, sample conversion, packet encoding, or Python callbacks.

#include "hardware/clocks.h"
#include "hardware/sync.h"
#include "supervisor/background_callback.h"
#include "common-hal/picodvi/AudioOut.h"
#include "common-hal/picodvi/dvi_audio_packet.c.inc"

#define DVI_AUDIO_RATE 48000u
#define DVI_AUDIO_BANKS 2
#define DVI_AUDIO_H_TOTAL (MODE_640_H_FRONT_PORCH + MODE_640_H_SYNC_WIDTH + MODE_640_H_BACK_PORCH + MODE_640_H_ACTIVE_PIXELS)
// The longest line dvi_audio_build_line() writes: an active line.
#define DVI_AUDIO_LINE_WORDS (7 + HSTX_DATA_ISLAND_WORDS + 4 + 9)

typedef uint32_t dvi_audio_line_t[DVI_AUDIO_LINE_WORDS];
typedef void (*dvi_audio_refill_t)(void *data);

typedef struct {
    uint32_t *commands;
    dvi_audio_line_t *lines;
    volatile uint32_t samples;
    volatile uint32_t order;
    volatile bool ready;
} dvi_audio_bank_t;

typedef struct dvi_audio_state {
    // Plain video command list plus audio headers, silence and active-line
    // preambles. Banks start as copies of it, and it is shown when no bank is
    // ready.
    uint32_t *base_commands;
    dvi_audio_bank_t bank[DVI_AUDIO_BANKS];
    volatile int active;
    // Set to go back to video only at the next frame, then acknowledged by
    // the frame interrupt once nothing reads this state any more.
    volatile bool detach;
    volatile bool detached;
    uint32_t next_order;
    uint32_t pixel_clock;
    uint32_t max_packets;
    uint64_t phase;
    int channel_frame;
    // Called in the background after every frame to refill banks, or NULL.
    volatile dvi_audio_refill_t refill;
    void *refill_data;
    // Fixed lines the command lists point at: ACR, audio InfoFrame and AVI
    // InfoFrame on lines 0-2, then ACR and silence on a blanking line [0] and
    // an active line [1].
    dvi_audio_line_t header_line[3];
    uint32_t header_length[3];
    dvi_audio_line_t acr_line[2];
    uint32_t acr_length[2];
    // One bit per line that carries an ACR packet.
    uint32_t acr_lines[(MODE_640_V_TOTAL_LINES + 31) / 32];
    dvi_audio_line_t silence_line[2];
    uint32_t silence_length[2];
} dvi_audio_state_t;

// Static so it can never be freed while it is queued.
static background_callback_t dvi_audio_refill_callback;

// Active line with the video preamble and guard band that audio mode needs.
static uint32_t dvi_audio_vactive_line640[] = {
    HSTX_CMD_RAW_REPEAT | MODE_640_H_FRONT_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | MODE_640_H_SYNC_WIDTH,
    SYNC_V1_H0,
    HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | (MODE_640_H_BACK_PORCH - 10),
    SYNC_V1_H1,
    HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | 8,
    TMDS_CTRL_11 | (TMDS_CTRL_01 << 10) | (TMDS_CTRL_00 << 20),
    HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | 2,
    0x2ccu | (0x133u << 10) | (0x2ccu << 20),
    HSTX_CMD_TMDS | MODE_640_H_ACTIVE_PIXELS
};

static uint32_t dvi_audio_build_line(uint32_t *p, const hstx_data_island_t *island, bool vsync, bool active) {
    uint32_t *start = p;
    uint32_t h0 = vsync ? SYNC_V0_H0 : SYNC_V1_H0;
    uint32_t h1 = vsync ? SYNC_V0_H1 : SYNC_V1_H1;
    uint32_t preamble = (h0 & 0x3ff) | (TMDS_CTRL_01 << 10) | (TMDS_CTRL_01 << 20);
    *p++ = HSTX_CMD_RAW_REPEAT | MODE_640_H_FRONT_PORCH;
    *p++ = h1;
    *p++ = HSTX_CMD_NOP;
    *p++ = HSTX_CMD_RAW_REPEAT | 8;
    *p++ = preamble;
    *p++ = HSTX_CMD_NOP;
    *p++ = HSTX_CMD_RAW | HSTX_DATA_ISLAND_WORDS;
    memcpy(p, island->words, sizeof(island->words));
    p += HSTX_DATA_ISLAND_WORDS;
    *p++ = HSTX_CMD_NOP;
    *p++ = HSTX_CMD_RAW_REPEAT | (MODE_640_H_SYNC_WIDTH - 8 - HSTX_DATA_ISLAND_WORDS);
    *p++ = h0;
    *p++ = HSTX_CMD_NOP;
    if (active) {
        *p++ = HSTX_CMD_RAW_REPEAT | (MODE_640_H_BACK_PORCH - 10);
        *p++ = h1;
        *p++ = HSTX_CMD_NOP;
        *p++ = HSTX_CMD_RAW_REPEAT | 8;
        *p++ = (h1 & 0x3ff) | (TMDS_CTRL_01 << 10) | (TMDS_CTRL_00 << 20);
        *p++ = HSTX_CMD_NOP;
        *p++ = HSTX_CMD_RAW_REPEAT | 2;
        *p++ = 0x2ccu | (0x133u << 10) | (0x2ccu << 20);
        *p++ = HSTX_CMD_TMDS | MODE_640_H_ACTIVE_PIXELS;
    } else {
        *p++ = HSTX_CMD_RAW_REPEAT | (MODE_640_H_BACK_PORCH + MODE_640_H_ACTIVE_PIXELS);
        *p++ = h1;
        *p++ = HSTX_CMD_NOP;
    }
    return p - start;
}

static void dvi_audio_init_lines(dvi_audio_state_t *audio) {
    hstx_packet_t packet;
    hstx_data_island_t island;
    uint32_t cts = ((uint64_t)audio->pixel_clock * 6144 + (128 * DVI_AUDIO_RATE / 2)) / (128 * DVI_AUDIO_RATE);
    hstx_packet_set_acr(&packet, 6144, cts);
    hstx_encode_data_island(&island, &packet, true, true);
    audio->header_length[0] = dvi_audio_build_line(audio->header_line[0], &island, true, false);
    hstx_encode_data_island(&island, &packet, false, true);
    for (int active = 0; active < 2; active++) {
        audio->acr_length[active] = dvi_audio_build_line(audio->acr_line[active], &island, false, active);
    }
    // Spread ACR packets over the frame at 128 * 48000 / 6144 = 1000 per
    // second, starting on line 0.
    uint64_t acr_phase = audio->pixel_clock;
    for (size_t line = 0; line < MODE_640_V_TOTAL_LINES; line++) {
        if (acr_phase >= audio->pixel_clock) {
            acr_phase -= audio->pixel_clock;
            audio->acr_lines[line / 32] |= 1u << (line % 32);
        }
        acr_phase += (uint64_t)(128 * DVI_AUDIO_RATE / 6144) * DVI_AUDIO_H_TOTAL;
    }
    hstx_packet_set_audio_infoframe(&packet, DVI_AUDIO_RATE, 2, 16);
    hstx_encode_data_island(&island, &packet, true, true);
    audio->header_length[1] = dvi_audio_build_line(audio->header_line[1], &island, true, false);
    // Unspecified VIC: preserve CircuitPython's existing 625-line timing.
    hstx_packet_set_avi_infoframe(&packet, 0, 0);
    hstx_encode_data_island(&island, &packet, false, true);
    audio->header_length[2] = dvi_audio_build_line(audio->header_line[2], &island, false, false);
    // Frame position 4 never sets the block start flag, as in pico_hdmi.
    audio_sample_t silence[4] = {{0}};
    hstx_packet_set_audio_samples(&packet, silence, 4, 4);
    hstx_encode_data_island(&island, &packet, false, true);
    for (int active = 0; active < 2; active++) {
        audio->silence_length[active] = dvi_audio_build_line(audio->silence_line[active], &island, false, active);
    }
}

static bool dvi_audio_acr_line(const dvi_audio_state_t *audio, size_t line) {
    return (audio->acr_lines[line / 32] >> (line % 32)) & 1;
}

// Returns true when a sample packet is due on this line. Header lines and
// ACR lines never carry sample packets.
static bool dvi_audio_packet_due(const dvi_audio_state_t *audio, uint64_t *phase, size_t line) {
    *phase += (uint64_t)DVI_AUDIO_RATE * DVI_AUDIO_H_TOTAL;
    if (line < 3 || dvi_audio_acr_line(audio, line) || *phase < (uint64_t)audio->pixel_clock * 4) {
        return false;
    }
    *phase -= (uint64_t)audio->pixel_clock * 4;
    return true;
}

static void dvi_audio_free_state(dvi_audio_state_t *audio) {
    if (audio->base_commands) {
        port_free(audio->base_commands);
    }
    for (int i = 0; i < DVI_AUDIO_BANKS; i++) {
        if (audio->bank[i].commands) {
            port_free(audio->bank[i].commands);
        }
        if (audio->bank[i].lines) {
            port_free(audio->bank[i].lines);
        }
    }
    port_free(audio);
}

// Frees the audio state. The caller makes sure DMA no longer reads it.
static void dvi_audio_free(picodvi_framebuffer_obj_t *self) {
    dvi_audio_state_t *audio = self->dvi_audio;
    if (!audio) {
        return;
    }
    self->dvi_audio = NULL;
    if (audio->refill) {
        picodvi_audioout_framebuffer_deinited(audio->refill_data);
    }
    dvi_audio_free_state(audio);
}

static void dvi_audio_allocate(picodvi_framebuffer_obj_t *self) {
    if (self->dvi_audio) {
        return;
    }
    dvi_audio_state_t *audio = port_malloc(sizeof(*audio), true);
    if (!audio) {
        m_malloc_fail(sizeof(*audio));
    }
    memset(audio, 0, sizeof(*audio));
    audio->active = -1;
    audio->pixel_clock = clock_get_hz(clk_hstx) / 5;
    // Room for the most packets one frame can need at this pixel clock.
    audio->max_packets = (uint64_t)MODE_640_V_TOTAL_LINES * DVI_AUDIO_RATE * DVI_AUDIO_H_TOTAL /
        ((uint64_t)audio->pixel_clock * 4) + 2;
    size_t commands_size = self->dma_commands_len * sizeof(uint32_t);
    size_t lines_size = audio->max_packets * sizeof(dvi_audio_line_t);
    // DMA reads all of these, so they must be in internal RAM.
    audio->base_commands = port_malloc(commands_size, true);
    bool ok = audio->base_commands != NULL;
    for (int i = 0; i < DVI_AUDIO_BANKS && ok; i++) {
        audio->bank[i].commands = port_malloc(commands_size, true);
        audio->bank[i].lines = port_malloc(lines_size, true);
        ok = audio->bank[i].commands != NULL && audio->bank[i].lines != NULL;
    }
    if (!ok) {
        dvi_audio_free_state(audio);
        m_malloc_fail(commands_size + lines_size);
    }
    dvi_audio_init_lines(audio);
    memcpy(audio->base_commands, self->dma_commands, commands_size);
    bool doubled = self->output_width != self->width;
    size_t command_size = doubled ? 4 : 2;
    size_t entry_offset = doubled ? 2 : 0;
    const size_t active_start = MODE_640_V_SYNC_WIDTH + MODE_640_V_BACK_PORCH;
    const size_t active_end = active_start + MODE_640_V_ACTIVE_LINES;
    size_t command_word = 0;
    uint64_t phase = 0;
    for (size_t line = 0; line < MODE_640_V_TOTAL_LINES; line++) {
        bool active = line >= active_start && line < active_end;
        uint32_t *entry = &audio->base_commands[command_word + entry_offset];
        if (dvi_audio_packet_due(audio, &phase, line)) {
            // Keep the audio link running with silence when no bank is ready.
            entry[0] = audio->silence_length[active];
            entry[1] = (uintptr_t)audio->silence_line[active];
        } else if (line < 3) {
            entry[0] = audio->header_length[line];
            entry[1] = (uintptr_t)audio->header_line[line];
        } else if (dvi_audio_acr_line(audio, line)) {
            entry[0] = audio->acr_length[active];
            entry[1] = (uintptr_t)audio->acr_line[active];
        } else if (active) {
            entry[0] = MP_ARRAY_SIZE(dvi_audio_vactive_line640);
            entry[1] = (uintptr_t)dvi_audio_vactive_line640;
        }
        command_word += command_size * (active ? 2 : 1);
    }
    __dmb();
    self->dvi_audio = audio;
}

// Looks the refill up again when it runs, so a refill cleared after the
// callback was queued is skipped. A deinitialized framebuffer's storage may
// already hold another display, so only the active framebuffer is touched.
static void dvi_audio_run_refill(void *data) {
    picodvi_framebuffer_obj_t *self = data;
    if (self != active_picodvi) {
        return;
    }
    dvi_audio_state_t *audio = self->dvi_audio;
    if (audio && audio->refill) {
        audio->refill(audio->refill_data);
    }
}

// Called from the frame interrupt after the next frame has started.
static void __not_in_flash_func(dvi_audio_frame_done)(void) {
    dvi_audio_state_t *audio = active_picodvi->dvi_audio;
    if (audio && audio->refill) {
        background_callback_add(&dvi_audio_refill_callback, dvi_audio_run_refill, active_picodvi);
    }
}

// Called at frame boundary only; old DMA frame has completed.
static uint32_t *__not_in_flash_func(dvi_audio_next_frame)(void) {
    dvi_audio_state_t *audio = active_picodvi->dvi_audio;
    if (!audio || audio->detached) {
        return active_picodvi->dma_commands;
    }
    if (audio->detach) {
        // From this frame on, DMA reads only the plain video commands.
        audio->active = -1;
        audio->detached = true;
        return active_picodvi->dma_commands;
    }
    if (audio->active >= 0) {
        audio->bank[audio->active].samples = 0;
        audio->active = -1;
    }
    int next = -1;
    for (int i = 0; i < DVI_AUDIO_BANKS; i++) {
        if (!audio->bank[i].ready) {
            continue;
        }
        if (next < 0 || (int32_t)(audio->bank[i].order - audio->bank[next].order) < 0) {
            next = i;
        }
    }
    if (next >= 0) {
        audio->active = next;
        audio->bank[next].ready = false;
        return audio->bank[next].commands;
    }
    // No bank ready: send a frame of silence rather than repeat the last one.
    return audio->base_commands;
}

size_t picodvi_framebuffer_audio_fill(picodvi_framebuffer_obj_t *self, const int16_t *samples, size_t available) {
    dvi_audio_state_t *audio = self->dvi_audio;
    if (!audio || !available) {
        return 0;
    }
    int bank_index = -1;
    // Read active + ready atomically: an IRQ can promote a ready bank to active
    // between these two observations. Once selected, an unready bank cannot be
    // taken by the ISR until this writer publishes it below.
    uint32_t irq_state = save_and_disable_interrupts();
    for (int i = 0; i < DVI_AUDIO_BANKS; i++) {
        if (audio->active != i && !audio->bank[i].ready) {
            bank_index = i;
            break;
        }
    }
    restore_interrupts(irq_state);
    if (bank_index < 0) {
        return 0;
    }
    dvi_audio_bank_t *bank = &audio->bank[bank_index];
    memcpy(bank->commands, audio->base_commands, self->dma_commands_len * sizeof(uint32_t));
    size_t command_word = 0;
    size_t samples_read = 0;
    unsigned packets = 0;
    bool doubled = self->output_width != self->width;
    size_t command_size = doubled ? 4 : 2;
    size_t entry_offset = doubled ? 2 : 0;
    const size_t active_start = MODE_640_V_SYNC_WIDTH + MODE_640_V_BACK_PORCH;
    const size_t active_end = active_start + MODE_640_V_ACTIVE_LINES;
    for (size_t line = 0; line < MODE_640_V_TOTAL_LINES; line++) {
        bool active = line >= active_start && line < active_end;
        uint32_t *entry = &bank->commands[command_word + entry_offset];
        if (!dvi_audio_packet_due(audio, &audio->phase, line)) {
            // The base list may have silence here, on a different cadence.
            if (entry[1] == (uintptr_t)audio->silence_line[active]) {
                if (active) {
                    entry[0] = MP_ARRAY_SIZE(dvi_audio_vactive_line640);
                    entry[1] = (uintptr_t)dvi_audio_vactive_line640;
                } else {
                    const uint32_t *plain = &self->dma_commands[command_word + entry_offset];
                    entry[0] = plain[0];
                    entry[1] = plain[1];
                }
            }
        } else if (samples_read >= available || packets >= audio->max_packets) {
            entry[0] = audio->silence_length[active];
            entry[1] = (uintptr_t)audio->silence_line[active];
        } else {
            audio_sample_t frames[4] = {{0}};
            size_t count = MIN(available - samples_read, 4);
            for (size_t j = 0; j < count; j++) {
                size_t k = samples_read + j;
                frames[j].left = samples[2 * k];
                frames[j].right = samples[2 * k + 1];
            }
            hstx_packet_t packet;
            hstx_data_island_t island;
            audio->channel_frame = hstx_packet_set_audio_samples(&packet, frames, count, audio->channel_frame);
            hstx_encode_data_island(&island, &packet, false, true);
            entry[0] = dvi_audio_build_line(bank->lines[packets], &island, false, active);
            entry[1] = (uintptr_t)bank->lines[packets];
            packets++;
            samples_read += count;
        }
        command_word += command_size * (active ? 2 : 1);
    }
    bank->samples = samples_read;
    bank->order = audio->next_order++;
    __dmb();
    bank->ready = true;
    return samples_read;
}

void picodvi_framebuffer_audio_reserve(picodvi_framebuffer_obj_t *self) {
    if (self->output_width != 640) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("Invalid %q"), MP_QSTR_framebuffer);
    }
    dvi_audio_allocate(self);
}

void picodvi_framebuffer_audio_release(picodvi_framebuffer_obj_t *self) {
    dvi_audio_state_t *audio = self->dvi_audio;
    if (!audio) {
        return;
    }
    audio->refill = NULL;
    audio->detach = true;
    // Wait for the frame interrupt to move DMA back to the video commands.
    // Without frame interrupts, DMA stops at the end of the current frame.
    for (int i = 0; i < 1000 && !audio->detached && active_picodvi == self; i++) {
        busy_wait_us(100);
    }
    uint32_t irq_state = save_and_disable_interrupts();
    self->dvi_audio = NULL;
    restore_interrupts(irq_state);
    dvi_audio_free_state(audio);
}

bool picodvi_framebuffer_audio_set_refill(picodvi_framebuffer_obj_t *self, void (*fun)(void *data), void *data) {
    dvi_audio_state_t *audio = self->dvi_audio;
    if (!audio || audio->refill) {
        return false;
    }
    uint32_t irq_state = save_and_disable_interrupts();
    audio->refill_data = data;
    audio->refill = fun;
    restore_interrupts(irq_state);
    return true;
}

size_t picodvi_framebuffer_audio_pending(picodvi_framebuffer_obj_t *self) {
    dvi_audio_state_t *audio = self->dvi_audio;
    if (!audio) {
        return 0;
    }
    uint32_t irq_state = save_and_disable_interrupts();
    size_t count = 0;
    for (int i = 0; i < DVI_AUDIO_BANKS; i++) {
        if (audio->bank[i].ready || audio->active == i) {
            count += audio->bank[i].samples;
        }
    }
    restore_interrupts(irq_state);
    return count;
}

void picodvi_framebuffer_audio_stop(picodvi_framebuffer_obj_t *self) {
    dvi_audio_state_t *audio = self->dvi_audio;
    if (!audio) {
        return;
    }
    uint32_t irq_state = save_and_disable_interrupts();
    for (int i = 0; i < DVI_AUDIO_BANKS; i++) {
        audio->bank[i].ready = false;
        if (audio->active != i) {
            audio->bank[i].samples = 0;
        }
    }
    restore_interrupts(irq_state);
}
