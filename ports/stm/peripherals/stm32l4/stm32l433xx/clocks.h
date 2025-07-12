// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2021 Blues Wireless Contributors
//
// SPDX-License-Identifier: MIT

#pragma once

#include "stm32l4xx_hal.h"

// Chip:                STM32L433
// Line Type:           Foundation Line
// Speed:               80MHz (MAX)

// Defaults:
#ifndef CPY_CLK_VSCALE
#define CPY_CLK_VSCALE (PWR_REGULATOR_VOLTAGE_SCALE1)      // up to 80MHz
#endif
#ifndef CPY_CLK_PLLM
#define CPY_CLK_PLLM (1)
#endif
#ifndef CPY_CLK_PLLN
#define CPY_CLK_PLLN (40)
#endif
#ifndef CPY_CLK_PLLP
#define CPY_CLK_PLLP (RCC_PLLP_DIV7)
#endif
#ifndef CPY_CLK_PLLQ
#define CPY_CLK_PLLQ (2)
#endif
#ifndef CPY_CLK_AHBDIV
#define CPY_CLK_AHBDIV (RCC_SYSCLK_DIV1)
#endif
#ifndef CPY_CLK_APB1DIV
#define CPY_CLK_APB1DIV (RCC_HCLK_DIV1)
#endif
#ifndef CPY_CLK_APB2DIV
#define CPY_CLK_APB2DIV (RCC_HCLK_DIV1)
#endif
#ifndef CPY_CLK_FLASH_LATENCY
#define CPY_CLK_FLASH_LATENCY (FLASH_LATENCY_4)
#endif
#ifndef CPY_CLK_USB_USES_AUDIOPLL
#define CPY_CLK_USB_USES_AUDIOPLL (0)
#endif

#ifndef BOARD_HAS_HIGH_SPEED_CRYSTAL
#define BOARD_HAS_HIGH_SPEED_CRYSTAL (1)
#endif
