/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2021 Dan Halbert for Adafruit Industries
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

#ifndef SHARED_MODULE_KEYPAD_H
#define SHARED_MODULE_KEYPAD_H

#include "py/obj.h"
#include "supervisor/shared/lock.h"

typedef struct _keypad_scanner_funcs_t {
    void (*scan_now)(void *self_in, mp_obj_t timestamp);
    size_t (*get_key_count)(void *self_in);
} keypad_scanner_funcs_t;

// All scanners must begin with these common fields.
// This is an ad hoc "superclass" struct for scanners, though they do
// not actually have a superclass relationship.
#define KEYPAD_SCANNER_COMMON_FIELDS \
    mp_obj_base_t base; \
    struct _keypad_scanner_obj_t *next; \
    keypad_scanner_funcs_t *funcs; \
    uint64_t next_scan_ticks; \
    int8_t *debounce_counter; \
    struct _keypad_eventqueue_obj_t *events; \
    mp_uint_t interval_ticks; \
    uint8_t debounce_threshold; \
    bool never_reset

typedef struct _keypad_scanner_obj_t {
    KEYPAD_SCANNER_COMMON_FIELDS;
} keypad_scanner_obj_t;

extern supervisor_lock_t keypad_scanners_linked_list_lock;

void keypad_tick(void);
void keypad_reset(void);

void keypad_register_scanner(keypad_scanner_obj_t *scanner);
void keypad_deregister_scanner(keypad_scanner_obj_t *scanner);
void keypad_construct_common(keypad_scanner_obj_t *scanner, mp_float_t interval, size_t max_events, uint8_t debounce_cycles);
bool keypad_debounce(keypad_scanner_obj_t *self, mp_uint_t key_number, bool current);
void keypad_never_reset(keypad_scanner_obj_t *self);

size_t common_hal_keypad_generic_get_key_count(void *scanner);
void common_hal_keypad_deinit_core(void *scanner);

#endif // SHARED_MODULE_KEYPAD_H
