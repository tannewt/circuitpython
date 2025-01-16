// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2024 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "supervisor/port.h"

#include <zephyr/autoconf.h>
#include <zephyr/kernel.h>

#include "lib/tlsf/tlsf.h"

static tlsf_t heap;

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

void reset_to_bootloader(void) {
}

void port_wake_main_task(void) {
}

void port_wake_main_task_from_isr(void) {
}

void port_yield(void) {
}

void port_boot_info(void) {
}

// Get stack limit address
uint32_t *port_stack_get_limit(void) {
    return k_current_get()->stack_info.start;
}

// Get stack top address
uint32_t *port_stack_get_top(void) {
    _thread_stack_info_t stack_info = k_current_get()->stack_info;

    return stack_info.start + stack_info.size - stack_info.delta;
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

extern uint32_t z_mapped_end;

// Zephyr doesn't maintain one multi-heap. So, make our own using TLSF.
void port_heap_init(void) {
    #if DT_CHOSEN_zephyr_sram_EXISTS
    uint32_t *heap_bottom = &z_mapped_end;
    uint32_t *heap_top = (uint32_t *)(DT_REG_ADDR(DT_CHOSEN(zephyr_sram)) + DT_REG_SIZE(DT_CHOSEN(zephyr_sram)));
    size_t size = (heap_top - heap_bottom) * sizeof(uint32_t);
    printk("Init heap at %p - %p with size %d\n", heap_bottom, heap_top, size);
    heap = tlsf_create_with_pool(heap_bottom, size, size);
    #else
    #error "No SRAM!"
    #endif
}

void *port_malloc(size_t size, bool dma_capable) {
    void *block = tlsf_malloc(heap, size);
    return block;
}

void port_free(void *ptr) {
    tlsf_free(heap, ptr);
}

void *port_realloc(void *ptr, size_t size) {
    return tlsf_realloc(heap, ptr, size);
}

static bool max_size_walker(void *ptr, size_t size, int used, void *user) {
    size_t *max_size = (size_t *)user;
    if (!used && *max_size < size) {
        *max_size = size;
    }
    return true;
}

size_t port_heap_get_largest_free_size(void) {
    size_t max_size = 0;
    tlsf_walk_pool(tlsf_get_pool(heap), max_size_walker, &max_size);
    // IDF does this. Not sure why.
    return tlsf_fit_size(heap, max_size);
}
