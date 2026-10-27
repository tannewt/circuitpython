// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2018 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

#include "shared-bindings/displayio/Group.h"

extern displayio_group_t circuitpython_splash;

// Both compositor backends implement this. It roots the displayed object tree into the
// GC so that user displays survive garbage collection.
void displayio_gc_collect(void);

#if !CIRCUITPY_DISPLAYIO_LVGL
#include "shared-module/displayio/legacy/__init__.h"
#endif
