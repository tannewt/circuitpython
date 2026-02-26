// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2024 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "supervisor/port.h"

#include "mpconfigboard.h"
#include "supervisor/shared/tick.h"

#if CIRCUITPY_BLEIO
#include "shared-bindings/_bleio/__init__.h"
#endif

#include <zephyr/autoconf.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>

#if defined(CONFIG_ARCH_POSIX)
#include <limits.h>
#include <stdlib.h>

#include "cmdline.h"
#include "posix_board_if.h"
#include "posix_native_task.h"
#endif

#include "lib/tlsf/tlsf.h"
#include <zephyr/device.h>

static tlsf_t heap;

// Auto generated in pins.c
extern const struct device *const rams[];
extern const uint32_t *const ram_bounds[];
extern const size_t circuitpy_max_ram_size;

static pool_t pools[CIRCUITPY_RAM_DEVICE_COUNT];
static uint8_t valid_pool_count = 0;
static bool zephyr_malloc_active = false;
static void *zephyr_malloc_top = NULL;
static void *zephyr_malloc_bottom = NULL;

static K_EVENT_DEFINE(main_needed);

static struct k_timer tick_timer;

#if defined(CONFIG_ARCH_POSIX)
// Number of VM runs before exiting.
// <= 0 means run forever.
// INT32_MAX means option was not provided.
static int32_t native_sim_vm_runs = INT32_MAX;
static uint32_t native_sim_reset_port_count = 0;
static bool native_sim_enable_usb = false;

static struct args_struct_t native_sim_reset_port_args[] = {
    {
        .option = "vm-runs",
        .name = "count",
        .type = 'i',
        .dest = &native_sim_vm_runs,
        .descript = "Exit native_sim after this many VM runs. "
            "Example: --vm-runs=2"
    },
    {
        .option = "enable-usb",
        .is_switch = true,
        .type = 'b',
        .dest = &native_sim_enable_usb,
        .descript = "Enable USB stack (USB/IP) in native_sim."
    },
    ARG_TABLE_ENDMARKER
};

static void native_sim_register_cmdline_opts(void) {
    native_add_command_line_opts(native_sim_reset_port_args);
}

NATIVE_TASK(native_sim_register_cmdline_opts, PRE_BOOT_1, 0);
#endif

bool native_sim_usb_enabled(void) {
    #if defined(CONFIG_ARCH_POSIX)
    return native_sim_enable_usb;
    #else
    return true;
    #endif
}

static void _tick_function(struct k_timer *timer_id) {
    supervisor_tick();
}

safe_mode_t port_init(void) {
    k_timer_init(&tick_timer, _tick_function, NULL);
    return SAFE_MODE_NONE;
}

// Reset the microcontroller completely.
void reset_cpu(void) {
    // Try a warm reboot first. It won't return if it works but isn't always
    // implemented.
    sys_reboot(SYS_REBOOT_WARM);
    sys_reboot(SYS_REBOOT_COLD);
    printk("Failed to reboot. Looping.\n");
    while (true) {
    }
}

void reset_port(void) {
    #if CIRCUITPY_BLEIO
    bleio_reset();
    #endif

    #if defined(CONFIG_ARCH_POSIX)
    native_sim_reset_port_count++;
    if (native_sim_vm_runs != INT32_MAX &&
        native_sim_vm_runs > 0 &&
        native_sim_reset_port_count >= (uint32_t)(native_sim_vm_runs + 1)) {
        printk("posix: exiting after %d VM runs\n", native_sim_vm_runs);
        posix_exit(0);
    }
    #endif
}

void reset_to_bootloader(void) {
    reset_cpu();
}

void port_wake_main_task(void) {
    k_event_set(&main_needed, 1);
}

void port_wake_main_task_from_isr(void) {
    k_event_set(&main_needed, 1);
}

void port_yield(void) {
    k_yield();
}

void port_boot_info(void) {
}

// Get stack limit address
uint32_t *port_stack_get_limit(void) {
    return (uint32_t *)k_current_get()->stack_info.start;
}

// Get stack top address
uint32_t *port_stack_get_top(void) {
    _thread_stack_info_t stack_info = k_current_get()->stack_info;

    return (uint32_t *)(stack_info.start + stack_info.size - stack_info.delta);
}

// Save and retrieve a word from memory that is preserved over reset. Used for safe mode.
void port_set_saved_word(uint32_t) {

}
uint32_t port_get_saved_word(void) {
    return 0;
}

uint64_t port_get_raw_ticks(uint8_t *subticks) {
    // Make sure time advances in the simulator.
    #if defined(CONFIG_ARCH_POSIX)
    k_busy_wait(100);
    #endif
    int64_t uptime = k_uptime_ticks() * 32768 / CONFIG_SYS_CLOCK_TICKS_PER_SEC;
    if (subticks != NULL) {
        *subticks = uptime % 32;
    }
    return uptime / 32;
}

// Enable 1/1024 second tick.
void port_enable_tick(void) {
    k_timer_start(&tick_timer, K_USEC(1000000 / 1024), K_USEC(1000000 / 1024));
}

// Disable 1/1024 second tick.
void port_disable_tick(void) {
    k_timer_stop(&tick_timer);
}

static k_timeout_t next_timeout;
static k_timepoint_t next_timepoint;

void port_interrupt_after_ticks(uint32_t ticks) {
    size_t zephyr_ticks = ticks * CONFIG_SYS_CLOCK_TICKS_PER_SEC / 1024;
    k_timeout_t maybe_next_timeout = K_TIMEOUT_ABS_TICKS(k_uptime_ticks() + zephyr_ticks);
    k_timepoint_t maybe_next_timepoint = sys_timepoint_calc(maybe_next_timeout);
    if (sys_timepoint_cmp(maybe_next_timepoint, next_timepoint) < 0) {
        next_timeout = maybe_next_timeout;
        next_timepoint = maybe_next_timepoint;
    }
}

void port_idle_until_interrupt(void) {
    k_event_wait(&main_needed, 0xffffffff, true, next_timeout);
    next_timeout = K_FOREVER;
    next_timepoint = sys_timepoint_calc(next_timeout);
}

// Zephyr doesn't maintain one multi-heap. So, make our own using TLSF.
void port_heap_init(void) {
    // Do a test malloc to determine if Zephyr has an outer heap that may
    // overlap with a memory region we've identified in ram_bounds. We'll
    // corrupt each other if we both use it.
    #ifdef CONFIG_COMMON_LIBC_MALLOC
    uint32_t *test_malloc = malloc(32);
    free(test_malloc); // Free right away so we don't forget. We don't actually write it anyway.
    zephyr_malloc_active = test_malloc != NULL;
    #endif

    for (size_t i = 0; i < CIRCUITPY_RAM_DEVICE_COUNT; i++) {
        uint32_t *heap_bottom = ram_bounds[2 * i];
        uint32_t *heap_top = ram_bounds[2 * i + 1];
        size_t size = (heap_top - heap_bottom) * sizeof(uint32_t);
        // The linker script may fill up a region we thought we could use at
        // build time. (The ram_bounds values are sometimes determined by the
        // linker.) So, we need to guard against regions that aren't actually
        // free.
        if (size < 1024) {
            printk("Skipping region because the linker filled it up.\n");
            continue;
        }
        #ifdef CONFIG_COMMON_LIBC_MALLOC
        // Skip a ram region if our test malloc is within it. We'll use Zephyr's
        // malloc to share that space with Zephyr.
        if (heap_bottom <= test_malloc && test_malloc < heap_top) {
            zephyr_malloc_top = heap_top;
            zephyr_malloc_bottom = heap_bottom;
            printk("Skipping region because Zephyr malloc is within bounds\n");
            pools[i] = NULL;
            continue;
        }
        #endif

        printk("Init heap at %p - %p with size %d\n", heap_bottom, heap_top, size);
        // If this crashes, then make sure you've enabled all of the Kconfig needed for the drivers.
        if (valid_pool_count == 0) {
            heap = tlsf_create_with_pool(heap_bottom, size, circuitpy_max_ram_size);
            pools[i] = tlsf_get_pool(heap);
        } else {
            pools[i] = tlsf_add_pool(heap, heap_bottom + 1, size - sizeof(uint32_t));
        }
        valid_pool_count++;
    }
    #if !DT_HAS_CHOSEN(zephyr_sram)
    #error "No SRAM!"
    #endif
}

void *port_malloc(size_t size, bool dma_capable) {
    void *block = NULL;
    if (valid_pool_count > 0) {
        block = tlsf_malloc(heap, size);
    }
    #ifdef CONFIG_COMMON_LIBC_MALLOC
    if (block == NULL) {
        block = malloc(size);
    }
    #endif
    return block;
}

void port_free(void *ptr) {
    if (valid_pool_count > 0 && !(ptr >= zephyr_malloc_bottom && ptr < zephyr_malloc_top)) {
        tlsf_free(heap, ptr);
        return;
    }
    #ifdef CONFIG_COMMON_LIBC_MALLOC
    free(ptr);
    #endif
}

void *port_realloc(void *ptr, size_t size, bool dma_capable) {
    if (valid_pool_count > 0 && !(ptr >= zephyr_malloc_bottom && ptr < zephyr_malloc_top)) {
        return tlsf_realloc(heap, ptr, size);
    }
    #ifdef CONFIG_COMMON_LIBC_MALLOC
    return realloc(ptr, size);
    #endif
    return NULL;
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
    if (valid_pool_count > 0) {
        for (size_t i = 0; i < CIRCUITPY_RAM_DEVICE_COUNT; i++) {
            if (pools[i] == NULL) {
                continue;
            }
            tlsf_walk_pool(pools[i], max_size_walker, &max_size);
        }
        // IDF does this. Not sure why.
        return tlsf_fit_size(heap, max_size);
    }
    return 64 * 1024;
}

void assert_post_action(const char *file, unsigned int line) {
    // printk("Assertion failed at %s:%u\n", file, line);
    // Check that this is arm
    #if defined(__arm__)
    __asm__ ("bkpt");
    #endif
    while (1) {
        ;
    }
}
