// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2021 Blues Wireless Contributors
//
// SPDX-License-Identifier: MIT

#include "stm32l4xx_hal.h"
#include "supervisor/shared/safe_mode.h"
#include <stdbool.h>

// L4 Series
#if defined(STM32L4R5xx)
#include "stm32l4/stm32l4r5xx/clocks.h"
#elif defined(STM32L433xx)
#include "stm32l4/stm32l433xx/clocks.h"
#else
#error Please add other MCUs here so that they are not silently ignored due to #define typos
#endif

#if BOARD_HAS_HIGH_SPEED_CRYSTAL
#error HSE support needs to be added for the L4 family.
#elif !BOARD_HAS_LOW_SPEED_CRYSTAL
  #error LSE clock source required
#elif !defined(BOARD_LSE_DRIVE_LEVEL)
  #error BOARD_LSE_DRIVE_LEVEL is not defined for this board. The board should define the drive strength of 32kHz external crystal in line with calculations specified in ST AN2867 sections 3.3, 3.4, and STM32L4 datasheet DS12023 Table 58, LSE oscillator characteristics.
#endif

void Error_Handler(void) {
    for (;;) {

    }
}

#define HAL_CHECK(x) if (x != HAL_OK) Error_Handler()

void stm32_peripherals_clocks_init(void) {

    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};

    // Configure LSE Drive
    HAL_PWR_EnableBkUpAccess();
    __HAL_RCC_LSEDRIVE_CONFIG(BOARD_LSE_DRIVE_LEVEL);
    __HAL_RCC_PWR_CLK_ENABLE();

    /** Configure the main internal regulator output voltage
  */
    #if defined(STM32L4R5xx)
    if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1_BOOST) != HAL_OK) {
        Error_Handler();
    }
    #elif defined(STM32L433xx)
    if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1) != HAL_OK) {
        Error_Handler();
    }
    #endif

    /* Activate PLL with MSI , stabilizied via PLL by LSE */
    #if defined(STM32L4R5xx)
    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_LSE | RCC_OSCILLATORTYPE_MSI;
    RCC_OscInitStruct.MSIState = RCC_MSI_ON;
    RCC_OscInitStruct.LSEState = RCC_LSE_ON;
    RCC_OscInitStruct.MSICalibrationValue = RCC_MSICALIBRATION_DEFAULT;
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_MSI;
    RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
    RCC_OscInitStruct.MSIClockRange = RCC_MSIRANGE_11;
    RCC_OscInitStruct.PLL.PLLM = 6;
    RCC_OscInitStruct.PLL.PLLN = 30;
    RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV5;
    RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
    #elif defined(STM32L433xx)
    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_LSE | RCC_OSCILLATORTYPE_HSI48 | RCC_OSCILLATORTYPE_MSI;
    RCC_OscInitStruct.HSI48State = RCC_HSI48_ON;
    RCC_OscInitStruct.MSIState = RCC_MSI_ON;
    RCC_OscInitStruct.LSEState = RCC_LSE_ON;
    RCC_OscInitStruct.MSICalibrationValue = 0;
    RCC_OscInitStruct.MSIClockRange = RCC_MSIRANGE_6;
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_MSI;
    RCC_OscInitStruct.PLL.PLLM = 1;
    RCC_OscInitStruct.PLL.PLLN = 40;
    RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV7;
    RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
    RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
    #else
    #error Unknown MCU
    #endif

    HAL_CHECK(HAL_RCC_OscConfig(&RCC_OscInitStruct));

    #ifdef STM32L4R5xx
    /* Enable MSI Auto-calibration through LSE */
    HAL_RCCEx_EnableMSIPLLMode();
    #endif

    /* Select PLL as system clock source and configure the HCLK, PCLK1 and PCLK2
    clocks dividers */
    RCC_ClkInitStruct.ClockType = (RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2);
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
    #if defined(STM32L4R5xx)
    HAL_CHECK(HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_3));
    #elif defined(STM32L433xx)
    HAL_CHECK(HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4));
    #else
    #error Please expand the conditional compilation to set the default flash latency
    #endif

    /* AHB prescaler divider at 1 as second step */
    #ifdef STM32L4R5xx
    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    HAL_CHECK(HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5));
    #endif

    /* Select MSI output as USB clock source */
    PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_USB | RCC_PERIPHCLK_RTC | RCC_PERIPHCLK_ADC;
    #if defined(STM32L4R5xx)
    PeriphClkInitStruct.UsbClockSelection = RCC_USBCLKSOURCE_MSI;
    #elif defined(STM32L433xx)
    PeriphClkInitStruct.UsbClockSelection = RCC_USBCLKSOURCE_HSI48;
    #endif
    PeriphClkInitStruct.RTCClockSelection = RCC_RTCCLKSOURCE_LSE;
    PeriphClkInitStruct.AdcClockSelection = RCC_ADCCLKSOURCE_SYSCLK;

    HAL_CHECK(HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct));
}
