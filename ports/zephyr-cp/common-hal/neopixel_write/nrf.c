// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Dan Halbert for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Nordic nRF: play the NeoPixel waveform with a PWM instance's EasyDMA
// sequence, one 16-bit entry per data bit setting that bit's high time, as
// ports/nordic does. The PWM instance is allocated from iobroker for the
// duration of each write and released afterwards, with all four outputs
// disconnected, so the pin stays owned by the caller's DigitalInOut; OUT0 is
// connected to it through PSEL here and disconnected again before the
// release. Zephyr's PWM driver is compiled so the instance devices exist,
// but it is never initialized on them.

#if defined(CONFIG_SOC_FAMILY_NORDIC_NRF)

#include <errno.h>

#include <nrfx.h>
#include <hal/nrf_gpio.h>
#include <hal/nrf_pwm.h>

#include <zephyr/device.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <iobroker/iobroker.h>

#include "py/mpconfig.h"
#include "py/mpstate.h"
#include "py/runtime.h"

#include "common-hal/neopixel_write/__init__.h"

LOG_MODULE_REGISTER(neopixel_write);

// PWM counter runs at 16 MHz with a period of 20 counts (1.25 us per bit).
// Entries of the waveform sequence carry the polarity bit (0x8000) so the
// output idles low.
// WS2812 (rev A) timing, 0.35/0.7 us, works with everything tried so far,
// including the timing-sensitive https://adafru.it/5225.
#define MAGIC_T0H  (5UL | 0x8000)   // 0.3125 us high
#define MAGIC_T1H  (12UL | 0x8000)  // 0.75 us high
#define CTOPVAL    20UL             // 1.25 us period


// Waveform sequence: one entry per bit plus two end-of-sequence entries.
#define SEQUENCE_SIZE(num_bytes) ((num_bytes) * 8 * sizeof(uint16_t) + 2 * sizeof(uint16_t))
// Writes of up to this many RGB pixels (or 18 RGBW pixels: the buffer is
// sized in bytes as 3 * STACK_PIXELS) build the sequence on the stack. The
// status NeoPixel is written between VM instantiations, when the heap is not
// available, so it must never allocate; 24 pixels also covers Circuit
// Playground's 10 and the common 12- and 16-pixel rings without touching
// the heap. The stack array is about 1.2 KB.
#define STACK_PIXELS 24

// Heap buffer for larger writes, kept across calls and freed on reset.
static size_t sequence_heap_size = 0;

void neopixel_write_reset(void) {
    MP_STATE_VM(neopixel_write_sequence_heap) = NULL;
    sequence_heap_size = 0;
}

// Fallback when every PWM instance is busy: bit-bang the waveform with
// interrupts locked, timed by the DWT cycle counter. The cycle counts are
// ports/nordic's 64 MHz values (71 per bit, 18 high for a zero, 41 high for
// a one) expressed as dividers of SystemCoreClock: a 1.11 us bit interval,
// 0.28 us and 0.64 us high, which the loop's own overhead stretches by a few
// cycles each.
//
// The Bluetooth controller's radio interrupts are zero-latency
// (CONFIG_ZERO_LATENCY_IRQS, port Kconfig) so they still preempt the locked
// section. A preempted bit runs long and may have latched garbage, so, in
// the spirit of ports/nordic's resend after SoftDevice preemption, every bit
// period is checked and the frame is resent after the strip's reset period
// when any bit ran more than twice its nominal length. The check is per bit
// rather than per frame because a few tens of microseconds of interrupt is
// a small fraction of a long frame but a whole latch period for the strip.
#define BITBANG_MAX_ATTEMPTS 8

static void bitbang_send(uint32_t pin, const uint8_t *pixels, uint32_t num_bytes) {
    uint32_t interval = SystemCoreClock / 900000;
    uint32_t t0 = SystemCoreClock / 3500000;
    uint32_t t1 = SystemCoreClock / 1560000;
    uint32_t limit = interval * 2;

    uint32_t decoded_pin = pin;
    NRF_GPIO_Type *port = nrf_gpio_pin_port_decode(&decoded_pin);
    uint32_t mask = 1UL << decoded_pin;

    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    for (int attempt = 0; attempt < BITBANG_MAX_ATTEMPTS; attempt++) {
        bool preempted = false;
        unsigned int key = irq_lock();
        for (uint32_t n = 0; n < num_bytes; n++) {
            uint8_t pix = pixels[n];
            for (uint8_t bit_mask = 0x80; bit_mask > 0; bit_mask >>= 1) {
                uint32_t high = (pix & bit_mask) ? t1 : t0;
                uint32_t start = DWT->CYCCNT;
                port->OUTSET = mask;
                while (DWT->CYCCNT - start < high) {
                }
                port->OUTCLR = mask;
                while (DWT->CYCCNT - start < interval) {
                }
                if (DWT->CYCCNT - start > limit) {
                    preempted = true;
                }
            }
        }
        irq_unlock(key);
        if (!preempted) {
            return;
        }
        // Let the strip latch whatever it got, then send the whole frame again.
        k_busy_wait(300);
    }
    LOG_WRN("bit-bang frame preempted %d times; giving up", BITBANG_MAX_ATTEMPTS);
}

void neopixel_write_send(const digitalio_digitalinout_obj_t *gpio,
    const uint8_t *pixels, uint32_t num_bytes) {
    // iobroker's global pin number (port * 32 + pin) is also the nrfx pin
    // number that PSEL and the GPIO HAL take.
    uint16_t pin;
    if (iobroker_package_pin_soc_pad(gpio->pin->package_pin, &pin) < 0) {
        LOG_WRN("pin is not in the package pin map; write skipped");
        return;
    }

    const struct device *dev = NULL;
    int ret = iobroker_pwm_allocate(IOBROKER_NO_PIN, IOBROKER_NO_PIN,
        IOBROKER_NO_PIN, IOBROKER_NO_PIN, &dev);
    if (ret == -ENODEV) {
        // Every instance is busy (pwmio or something else is holding them all):
        // bit-bang instead. That locks ordinary interrupts for the frame; the
        // zero-latency radio interrupt can still preempt it and glitch the
        // output, which bitbang_send() detects and resends.
        bitbang_send(pin, pixels, num_bytes);
        return;
    }
    if (ret < 0) {
        LOG_WRN("PWM allocation failed (%d); write skipped", ret);
        return;
    }
    uint32_t pwm_addr;
    if (iobroker_instance_reg_addr(dev, &pwm_addr) < 0) {
        LOG_WRN("%s has no register address; write skipped", dev->name);
        (void)iobroker_release(dev);
        return;
    }
    NRF_PWM_Type *pwm = (NRF_PWM_Type *)pwm_addr;

    // Sequence buffer: up to 3 * STACK_PIXELS bytes of pixel data fit the
    // stack array (uint32_t so that EasyDMA gets an aligned buffer;
    // SEQUENCE_SIZE is a multiple of 4). Longer strips use a heap buffer that
    // is kept between writes and grown when a longer strip appears. Only user
    // code with such a strip reaches the heap path, with the VM running, so
    // the raising allocator is fine here.
    size_t sequence_size = SEQUENCE_SIZE(num_bytes);
    uint32_t stack_sequence[SEQUENCE_SIZE(3 * STACK_PIXELS) / sizeof(uint32_t)];
    uint16_t *sequence;
    if (sequence_size <= sizeof(stack_sequence)) {
        sequence = (uint16_t *)stack_sequence;
    } else {
        if (sequence_heap_size < sequence_size) {
            MP_STATE_VM(neopixel_write_sequence_heap) =
                m_realloc(MP_STATE_VM(neopixel_write_sequence_heap), sequence_size);
            sequence_heap_size = sequence_size;
        }
        sequence = MP_STATE_VM(neopixel_write_sequence_heap);
    }

    size_t pos = 0;
    for (uint32_t n = 0; n < num_bytes; n++) {
        uint8_t pix = pixels[n];
        for (uint8_t mask = 0x80; mask > 0; mask >>= 1) {
            sequence[pos++] = (pix & mask) ? MAGIC_T1H : MAGIC_T0H;
        }
    }
    // Two zero-width entries end the waveform sequence with the line low.
    sequence[pos++] = 0 | 0x8000;
    sequence[pos++] = 0 | 0x8000;

    nrf_pwm_configure(pwm, NRF_PWM_CLK_16MHz, NRF_PWM_MODE_UP, CTOPVAL);
    nrf_pwm_loop_set(pwm, 0);
    nrf_pwm_decoder_set(pwm, NRF_PWM_LOAD_COMMON, NRF_PWM_STEP_AUTO);
    nrf_pwm_seq_ptr_set(pwm, 0, sequence);
    nrf_pwm_seq_cnt_set(pwm, 0, sequence_size / sizeof(uint16_t));
    nrf_pwm_seq_refresh_set(pwm, 0, 0);
    nrf_pwm_seq_end_delay_set(pwm, 0, 0);

    // PSEL must be set before the PWM is enabled.
    nrf_pwm_pins_set(pwm, (uint32_t[]) {pin, NRF_PWM_PIN_NOT_CONNECTED,
                                        NRF_PWM_PIN_NOT_CONNECTED, NRF_PWM_PIN_NOT_CONNECTED});
    nrf_pwm_enable(pwm);

    nrf_pwm_event_clear(pwm, NRF_PWM_EVENT_SEQEND0);
    nrf_pwm_task_trigger(pwm, NRF_PWM_TASK_SEQSTART0);
    while (!nrf_pwm_event_check(pwm, NRF_PWM_EVENT_SEQEND0)) {
        RUN_BACKGROUND_TASKS;
    }
    nrf_pwm_event_clear(pwm, NRF_PWM_EVENT_SEQEND0);

    nrf_pwm_disable(pwm);
    // Hand the pad back to the GPIO output the DigitalInOut configured.
    nrf_pwm_pins_set(pwm, (uint32_t[]) {NRF_PWM_PIN_NOT_CONNECTED, NRF_PWM_PIN_NOT_CONNECTED,
                                        NRF_PWM_PIN_NOT_CONNECTED, NRF_PWM_PIN_NOT_CONNECTED});

    (void)iobroker_release(dev);
}

MP_REGISTER_ROOT_POINTER(uint16_t * neopixel_write_sequence_heap);

#endif // CONFIG_SOC_FAMILY_NORDIC_NRF
