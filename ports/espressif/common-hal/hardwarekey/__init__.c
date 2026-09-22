// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Mike Mabey
//
// SPDX-License-Identifier: MIT

#include "common-hal/hardwarekey/__init__.h"
#include "common-hal/hardwarekey/board.h"

#include "shared-bindings/hardwarekey/HardwareKey.h"

// The objects board.EFUSE_KEY0 .. board.EFUSE_KEY<n-1> point at (see board.h).
// Static, not heap: the board globals dict is const and outlives the GC heap
// across a soft reset, so these must too. No GC-traced pointers inside.
hardwarekey_hardwarekey_obj_t hardwarekey_efuse_keys[HARDWAREKEY_EFUSE_SLOT_COUNT];

// board.EFUSE_KEYn, for repr(). Can't assume MP_QSTR_EFUSE_KEY0 + n are contiguous.
static const qstr slot_names[HARDWAREKEY_EFUSE_SLOT_COUNT] = {
    MP_QSTR_EFUSE_KEY0, MP_QSTR_EFUSE_KEY1, MP_QSTR_EFUSE_KEY2,
    MP_QSTR_EFUSE_KEY3, MP_QSTR_EFUSE_KEY4, MP_QSTR_EFUSE_KEY5,
};

void espressif_hardwarekey_init(void) {
    for (mp_int_t slot = 0; slot < HARDWAREKEY_EFUSE_SLOT_COUNT; slot++) {
        hardwarekey_hardwarekey_obj_t *key = &hardwarekey_efuse_keys[slot];
        key->base.type = &hardwarekey_hardwarekey_type;
        key->name = slot_names[slot];
        hardwarekey_efuse_slot_load(slot, key);
    }
}
