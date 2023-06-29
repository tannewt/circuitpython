/*
 * This file is part of the Micro Python project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2022 Scott Shawcroft for Adafruit Industries
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "bindings/rp2pio/StateMachine.h"
#include "shared-bindings/microcontroller/Pin.h"
#include "shared-bindings/microcontroller/Processor.h"
#include "shared-bindings/usb_host/Port.h"
#include "supervisor/usb.h"

#include "src/common/pico_time/include/pico/time.h"
#include "src/rp2040/hardware_structs/include/hardware/structs/mpu.h"
#include "src/rp2_common/cmsis/stub/CMSIS/Device/RaspberryPi/RP2040/Include/RP2040.h"
#include "src/rp2_common/hardware_dma/include/hardware/dma.h"
#include "src/rp2_common/pico_multicore/include/pico/multicore.h"

#include "py/runtime.h"

#include "tusb.h"

#include "lib/Pico-PIO-USB/src/pio_usb.h"
#include "lib/Pico-PIO-USB/src/pio_usb_configuration.h"

#include "supervisor/serial.h"
#include "supervisor/usb.h"

volatile bool usb_host_init;

STATIC PIO pio_instances[2] = {pio0, pio1};
volatile bool _core1_ready = false;

static void __not_in_flash_func(core1_main)(void) {
    // The MPU is reset before this starts.
    SysTick->LOAD = (uint32_t)((common_hal_mcu_processor_get_frequency() / 1000) - 1UL);
    SysTick->VAL = 0UL;   /* Load the SysTick Counter Value */
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk |  // Processor clock.
        SysTick_CTRL_ENABLE_Msk;

    #if 0
    // Turn off flash access. After this, it will hard fault. Better than messing
    // up CIRCUITPY.
    MPU->CTRL = MPU_CTRL_PRIVDEFENA_Msk | MPU_CTRL_ENABLE_Msk;
    MPU->RNR = 6; // 7 is used by pico-sdk stack protection.
    MPU->RBAR = XIP_MAIN_BASE | MPU_RBAR_VALID_Msk;
    MPU->RASR = MPU_RASR_XN_Msk | // Set execute never and everything else is restricted.
        MPU_RASR_ENABLE_Msk |
        (0x1b << MPU_RASR_SIZE_Pos);         // Size is 0x10000000 which masks up to SRAM region.
    MPU->RNR = 7;
    #endif

    _core1_ready = true;
    while (!usb_host_init) {
        // Wait for USB host to be initialized.
    }

    while (true) {
        pio_usb_host_frame();
        if (tuh_task_event_ready()) {
            // Queue the tinyusb background task.
            usb_background_schedule();
        }
        // Wait for systick to reload.
        while ((SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk) == 0) {
        }
    }
}

STATIC uint8_t _sm_free_count(uint8_t pio_index) {
    PIO pio = pio_instances[pio_index];
    uint8_t free_count = 0;
    for (size_t j = 0; j < NUM_PIO_STATE_MACHINES; j++) {
        if (!pio_sm_is_claimed(pio, j)) {
            free_count++;
        }
    }
    return free_count;
}

STATIC bool _has_program_room(uint8_t pio_index, uint8_t program_size) {
    PIO pio = pio_instances[pio_index];
    pio_program_t program_struct = {
        .instructions = NULL,
        .length = program_size,
        .origin = -1
    };
    return pio_can_add_program(pio, &program_struct);
}

void common_hal_usb_host_port_construct(usb_host_port_obj_t *self, const mcu_pin_obj_t *dp, const mcu_pin_obj_t *dm) {
    if (dp->number + 1 != dm->number) {
        raise_ValueError_invalid_pins();
    }

    // PIO-USB requires CPU clock to be multiple of 120Mhz
    uint32_t cpu_freq = common_hal_mcu_processor_get_frequency();
    if (cpu_freq != 120000000 && cpu_freq != 240000000) {
        // Change CPU clock here cause in-correct clock computation for other already inited peripherals such as UART
//        mcu_processor_obj_t dummy_mcu;
//        common_hal_mcu_processor_set_frequency(&dummy_mcu, 120000000);
//        cpu_freq = common_hal_mcu_processor_get_frequency();
//        if (cpu_freq != 120000000 && cpu_freq != 240000000) {
        mp_raise_RuntimeError(translate("CPU clock must be multiple of 120 MHz"));
        return;
//        }
    }

    pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
    pio_cfg.use_alarm_pool = false;
    pio_cfg.pin_dp = dp->number;
    pio_cfg.pio_tx_num = 0;
    pio_cfg.pio_rx_num = 1;
    // PIO with room for 22 instructions
    // PIO with room for 31 instructions and two free SM.
    if (!_has_program_room(pio_cfg.pio_tx_num, 22) || _sm_free_count(pio_cfg.pio_tx_num) < 1 ||
        !_has_program_room(pio_cfg.pio_rx_num, 31) || _sm_free_count(pio_cfg.pio_rx_num) < 2) {
        mp_raise_RuntimeError(translate("All state machines in use"));
    }
    pio_cfg.tx_ch = dma_claim_unused_channel(false); // DMA channel
    if (pio_cfg.tx_ch < 0) {
        mp_raise_RuntimeError(translate("All dma channels in use"));
    }

    PIO tx_pio = pio_instances[pio_cfg.pio_tx_num];
    pio_cfg.sm_tx = pio_claim_unused_sm(tx_pio, false);
    PIO rx_pio = pio_instances[pio_cfg.pio_rx_num];
    pio_cfg.sm_rx = pio_claim_unused_sm(rx_pio, false);
    pio_cfg.sm_eop = pio_claim_unused_sm(rx_pio, false);

    // Unclaim everything so that the library can.
    dma_channel_unclaim(pio_cfg.tx_ch);
    pio_sm_unclaim(tx_pio, pio_cfg.sm_tx);
    pio_sm_unclaim(rx_pio, pio_cfg.sm_rx);
    pio_sm_unclaim(rx_pio, pio_cfg.sm_eop);

    // Set all of the state machines to never reset.
    rp2pio_statemachine_never_reset(tx_pio, pio_cfg.sm_tx);
    rp2pio_statemachine_never_reset(rx_pio, pio_cfg.sm_rx);
    rp2pio_statemachine_never_reset(rx_pio, pio_cfg.sm_eop);

    common_hal_never_reset_pin(dp);
    common_hal_never_reset_pin(dm);

    // Core 1 will run the SOF interrupt directly.
    _core1_ready = false;
    multicore_launch_core1(core1_main);
    while (!_core1_ready) {
    }

    tuh_configure(TUH_OPT_RHPORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);
    tuh_init(TUH_OPT_RHPORT);

    self->init = true;
    usb_host_init = true;
}

void common_hal_usb_host_port_deinit(usb_host_port_obj_t *self) {
    self->init = false;
    usb_host_init = false;
}

bool common_hal_usb_host_port_deinited(usb_host_port_obj_t *self) {
    return !self->init;
}
