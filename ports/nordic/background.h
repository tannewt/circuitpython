// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2019 Dan Halbert for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

void board_background_task(void);

// Feed a watchdog that wasn't armed by CircuitPython i.e. a boards custom bootloader
void board_wdt_feed(void);
