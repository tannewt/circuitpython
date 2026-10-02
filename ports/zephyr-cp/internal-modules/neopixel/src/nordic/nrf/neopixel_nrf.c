// neopixel: nRF transmit.
//
// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Adafruit Industries LLC
//
// SPDX-License-Identifier: MIT

// <vendor>/<soc> = nordic/nrf: play the NeoPixel waveform with a PWM
// instance's EasyDMA sequence, one 16-bit entry per data bit setting that
// bit's high time, as CircuitPython's ports/nordic does. The PWM instance is
// allocated from iobroker for the duration of each transfer and released
// afterwards, with its outputs left unrouted, so the pin stays owned by the
// caller; OUT0 is connected to it through PSEL here and disconnected again
// before the release. Zephyr's PWM driver is compiled so the instance
// devices exist, but it is never initialized on them. When every instance
// is busy, or none can reach the pad, the waveform is bit-banged instead,
// except on nRF54L P0, whose GPIO is too slow for it.

#include <errno.h>
#include <stdbool.h>

#include <nrfx.h>
#include <hal/nrf_gpio.h>
#include <hal/nrf_pwm.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <iobroker/iobroker.h>
#include <neopixel/neopixel.h>

LOG_MODULE_REGISTER(neopixel, CONFIG_LOG_DEFAULT_LEVEL);

// PWM counter runs at 16 MHz with a period of 20 counts (1.25 us per bit).
// Entries of the waveform sequence carry the polarity bit (0x8000) so the
// output idles low.
// WS2812 (rev A) timing, 0.35/0.7 us, works with everything tried so far,
// including the timing-sensitive https://adafru.it/5225.
#define MAGIC_T0H  (5UL | 0x8000)   // 0.3125 us high
#define MAGIC_T1H  (12UL | 0x8000)  // 0.75 us high
#define CTOPVAL    20UL             // 1.25 us period

// PWM instances enabled in the devicetree, with their register blocks. The
// iobroker hands out the Zephyr device; the HAL uses the registers. Zephyr
// has no public way to get a device's register address, but the devicetree
// knows it.
typedef struct {
    const struct device *dev;
    NRF_PWM_Type *regs;
} pwm_regs_t;

#define PWM_REGS_ENTRY(node_id) \
    { .dev = DEVICE_DT_GET(node_id), .regs = (NRF_PWM_Type *)DT_REG_ADDR(node_id) },

static const pwm_regs_t pwm_regs[] = {
    DT_FOREACH_STATUS_OKAY(nordic_nrf_pwm, PWM_REGS_ENTRY)
};

static NRF_PWM_Type *pwm_regs_for(const struct device *dev) {
    for (size_t i = 0; i < ARRAY_SIZE(pwm_regs); i++) {
        if (pwm_regs[i].dev == dev) {
            return pwm_regs[i].regs;
        }
    }
    return NULL;
}

// Fallback when no PWM instance is free or none can reach the pad: bit-bang the waveform with
// interrupts locked, timed by the DWT cycle counter. The cycle counts are
// ports/nordic's 64 MHz values (71 per bit, 18 high for a zero, 41 high for
// a one) expressed as dividers of SystemCoreClock: a 1.11 us bit interval,
// 0.28 us and 0.64 us high, which the loop's own overhead stretches by a few
// cycles each.
//
// The Bluetooth controller's radio interrupts are zero-latency
// (CONFIG_ZERO_LATENCY_IRQS) so they still preempt the locked section. A
// preempted bit runs long and may have latched garbage, so, in the spirit of
// ports/nordic's resend after SoftDevice preemption, every bit period is
// checked and the frame is resent after the strip's reset period when any
// bit ran more than twice its nominal length. The check is per bit rather
// than per frame because a few tens of microseconds of interrupt is a small
// fraction of a long frame but a whole latch period for the strip.
//
// This is the vendor-specific register poking that Zephyr's proposed fast
// GPIO API (https://github.com/zephyrproject-rtos/zephyr/issues/106831)
// would replace; switch to that API once it lands.
#define BITBANG_MAX_ATTEMPTS 8

static int bitbang_send(uint32_t pin, const uint8_t *pixels, size_t num_bytes) {
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
        for (size_t n = 0; n < num_bytes; n++) {
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
            return 0;
        }
        // Let the strip latch whatever it got, then send the whole frame again.
        k_busy_wait(300);
    }
    LOG_WRN("bit-bang frame preempted %d times; giving up", BITBANG_MAX_ATTEMPTS);
    return -EIO;
}

static int pwm_send(NRF_PWM_Type *pwm, uint32_t pin, const uint8_t *pixels, size_t num_bytes,
    uint16_t *sequence) {
    size_t pos = 0;
    for (size_t n = 0; n < num_bytes; n++) {
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
    nrf_pwm_seq_cnt_set(pwm, 0, pos);
    nrf_pwm_seq_refresh_set(pwm, 0, 0);
    nrf_pwm_seq_end_delay_set(pwm, 0, 0);

    // PSEL must be set before the PWM is enabled.
    nrf_pwm_pins_set(pwm, (uint32_t[]) {pin, NRF_PWM_PIN_NOT_CONNECTED,
                                        NRF_PWM_PIN_NOT_CONNECTED, NRF_PWM_PIN_NOT_CONNECTED});
    nrf_pwm_enable(pwm);

    nrf_pwm_event_clear(pwm, NRF_PWM_EVENT_SEQEND0);
    nrf_pwm_task_trigger(pwm, NRF_PWM_TASK_SEQSTART0);
    while (!nrf_pwm_event_check(pwm, NRF_PWM_EVENT_SEQEND0)) {
        k_yield();
    }
    nrf_pwm_event_clear(pwm, NRF_PWM_EVENT_SEQEND0);

    nrf_pwm_disable(pwm);
    // Hand the pad back to the GPIO output the caller configured.
    nrf_pwm_pins_set(pwm, (uint32_t[]) {NRF_PWM_PIN_NOT_CONNECTED, NRF_PWM_PIN_NOT_CONNECTED,
                                        NRF_PWM_PIN_NOT_CONNECTED, NRF_PWM_PIN_NOT_CONNECTED});
    return 0;
}

// Whether the CPU can bit-bang the waveform on a pad. On nRF54L the P0 GPIO
// port sits in the low-power (LP) domain, which runs at 16 MHz asynchronously
// to the CPU: a register write to it stalls about 0.5 us (65 cycles at
// 128 MHz, measured on the nRF54LM20A), longer than a zero bit's 0.3 us high
// time, so every bit would read as a one. P1-P3 (PERI and MCU domains) take
// 7 to 26 cycles. The domain is in bits 18-20 of the register address, the
// same decoding as nrfx_gppi_domain_id_get(); LP is 3.
#define NRF54L_DOMAIN_LP 3

static bool pad_can_bitbang(uint16_t pad) {
    #if defined(CONFIG_SOC_SERIES_NRF54L)
    uint32_t pin_number = pad;
    uintptr_t port_addr = (uintptr_t)nrf_gpio_pin_port_decode(&pin_number);
    return ((port_addr >> 18) & 0x7) - 1 != NRF54L_DOMAIN_LP;
    #else
    (void)pad;
    return true;
    #endif
}

int neopixel_send(package_pin_t pin, const uint8_t *pixels, size_t num_bytes,
    void *pattern_buffer, size_t pattern_buffer_size) {
    // iobroker's global GPIO number (port * 32 + pin) is also the nrfx pin
    // number that PSEL and the GPIO HAL take.
    uint16_t pad;
    int ret = iobroker_package_pin_gpio_pad(pin, &pad);
    if (ret < 0 || pad == IOBROKER_NO_PIN) {
        return -EINVAL;
    }
    if (pattern_buffer_size < NEOPIXEL_PATTERN_BUFFER_SIZE(num_bytes) ||
        ((uintptr_t)pattern_buffer & 3) != 0) {
        return -EINVAL;
    }

    const struct device *dev = NULL;
    ret = iobroker_pwm_allocate_unrouted(pin, &dev);
    if (ret == -ENODEV || (ret == -EINVAL && pad_can_bitbang(pad))) {
        // Every instance is busy (pwmio or something else is holding them all),
        // or none can reach this pad (nRF54L PWMs drive only their own power
        // domain's pads) but the CPU can drive it fast enough: bit-bang
        // instead. That locks ordinary interrupts for the frame; the
        // zero-latency radio interrupt can still preempt it and glitch the
        // output, which bitbang_send() detects and resends. A pad that neither
        // can drive keeps -EINVAL.
        return bitbang_send(pad, pixels, num_bytes);
    }
    if (ret < 0) {
        return ret;
    }
    NRF_PWM_Type *pwm = pwm_regs_for(dev);
    if (pwm == NULL) {
        LOG_WRN("%s has no register entry", dev->name);
        (void)iobroker_release(dev);
        return -ENODEV;
    }

    ret = pwm_send(pwm, pad, pixels, num_bytes, pattern_buffer);
    (void)iobroker_release(dev);
    return ret;
}
