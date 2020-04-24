/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2017 Scott Shawcroft for Adafruit Industries
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

#include "tick.h"

#include "supervisor/shared/autoreload.h"
#include "supervisor/filesystem.h"
#include "supervisor/shared/tick.h"
#include "shared-module/gamepad/__init__.h"
#include "shared-bindings/microcontroller/Processor.h"

// Global millisecond tick count
// volatile uint64_t ticks_ms = 0;

__attribute__((section(".ramtext")))
void SysTick_Handler(void) {
    supervisor_tick();
}

void tick_init() {
}

void tick_delay(uint32_t us) {
}

// us counts down!
void current_tick(uint64_t* ms, uint32_t* us_until_ms) {
}

void wait_until(uint64_t ms, uint32_t us_until_ms) {
}
