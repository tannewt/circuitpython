// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2025 Dan Halbert for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "py/enum.h"

#include "shared-bindings/wifi/PowerManagement.h"

//| class PowerManagement:
//|     """Power-saving options for wifi
//|
//|     .. note:: On boards using the CYW43 radio module, the choices below correspond to the
//|       power management values defined in the `cyw43` module:
//|       `PowerManagement.MIN` is the same as `cyw43.PM_PERFORMANCE`, `PowerManagement.MAX`
//|       is the same as `cyw43.PM_AGGRESSIVE`, and `PowerManagement.NONE` is the same as
//|       `cyw43.PM_DISABLED`. If a custom value was set with `cyw43.set_power_management()`
//|       not corresponding to one of these three values, then `PowerManagement.UNKNOWN` will be returned.
//|       """
//|
//|     MIN: PowerManagement
//|     """Minimum power management (default). The WiFi station wakes up to receive a beacon every DTIM period.
//|        The DTIM period is set by the access point."""
//|     MAX: PowerManagement
//|     """Maximum power management, at the expense of some performance. The WiFi station wakes up less often than `MIN`."""
//|     NONE: PowerManagement
//|     """No power management: the WiFi station does not sleep."""
//|     UNKNOWN: PowerManagement
//|     """Power management setting cannot be determined."""
//|

// In order of the enum type.
MAKE_ENUM_VALUE(wifi_power_management_type, power_management, NONE, POWER_MANAGEMENT_NONE);
MAKE_ENUM_VALUE(wifi_power_management_type, power_management, MIN, POWER_MANAGEMENT_MIN);
MAKE_ENUM_VALUE(wifi_power_management_type, power_management, MAX, POWER_MANAGEMENT_MAX);
MAKE_ENUM_VALUE(wifi_power_management_type, power_management, UNKNOWN, POWER_MANAGEMENT_UNKNOWN);

MAKE_ENUM_MAP(wifi_power_management) {
    MAKE_ENUM_MAP_ENTRY(power_management, NONE),
    MAKE_ENUM_MAP_ENTRY(power_management, MIN),
    MAKE_ENUM_MAP_ENTRY(power_management, MAX),
    MAKE_ENUM_MAP_ENTRY(power_management, UNKNOWN),
};

static MP_DEFINE_CONST_DICT(wifi_power_management_locals_dict, wifi_power_management_locals_table);

MAKE_PRINTER(wifi, wifi_power_management);

MAKE_ENUM_TYPE(wifi, PowerManagement, wifi_power_management);
