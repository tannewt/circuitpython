// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2024 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "supervisor/port.h"

#include <zephyr/autoconf.h>
#include <zephyr/kernel.h>

safe_mode_t port_init(void) {
    return SAFE_MODE_NONE;
}

// Reset the microcontroller completely.
void reset_cpu(void) {
    while (true) {
    }
}

void reset_port(void) {

}

void port_wake_main_task(void) {
}

void port_wake_main_task_from_isr(void) {
}

void port_yield(void) {
}

void port_boot_info(void) {
}

void port_heap_init(void) {
}

// Get stack limit address
uint32_t *port_stack_get_limit(void) {
    return NULL;
}

// Get stack top address
uint32_t *port_stack_get_top(void) {
    return NULL;
}

// Get heap bottom address
uint32_t *port_heap_get_bottom(void) {
    return NULL;
}

// Get heap top address
uint32_t *port_heap_get_top(void) {
    return NULL;
}

// Save and retrieve a word from memory that is preserved over reset. Used for safe mode.
void port_set_saved_word(uint32_t) {

}
uint32_t port_get_saved_word(void) {
    return 0;
}

uint64_t port_get_raw_ticks(uint8_t *subticks) {
    int64_t uptime = k_uptime_ticks() * 32768 / CONFIG_SYS_CLOCK_TICKS_PER_SEC;
    if (subticks != NULL) {
        *subticks = uptime % 32;
    }
    return uptime / 32;
}

// Enable 1/1024 second tick.
void port_enable_tick(void) {

}

// Disable 1/1024 second tick.
void port_disable_tick(void) {

}

void port_interrupt_after_ticks(uint32_t ticks) {
}

void port_idle_until_interrupt(void) {
}

void *port_malloc(size_t size, bool dma_capable) {
    void *block = k_malloc(size);
    return block;
}

void port_free(void *ptr) {
    k_free(ptr);
}

void *port_realloc(void *ptr, size_t size) {
    return k_realloc(ptr, size);
}
