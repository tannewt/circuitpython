// Emulated analog pad allocation for the iobroker module (native_sim).
//
// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-License-Identifier: MIT

// The emulated ADC (zephyr,adc-emul) and the test DAC (vnd,dac) have no
// analog mux: the emulated channel a pad feeds is the pad number itself, and
// the pads are plain GPIO controller pads on the one-to-one package map
// native_sim selects. So allocate resolves the channel from the pad's global
// number and claims the pad; release only drops the claim (the emulated
// devices need no channel teardown).

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "iobroker_internal.h"

LOG_MODULE_DECLARE(iobroker, CONFIG_LOG_DEFAULT_LEVEL);

// Emulated devices: one instance each. Additional instances are ignored
// (there are none in practice on native_sim).

#if defined(CONFIG_ADC_EMUL) && DT_HAS_COMPAT_STATUS_OKAY(zephyr_adc_emul)
#define ANALOG_ADC_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(zephyr_adc_emul)
static const struct device *analog_adc_device(void) {
    return DEVICE_DT_GET(ANALOG_ADC_NODE);
}

static uint8_t analog_adc_channel_count(void) {
    return DT_PROP(ANALOG_ADC_NODE, nchannels);
}
#else
static const struct device *analog_adc_device(void) {
    return NULL;
}

static uint8_t analog_adc_channel_count(void) {
    return 0;
}
#endif

#if defined(CONFIG_DAC_TEST) && DT_HAS_COMPAT_STATUS_OKAY(vnd_dac)
#define ANALOG_DAC_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(vnd_dac)
static const struct device *analog_dac_device(void) {
    return DEVICE_DT_GET(ANALOG_DAC_NODE);
}

// The test DAC's binding has no channel count; the emulated hardware takes
// any channel id, so the number of channels is not bounded beyond the claim
// registry itself.
#define ANALOG_DAC_CHANNEL_COUNT 8
#else
static const struct device *analog_dac_device(void) {
    return NULL;
}

#define ANALOG_DAC_CHANNEL_COUNT 0
#endif

int iobroker_analog_allocate(package_pin_t pin, uint16_t kind,
    const struct device **dev_out, uint8_t *channel_out, uint8_t *input_out) {
    const struct device *dev;
    uint8_t count;
    if (kind == IOBROKER_ANALOG_ADC) {
        dev = analog_adc_device();
        count = analog_adc_channel_count();
    } else if (kind == IOBROKER_ANALOG_DAC) {
        dev = analog_dac_device();
        count = ANALOG_DAC_CHANNEL_COUNT;
    } else {
        return -EINVAL;
    }

    if (dev == NULL || count == 0) {
        return -ENOSYS;
    }

    uint16_t soc_pad;
    int ret = iobroker_package_pin_soc_pad(pin, &soc_pad);
    if (ret < 0) {
        LOG_WRN("analog allocate: package pin %u not in package map",
            (unsigned)pin);
        return ret;
    }

    // Emulated devices have no analog mux: the channel a pad feeds is the
    // pad's global number itself, while it indexes into the device's
    // channel array.
    if (soc_pad >= count) {
        LOG_WRN("analog allocate: pad %u has no analog channel on %s",
            (unsigned)soc_pad, dev->name);
        return -EINVAL;
    }
    uint8_t channel = (uint8_t)soc_pad;

    ret = iobroker_analog_claim_add(pin, dev, channel);
    if (ret < 0) {
        return ret;
    }

    *dev_out = dev;
    *channel_out = channel;
    // The emulated devices have no selectable inputs; the caller ignores
    // this when it configures the channel.
    *input_out = 0;
    LOG_INF("analog allocate: package pin %u (pad %u) -> %s channel %u",
        (unsigned)pin, (unsigned)soc_pad, dev->name, (unsigned)channel);
    return 0;
}

bool iobroker_analog_release(const struct device *dev, uint8_t channel) {
    uint16_t soc_pad;
    if (!iobroker_analog_claim_remove(dev, channel, &soc_pad)) {
        LOG_DBG("analog release: %s channel %u not allocated",
            dev == NULL ? "(null)" : dev->name, (unsigned)channel);
        return false;
    }

    iobroker_gpio_pad_quiesce(soc_pad);
    LOG_INF("analog release: %s channel %u released", dev->name,
        (unsigned)channel);
    return true;
}
