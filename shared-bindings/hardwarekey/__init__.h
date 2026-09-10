// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Mike Mabey
//
// SPDX-License-Identifier: MIT

#pragma once

#include "py/enum.h"
#include "py/obj.h"

#include "shared-module/hardwarekey/HardwareKey.h"

extern const mp_obj_type_t hardwarekey_purpose_type;
extern const cp_enum_obj_t hardwarekey_purpose_hmac_obj;
extern const cp_enum_obj_t hardwarekey_purpose_unused_obj;

// The Purpose singleton for a hardwarekey_purpose_t code.
mp_obj_t hardwarekey_purpose_to_obj(hardwarekey_purpose_t purpose);
