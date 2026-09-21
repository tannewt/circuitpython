// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Mike Mabey
//
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"

// Object struct and the common_hal_* accessors.
#include "shared-module/hardwarekey/HardwareKey.h"

// Type object used in Python. Shared between ports.
extern const mp_obj_type_t hardwarekey_hardwarekey_type;
