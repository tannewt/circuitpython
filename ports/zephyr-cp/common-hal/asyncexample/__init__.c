// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2025 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include <zephyr/kernel.h>
#include "py/runtime.h"
#include "common-hal/asyncexample/__init__.h"

typedef struct {
    mp_int_t delay_ms;
    circuitpy_async_flag_t *flag;
    struct k_timer timer;
} asyncexample_delay_context_t;

static void timer_expiry(struct k_timer *timer) {
    asyncexample_delay_context_t *ctx =
        CONTAINER_OF(timer, asyncexample_delay_context_t, timer);
    CIRCUITPY_ASYNC_FLAG_SET(ctx->flag);
}

void *common_hal_asyncexample_delay_start(circuitpy_async_flag_t *flag, mp_int_t ms) {
    asyncexample_delay_context_t *ctx = m_new_obj(asyncexample_delay_context_t);
    ctx->delay_ms = ms;
    ctx->flag = flag;

    if (ms == 0) {
        CIRCUITPY_ASYNC_FLAG_SET(flag);
        return ctx;
    }
    k_timer_init(&ctx->timer, timer_expiry, NULL);
    k_timer_start(&ctx->timer, K_MSEC(ms), K_NO_WAIT);
    return ctx;
}

mp_obj_t common_hal_asyncexample_delay_end(void *context) {
    asyncexample_delay_context_t *ctx = context;
    k_timer_stop(&ctx->timer);
    return mp_obj_new_int(ctx->delay_ms);
}

void common_hal_asyncexample_delay_cancel(void *context) {
    asyncexample_delay_context_t *ctx = context;
    k_timer_stop(&ctx->timer);
}
