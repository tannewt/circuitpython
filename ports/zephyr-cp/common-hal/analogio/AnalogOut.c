// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Scott Shawcroft for Adafruit Industries LLC
//
// SPDX-License-Identifier: MIT

#include "shared-bindings/analogio/AnalogOut.h"
#include "shared-bindings/microcontroller/Pin.h"

#include <iobroker/iobroker.h>
#include <zephyr/device.h>
#include <zephyr/drivers/dac.h>

#include "bindings/zephyr_kernel/__init__.h"
#include "py/runtime.h"

void common_hal_analogio_analogout_construct(analogio_analogout_obj_t *self, const mcu_pin_obj_t *pin) {
    // Allocate the pad's analog output through the iobroker module: the call
    // claims the pad so that bus and GPIO allocations refuse it and hands out
    // a free channel slot on the DAC device.
    const struct device *dac;
    uint8_t channel;
    uint8_t input;
    int res = iobroker_analog_allocate(pin->package_pin, IOBROKER_ANALOG_DAC,
        &dac, &channel, &input);
    if (res == -ENOSYS) {
        mp_raise_NotImplementedError_varg(MP_ERROR_TEXT("%q"), MP_QSTR_AnalogOut);
    }
    if (res < 0) {
        raise_zephyr_error(res);
    }

    struct dac_channel_cfg channel_cfg = {
        .channel_id = channel,
        .resolution = 16,
        .buffered = true,
        .internal = false,
    };

    res = dac_channel_setup(dac, &channel_cfg);
    if (res != 0) {
        iobroker_analog_release(dac, channel);
        raise_zephyr_error(res);
    }

    self->pin = pin;
    self->dac = dac;
    self->channel_id = channel;
    self->resolution = channel_cfg.resolution;
}

bool common_hal_analogio_analogout_deinited(analogio_analogout_obj_t *self) {
    return self->dac == NULL;
}

void common_hal_analogio_analogout_deinit(analogio_analogout_obj_t *self) {
    if (common_hal_analogio_analogout_deinited(self)) {
        return;
    }

    (void)iobroker_analog_release(self->dac, self->channel_id);
    self->pin = NULL;
    self->dac = NULL;
}

void common_hal_analogio_analogout_set_value(analogio_analogout_obj_t *self, uint16_t value) {
    if (common_hal_analogio_analogout_deinited(self)) {
        return;
    }

    int err = dac_write_value(self->dac, self->channel_id, value);
    if (err != 0) {
        raise_zephyr_error(err);
    }
}
