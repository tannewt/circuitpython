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
#include "shared-bindings/usb_host/Port.h"

#include "src/common/pico_time/include/pico/time.h"
#include "src/rp2040/hardware_structs/include/hardware/structs/mpu.h"
#include "src/rp2_common/cmsis/stub/CMSIS/Device/RaspberryPi/RP2040/Include/RP2040.h"
#include "src/rp2_common/hardware_dma/include/hardware/dma.h"
#include "src/rp2_common/pico_multicore/include/pico/multicore.h"

#include "py/runtime.h"

#include "tusb.h"

#include "lib/Pico-PIO-USB/src/pio_usb_configuration.h"

#include "supervisor/serial.h"

bool usb_host_init;

STATIC PIO pio_instances[2] = {pio0, pio1};
static alarm_pool_t *volatile _core1_alarm_pool = NULL;

static void __not_in_flash_func(core1_main)(void) {
    // The MPU is reset before this starts.
    _core1_alarm_pool = alarm_pool_create(2, 1);
    console_uart_printf("core1 alarm pool created %p\r\n", _core1_alarm_pool);

    // Turn off flash access. After this, it will hard fault. Better than messing
    // up CIRCUITPY.
    // MPU->CTRL = MPU_CTRL_PRIVDEFENA_Msk | MPU_CTRL_ENABLE_Msk;
    // MPU->RNR = 6; // 7 is used by pico-sdk stack protection.
    // MPU->RBAR = XIP_MAIN_BASE | MPU_RBAR_VALID_Msk;
    // MPU->RASR = MPU_RASR_XN_Msk | // Set execute never and everything else is restricted.
    //     MPU_RASR_ENABLE_Msk |
    //     (0x1b << MPU_RASR_SIZE_Pos);         // Size is 0x10000000 which masks up to SRAM region.
    // MPU->RNR = 7;

    while (true) {
        // Sleep until an interrupt occurs.
        // __wfi();
        asm ("nop");
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
    pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
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


    // Core 1 will run the SOF interrupt in the alarm pool it creates.
    _core1_alarm_pool = NULL;
    multicore_launch_core1(core1_main);
    console_uart_printf("waiting for alarm pool\r\n");
    while (_core1_alarm_pool == NULL) {
    }
    console_uart_printf("got alarm pool\r\n");
    pio_cfg.alarm_pool = _core1_alarm_pool;

    console_uart_printf("tuh_configure\r\n");
    tuh_configure(TUH_OPT_RHPORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);
    console_uart_printf("tuh_configured\r\n");
    tuh_init(TUH_OPT_RHPORT);

    console_uart_printf("tuh_inited\r\n");
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
