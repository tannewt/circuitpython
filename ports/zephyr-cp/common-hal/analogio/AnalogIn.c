// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Scott Shawcroft for Adafruit Industries LLC
//
// SPDX-License-Identifier: MIT

#include "shared-bindings/analogio/AnalogIn.h"
#include "shared-bindings/microcontroller/Pin.h"

#include <stdint.h>

#include <iobroker/iobroker.h>
#include <zephyr/drivers/adc.h>

#include "bindings/zephyr_kernel/__init__.h"
#include "py/runtime.h"

// Sample at the ADC's full hardware resolution and normalize to CircuitPython's
// 16-bit contract (raw * 65535 / max, rounded, endpoints preserved). The
// emulated ADC handles 16 bits natively; the SAADC tops out at 14.
static uint16_t analogin_sample(const struct device *adc, uint8_t channel_id) {
    #if defined(CONFIG_ADC_NRFX_SAADC)
    const uint8_t resolution = 14;
    #else
    const uint8_t resolution = 16;
    #endif

    uint16_t raw = 0;
    struct adc_sequence sequence = {
        .channels = BIT(channel_id),
        .buffer = &raw,
        .buffer_size = sizeof(raw),
        .resolution = resolution,
    };

    int err = adc_read(adc, &sequence);
    if (err != 0) {
        raise_zephyr_error(err);
    }

    if (resolution >= 16) {
        return raw;
    }

    const uint32_t max_raw = (1u << resolution) - 1u;
    return (uint16_t)((raw > max_raw ? max_raw : (uint32_t)raw) * 65535u / max_raw);
}

void common_hal_analogio_analogin_construct(analogio_analogin_obj_t *self, const mcu_pin_obj_t *pin) {
    // Allocate the pad's analog input through the iobroker module: the call
    // claims the pad so that bus and GPIO allocations refuse it, resolves the
    // fixed analog input the pad is bonded to (identity on the emulated ADC)
    // and hands out a free channel slot on the ADC device.
    const struct device *adc;
    uint8_t channel;
    uint8_t input;
    int res = iobroker_analog_allocate(pin->package_pin, IOBROKER_ANALOG_ADC,
        &adc, &channel, &input);
    if (res < 0) {
        raise_zephyr_error(res);
    }

    if (!device_is_ready(adc)) {
        iobroker_analog_release(adc, channel);
        raise_zephyr_error(-ENODEV);
    }

    struct adc_channel_cfg channel_cfg = {
        .gain = ADC_GAIN_1,
        .reference = ADC_REF_INTERNAL,
        .acquisition_time = ADC_ACQ_TIME_DEFAULT,
        .channel_id = channel,
    };
    #ifdef CONFIG_ADC_CONFIGURABLE_INPUTS
    channel_cfg.input_positive = input;
    #endif

    int err = adc_channel_setup(adc, &channel_cfg);
    if (err != 0) {
        iobroker_analog_release(adc, channel);
        raise_zephyr_error(err);
    }

    self->pin = pin;
    self->adc = adc;
    self->channel_id = channel;
}

bool common_hal_analogio_analogin_deinited(analogio_analogin_obj_t *self) {
    return self->adc == NULL;
}

void common_hal_analogio_analogin_deinit(analogio_analogin_obj_t *self) {
    if (common_hal_analogio_analogin_deinited(self)) {
        return;
    }

    (void)iobroker_analog_release(self->adc, self->channel_id);
    self->pin = NULL;
    self->adc = NULL;
}

uint16_t common_hal_analogio_analogin_get_value(analogio_analogin_obj_t *self) {
    // The value is a 16-bit code relative to the reference voltage, which is
    // what CircuitPython's analogio contract is.
    return analogin_sample(self->adc, self->channel_id);
}

float common_hal_analogio_analogin_get_reference_voltage(analogio_analogin_obj_t *self) {
    uint16_t internal_ref_mv = adc_ref_internal(self->adc);
    if (internal_ref_mv == 0) {
        return 0.0f;
    }
    return (float)internal_ref_mv / 1000.0f;
}
