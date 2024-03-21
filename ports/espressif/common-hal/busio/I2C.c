/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2020 Scott Shawcroft for Adafruit Industries LLC
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

#include "shared-bindings/busio/I2C.h"
#include "py/mperrno.h"
#include "py/runtime.h"

#include "components/driver/i2c/include/driver/i2c.h"

#include "bindings/espidf/__init__.h"
#include "shared-bindings/microcontroller/__init__.h"
#include "shared-bindings/microcontroller/Pin.h"

void common_hal_busio_i2c_construct(busio_i2c_obj_t *self,
    const mcu_pin_obj_t *scl, const mcu_pin_obj_t *sda, uint32_t frequency, uint32_t timeout_us) {
    // Pins 45 and 46 are "strapping" pins that impact start up behavior. They usually need to
    // be pulled-down so pulling them up for I2C is a bad idea. To make this hard, we don't
    // support I2C on these pins.
    //
    // 46 is also input-only so it'll never work.
    if (scl->number == 45 || scl->number == 46 || sda->number == 45 || sda->number == 46) {
        raise_ValueError_invalid_pins();
    }

    #if CIRCUITPY_REQUIRE_I2C_PULLUPS
    // Test that the pins are in a high state. (Hopefully indicating they are pulled up.)
    gpio_set_direction(sda->number, GPIO_MODE_DEF_INPUT);
    gpio_set_direction(scl->number, GPIO_MODE_DEF_INPUT);

    gpio_pullup_dis(sda->number);
    gpio_pullup_dis(scl->number);
    gpio_pulldown_en(sda->number);
    gpio_pulldown_en(scl->number);

    common_hal_mcu_delay_us(10);

    gpio_pulldown_dis(sda->number);
    gpio_pulldown_dis(scl->number);

    #if CIRCUITPY_I2C_ALLOW_INTERNAL_PULL_UP
    gpio_pullup_en(sda->number);
    gpio_pullup_en(scl->number);
    #endif

    // We must pull up within 3us to achieve 400khz.
    common_hal_mcu_delay_us((1200000 + frequency - 1) / frequency);

    if (gpio_get_level(sda->number) == 0 || gpio_get_level(scl->number) == 0) {
        reset_pin_number(sda->number);
        reset_pin_number(scl->number);
        mp_raise_RuntimeError(MP_ERROR_TEXT("No pull up found on SDA or SCL; check your wiring"));
    }
    #endif


    i2c_master_bus_config_t config = {
        .i2c_port = -1, // auto
        .sda_io_num = sda->number,
        .scl_io_num = scl->number,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {
            #if CIRCUITPY_I2C_ALLOW_INTERNAL_PULL_UP
            .enable_internal_pullup = true, /*!< Internal GPIO pull mode for I2C sda signal*/
            #else
            .enable_internal_pullup = false, /*!< Internal GPIO pull mode for I2C sda signal*/
            #endif
        }
    };
    esp_err_t result = i2c_new_master_bus(&config, &self->handle);

    if (result == ESP_ERR_NOT_FOUND) {
        mp_raise_ValueError(MP_ERROR_TEXT("All I2C peripherals are in use"));
    }
    CHECK_ESP_RESULT(result);

    self->xSemaphore = xSemaphoreCreateMutex();
    if (self->xSemaphore == NULL) {
        i2c_del_master_bus(self->handle);
        self->handle = NULL;
        mp_raise_RuntimeError(MP_ERROR_TEXT("Unable to create lock"));
    }
    self->sda_pin = sda;
    self->scl_pin = scl;
    self->has_lock = false;
    self->frequency = frequency;
    self->timeout_ms = timeout_us / 1000;
    // Round up timeout to nearest ms.
    if (timeout_us % 1000 != 0) {
        self->timeout_ms += 1;
    }

    claim_pin(sda);
    claim_pin(scl);
}

bool common_hal_busio_i2c_deinited(busio_i2c_obj_t *self) {
    return self->sda_pin == NULL;
}

void common_hal_busio_i2c_mark_deinit(busio_i2c_obj_t *self) {
    self->sda_pin = NULL;
}

void common_hal_busio_i2c_deinit(busio_i2c_obj_t *self) {
    if (common_hal_busio_i2c_deinited(self)) {
        return;
    }

    i2c_del_master_bus(self->handle);
    self->handle = NULL;

    common_hal_reset_pin(self->sda_pin);
    common_hal_reset_pin(self->scl_pin);
    self->sda_pin = NULL;
    self->scl_pin = NULL;
    common_hal_busio_i2c_mark_deinit(self);
}

bool common_hal_busio_i2c_probe(busio_i2c_obj_t *self, uint8_t addr) {
    esp_err_t result = i2c_master_probe(self->handle, addr, 1);
    return result == ESP_OK;
}

bool common_hal_busio_i2c_try_lock(busio_i2c_obj_t *self) {
    if (self->has_lock) {
        return false;
    }
    self->has_lock = xSemaphoreTake(self->xSemaphore, 0) == pdTRUE;
    return self->has_lock;
}

bool common_hal_busio_i2c_has_lock(busio_i2c_obj_t *self) {
    return self->has_lock;
}

void common_hal_busio_i2c_unlock(busio_i2c_obj_t *self) {
    xSemaphoreGive(self->xSemaphore);
    self->has_lock = false;
}

static uint8_t convert_esp_err(esp_err_t result) {
    switch (result) {
        case ESP_OK:
            return 0;
        case ESP_FAIL:
            return MP_ENODEV;
        case ESP_ERR_TIMEOUT:
            return MP_ETIMEDOUT;
        default:
            return MP_EIO;
    }
}

static size_t _transaction_duration(size_t frequency, size_t len) {
    size_t khz = frequency / 1000;
    size_t bytes_per_ms = khz / 8;
    // + 1 for the address byte
    return (len + 1) / bytes_per_ms;
}

uint8_t common_hal_busio_i2c_write(busio_i2c_obj_t *self, uint16_t addr, const uint8_t *data, size_t len) {
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = self->frequency
    };
    i2c_master_dev_handle_t dev_handle;
    CHECK_ESP_RESULT(i2c_master_bus_add_device(self->handle, &dev_config, &dev_handle));
    esp_err_t result = i2c_master_transmit(dev_handle, data, len, _transaction_duration(self->frequency, len) + self->timeout_ms);
    CHECK_ESP_RESULT(i2c_master_bus_rm_device(dev_handle));
    return convert_esp_err(result);
}

uint8_t common_hal_busio_i2c_read(busio_i2c_obj_t *self, uint16_t addr, uint8_t *data, size_t len) {
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = self->frequency
    };
    i2c_master_dev_handle_t dev_handle;
    CHECK_ESP_RESULT(i2c_master_bus_add_device(self->handle, &dev_config, &dev_handle));
    esp_err_t result = i2c_master_receive(dev_handle, data, len, _transaction_duration(self->frequency, len) + self->timeout_ms);
    CHECK_ESP_RESULT(i2c_master_bus_rm_device(dev_handle));
    return convert_esp_err(result);
}

uint8_t common_hal_busio_i2c_write_read(busio_i2c_obj_t *self, uint16_t addr,
    uint8_t *out_data, size_t out_len, uint8_t *in_data, size_t in_len) {
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = self->frequency
    };
    i2c_master_dev_handle_t dev_handle;
    CHECK_ESP_RESULT(i2c_master_bus_add_device(self->handle, &dev_config, &dev_handle));
    esp_err_t result = i2c_master_transmit_receive(dev_handle, out_data, out_len, in_data, in_len, _transaction_duration(self->frequency, out_len) + _transaction_duration(self->frequency, in_len) + self->timeout_ms);
    CHECK_ESP_RESULT(i2c_master_bus_rm_device(dev_handle));
    return convert_esp_err(result);
}

void common_hal_busio_i2c_never_reset(busio_i2c_obj_t *self) {
    common_hal_never_reset_pin(self->scl_pin);
    common_hal_never_reset_pin(self->sda_pin);
}
