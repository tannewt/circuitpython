/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2016 Scott Shawcroft for Adafruit Industries
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "hpl_gpio.h"

#include "py/mphal.h"

#include "shared-bindings/neopixel_write/__init__.h"

#include "supervisor/port.h"

#if defined(SAME54)
#include "hri/hri_cmcc_e54.h"
#include "hri/hri_nvmctrl_e54.h"
#elif defined(SAME51)
#include "hri/hri_cmcc_e51.h"
#include "hri/hri_nvmctrl_e51.h"
#elif defined(SAMD51)
#include "hri/hri_cmcc_d51.h"
#include "hri/hri_nvmctrl_d51.h"
#endif

__attribute__((naked,noinline,aligned(16)))
static void neopixel_send_buffer_core(volatile uint32_t *clraddr, uint32_t pinMask,
    const uint8_t *ptr, int numBytes);

// The SAMD21 timing loop durations below are approximate,
// because the other instructions take significant time.

static void neopixel_send_buffer_core(volatile uint32_t *clraddr, uint32_t pinMask,
    const uint8_t *ptr, int numBytes) {
    asm volatile ("        push    {r4, r5, r6, lr};\n"
        "        add     r3, r2, r3;\n"
        "loopLoad:\n"
        "        ldrb r5, [r2, #0];\n"          // r5 := *ptr
        "        adds  r2, r2, #1;\n"                // ptr++
        "        movs    r4, #128;\n"           // r4-mask, 0x80

        "loopBit:\n"
        "        str r1, [r0, #4];\n"                             // set
        #ifdef SAMD21
        "        movs r6, #2; d2: subs r6, #1; bne d2;\n"          // 248 ns high (entire T0H or start T1H)
        #endif
        #ifdef SAM_D5X_E5X
        "        movs r6, #11; d2: subs r6, #1; bne d2;\n"        // 300 ns high (entire T0H or start T1H)
        #endif
        "        tst r4, r5;\n"                                   // mask&r5
        "        bne skipclr;\n"
        "        str r1, [r0, #0];\n"          // clr

        "skipclr:\n"
        #ifdef SAMD21
        "        movs r6, #7; d0: subs r6, #1; bne d0;\n"          // 772 ns low or high (start T0L or end T1H)
        #endif
        #ifdef SAM_D5X_E5X
        "        movs r6, #15; d0: subs r6, #1; bne d0;\n"        // 388 ns low or high (start T0L or end T1H)
        #endif
        "        str r1, [r0, #0];\n"            // clr (possibly again, doesn't matter)

        #ifdef SAMD21
        "        asrs     r4, r4, #1;\n"          // mask >>= 1
        #endif
        #ifdef SAM_D5X_E5X
        "        asrs     r4, r4, #1;\n"          // mask >>= 1
        #endif
        "        beq     nextbyte;\n"
        "        uxtb    r4, r4;\n"
        #ifdef SAMD21
        "        movs r6, #5; d1: subs r6, #1; bne d1;\n"          // 496 ns (end TOL or entire T1L)
        #endif
        #ifdef SAM_D5X_E5X
        "        movs r6, #20; d1: subs r6, #1; bne d1;\n"        // 548 ns (end TOL or entire T1L)
        #endif
        "        b       loopBit;\n"

        "nextbyte:\n"
        #ifdef SAMD21
        "        movs r6, #1; d3: subs r6, #1; bne d3;\n"          // 60 ns (end TOL or entire T1L)
                                                                   // other instructions add more delay
        #endif
        #ifdef SAM_D5X_E5X
        "        movs r6, #18; d3: subs r6, #1; bne d3;\n"        // extra for 936 ns total (byte end T0L or entire T1L)
        #endif
        "        cmp r2, r3;\n"
        "        bcs neopixel_stop;\n"
        "        b loopLoad;\n"
        "neopixel_stop:\n"
        "        pop {r4, r5, r6, pc};\n"
        "");
}

STATIC uint64_t next_start_raw_ticks = 0;

void common_hal_neopixel_write(const digitalio_digitalinout_obj_t *digitalinout, uint8_t *pixels, uint32_t numBytes) {
    // This is adapted directly from the Adafruit NeoPixel library SAMD21G18A code:
    // https://github.com/adafruit/Adafruit_NeoPixel/blob/master/Adafruit_NeoPixel.cpp
    // and the asm version from https://github.com/microsoft/uf2-samdx1/blob/master/inc/neopixel.h
    uint32_t pinMask;
    PortGroup *port;

    // Wait to make sure we don't append onto the last transmission. This should only be a tick or
    // two.
    while (port_get_raw_ticks(NULL) < next_start_raw_ticks) {
    }

    // Turn off interrupts of any kind during timing-sensitive code.
    mp_hal_disable_all_interrupts();

    uint32_t pin = digitalinout->pin->number;
    port = &PORT->Group[GPIO_PORT(pin)];      // Convert GPIO # to port register
    pinMask = (1UL << (pin % 32));   // From port_pin_set_output_level ASF code.
    volatile uint32_t *clr = &(port->OUTCLR.reg);
    neopixel_send_buffer_core(clr, pinMask, pixels, numBytes);

    // Update the next start.
    next_start_raw_ticks = port_get_raw_ticks(NULL) + 4;

    // Turn on interrupts after timing-sensitive code.
    mp_hal_enable_all_interrupts();

}
