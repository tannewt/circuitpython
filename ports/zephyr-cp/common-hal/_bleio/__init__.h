// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2018 Dan Halbert for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2026 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

#include <string.h>

#include <zephyr/bluetooth/uuid.h>

#include "common-hal/_bleio/UUID.h"

// Convert a CircuitPython bleio UUID to a Zephyr bt_uuid stored in caller-
// provided bt_uuid_128 storage.  For 16-bit UUIDs we reinterpret the storage
// as bt_uuid_16 and write .val (at offset 2, matching Zephyr's struct layout).
// For 128-bit UUIDs the 16-byte value is copied directly into bt_uuid_128.val.
// The result can be used as `&out->uuid` wherever a `const struct bt_uuid *`
// is needed; Zephyr dispatches on the .type field at runtime.
static inline void bleio_uuid_to_zephyr(const bleio_uuid_obj_t *cp_uuid,
    struct bt_uuid_128 *out) {
    if (cp_uuid->size == 16) {
        out->uuid.type = BT_UUID_TYPE_16;
        ((struct bt_uuid_16 *)out)->val =
            (cp_uuid->uuid128[13] << 8) | cp_uuid->uuid128[12];
    } else {
        out->uuid.type = BT_UUID_TYPE_128;
        memcpy(out->val, cp_uuid->uuid128, 16);
    }
}
