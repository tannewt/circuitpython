// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Tim Cocks for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "shared-bindings/_nina/__init__.h"

#include "py/mperrno.h"
#include "py/objlist.h"
#include "py/runtime.h"

// Replies to VM commands land here before they are copied into bytearrays. Only
// the VM uses these, so a background command can't overwrite them.
static uint8_t vm_reply_buf[NINA_MAX_TRANSFER];
static uint16_t vm_reply_lens[255];

void common_hal__nina_init(void) {
    if (!nina_transport_init()) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("%q in use"), MP_QSTR_pin);
    }
}

bool common_hal__nina_active(void) {
    return nina_transport_active();
}

static void check_active(void) {
    if (!nina_transport_active()) {
        mp_raise_OSError(MP_ENODEV);
    }
}

void common_hal__nina_reset(void) {
    check_active();
    nina_transport_reset();
}

void common_hal__nina_release(void) {
    nina_transport_deinit();
}

// Raises the exception type ESP32SPI raises for the same failure, so its error
// handling keeps working.
static void raise_for(nina_result_t result, uint8_t cmd) {
    switch (result) {
        case NINA_OK:
            return;
        case NINA_NOT_ACTIVE:
            mp_raise_OSError(MP_ENODEV);
        case NINA_BUSY:
            mp_raise_OSError(MP_EBUSY);
        case NINA_INTERRUPTED:
            // Nothing was sent. Raise the pending KeyboardInterrupt now.
            mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_EXCEPTIONS);
            mp_raise_OSError(MP_EINTR);
        case NINA_BAD_SOCKET:
            mp_raise_OSError(MP_EBADF);
        case NINA_TOO_BIG:
            if (cmd == NINA_GET_DATABUF_TCP) {
                mp_raise_ValueError_varg(MP_ERROR_TEXT("%q must be <= %d"), MP_QSTR_size, NINA_MAX_READ);
            }
            mp_raise_ValueError_varg(MP_ERROR_TEXT("%q length must be <= %d"), MP_QSTR_params, NINA_MAX_TRANSFER);
        case NINA_TIMEOUT_READY:
        case NINA_TIMEOUT_SELECT:
        case NINA_TIMEOUT_START:
            mp_raise_msg(&mp_type_TimeoutError, MP_ERROR_TEXT("Operation timed out"));
        case NINA_ERR_RESPONSE:
        case NINA_BAD_REPLY:
            mp_raise_msg(&mp_type_BrokenPipeError, MP_ERROR_TEXT("Received response was invalid"));
        case NINA_REPLY_TOO_BIG:
            mp_raise_OSError(MP_ENOBUFS);
    }
}

mp_obj_t common_hal__nina_transact(uint8_t cmd, const nina_param_t *params, size_t n_params,
    bool param_len_16, int expected_replies, bool reply_len_16) {
    nina_reply_t reply = {
        .buf = vm_reply_buf,
        .buf_len = sizeof(vm_reply_buf),
        .lens = vm_reply_lens,
        .max_count = MP_ARRAY_SIZE(vm_reply_lens),
    };
    nina_result_t result = nina_transact(cmd, params, n_params, param_len_16, &reply,
        expected_replies, reply_len_16, true);
    raise_for(result, cmd);

    mp_obj_list_t *list = MP_OBJ_TO_PTR(mp_obj_new_list(reply.count, NULL));
    size_t offset = 0;
    for (size_t i = 0; i < reply.count; i++) {
        list->items[i] = mp_obj_new_bytearray(vm_reply_lens[i], vm_reply_buf + offset);
        offset += vm_reply_lens[i];
    }
    return MP_OBJ_FROM_PTR(list);
}
