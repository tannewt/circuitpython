// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Mikey Sklar
//
// SPDX-License-Identifier: MIT

#pragma once

// Releases the JPEG engine. Called on VM reset.
void espressif_jpegio_reset(void);
