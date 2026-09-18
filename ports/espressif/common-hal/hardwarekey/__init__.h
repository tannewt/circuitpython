// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Mike Mabey
//
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"

#include "shared-module/hardwarekey/HardwareKey.h"

// Probe every eFuse key block and fill in the board.EFUSE_KEY* HardwareKey
// objects. Call once at startup, before user code. Never raises.
void espressif_hardwarekey_init(void);

// Fill in `key` for eFuse key block `slot`. Sets key->purpose to HMAC when the
// block is burned HMAC_UP (importing its PSA key), else HARDWAREKEY_PURPOSE_UNUSED.
// Never raises. Returns true when the slot ended up usable.
bool hardwarekey_efuse_slot_load(mp_int_t slot, hardwarekey_hardwarekey_obj_t *key);
