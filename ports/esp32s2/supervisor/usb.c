/*
 * This file is part of the Micro Python project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2018 hathach for Adafruit Industries
 * Copyright (c) 2019 Lucian Copeland for Adafruit Industries
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

#include "supervisor/usb.h"
#include "lib/utils/interrupt_char.h"
#include "lib/mp-readline/readline.h"

#include "components/soc/soc/esp32s2/include/soc/usb_periph.h"
#include "components/driver/include/driver/periph_ctrl.h"
#include "components/driver/include/driver/gpio.h"
#include "components/esp_rom/include/esp32s2/rom/gpio.h"
#include "components/esp_rom/include/esp_rom_gpio.h"
#include "components/hal/esp32s2/include/hal/gpio_ll.h"
#include "components/xtensa/include/esp_debug_helpers.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hal/usb_hal.h"

#include "tusb.h"

#include "components/log/include/esp_log.h"

static const char* TAG = "esp usb";

#ifdef CFG_TUSB_DEBUG
  #define USBD_STACK_SIZE     (3*configMINIMAL_STACK_SIZE)
#else
  #define USBD_STACK_SIZE     (3*configMINIMAL_STACK_SIZE/2)
#endif

StackType_t  usb_device_stack[USBD_STACK_SIZE];
StaticTask_t usb_device_taskdef;

TaskHandle_t sleeping_circuitpython_task = NULL;

// USB Device Driver task
// This top level thread process all usb events and invoke callbacks
void usb_device_task(void* param)
{
  (void) param;

  // RTOS forever loop
  while (1)
  {
    // tinyusb device task
    if (tusb_inited()) {
        tud_task();
        tud_cdc_write_flush();
    }
    vTaskDelay(1);
  }
}

static void configure_pins (usb_hal_context_t *usb)
{
    /* usb_periph_iopins currently configures USB_OTG as USB Device.
     * Introduce additional parameters in usb_hal_context_t when adding support
     * for USB Host.
     */
    for ( const usb_iopin_dsc_t *iopin = usb_periph_iopins; iopin->pin != -1; ++iopin ) {
        if ( (usb->use_external_phy) || (iopin->ext_phy_only == 0) ) {
            esp_rom_gpio_pad_select_gpio(iopin->pin);
            if ( iopin->is_output ) {
                esp_rom_gpio_connect_out_signal(iopin->pin, iopin->func, false, false);
            }
            else {
                esp_rom_gpio_connect_in_signal(iopin->pin, iopin->func, false);
                if ( (iopin->pin != GPIO_FUNC_IN_LOW) && (iopin->pin != GPIO_FUNC_IN_HIGH) ) {
                    gpio_ll_input_enable(&GPIO, iopin->pin);
                }
            }
            esp_rom_gpio_pad_unhold(iopin->pin);
        }
    }
    if ( !usb->use_external_phy ) {
        gpio_set_drive_capability(USBPHY_DM_NUM, GPIO_DRIVE_CAP_3);
        gpio_set_drive_capability(USBPHY_DP_NUM, GPIO_DRIVE_CAP_3);
    }
}

void init_usb_hardware(void) {
    periph_module_reset(PERIPH_USB_MODULE);
    periph_module_enable(PERIPH_USB_MODULE);
    usb_hal_context_t hal = {
        .use_external_phy = false // use built-in PHY
    };
    usb_hal_init(&hal);
    configure_pins(&hal);

    (void) xTaskCreateStatic(usb_device_task,
                             "usbd",
                             USBD_STACK_SIZE,
                             NULL,
                             5,
                             usb_device_stack,
                             &usb_device_taskdef);
}

// void print_backtrace(size_t pc, size_t sp, size_t next_pc) {
//   //Initialize stk_frame with first frame of stack
//     esp_backtrace_frame_t stk_frame;
//     stk_frame.pc = pc;
//     stk_frame.sp = sp;
//     stk_frame.next_pc = next_pc;
//     esp_rom_printf("\r\n\r\nBacktrace:");
//     esp_rom_printf("0x%08X:0x%08X ", esp_cpu_process_stack_pc(stk_frame.pc), stk_frame.sp);

//     //Check if first frame is valid
//     bool corrupted = (esp_stack_ptr_is_sane(stk_frame.sp) &&
//                       esp_ptr_executable((void*)esp_cpu_process_stack_pc(stk_frame.pc))) ?
//                       false : true;

//     uint32_t i = (depth <= 0) ? INT32_MAX : depth;
//     while (i-- > 0 && stk_frame.next_pc != 0 && !corrupted) {
//         if (!esp_backtrace_get_next_frame(&stk_frame)) {    //Get previous stack frame
//             corrupted = true;
//         }
//         esp_rom_printf("0x%08X:0x%08X ", esp_cpu_process_stack_pc(stk_frame.pc), stk_frame.sp);
//     }

//     //Print backtrace termination marker
//     esp_err_t ret = ESP_OK;
//     if (corrupted) {
//         esp_rom_printf(" |<-CORRUPTED");
//         ret =  ESP_FAIL;
//     } else if (stk_frame.next_pc != 0) {    //Backtrace continues
//         esp_rom_printf(" |<-CONTINUES");
//     }
//     esp_rom_printf("\r\n\r\n");
// }
/**
 * Callback invoked when received an "wanted" char.
 * @param itf           Interface index (for multiple cdc interfaces)
 * @param wanted_char   The wanted char (set previously)
 */
void tud_cdc_rx_wanted_cb(uint8_t itf, char wanted_char)
{
    (void) itf; // not used
    ESP_LOGW(TAG, "ctrl-c");
    TaskStatus_t freertos_info[10];
    size_t task_count = uxTaskGetSystemState(freertos_info, 10, NULL);
    for (size_t i = 0; i < task_count; i++) {
      TaskStatus_t* task = &freertos_info[i];
      esp_rom_printf("Task #%d - %s - Stack %p\r\n", task->xTaskNumber, task->pcTaskName, task->pxStackBase);
    }

    esp_backtrace_print(100);



    // Workaround for using lib/utils/interrupt_char.c
    // Compare mp_interrupt_char with wanted_char and ignore if not matched
    if (mp_interrupt_char == wanted_char) {
        tud_cdc_read_flush();    // flush read fifo
        mp_keyboard_interrupt();
        // CircuitPython's VM is run in a separate FreeRTOS task from TinyUSB.
        // So, we must notify the other task when a CTRL-C is received.
        if (sleeping_circuitpython_task != NULL) {
          xTaskNotifyGive(sleeping_circuitpython_task);
        }
    }
}
