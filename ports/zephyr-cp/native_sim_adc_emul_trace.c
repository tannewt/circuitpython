/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Feed the emulated ADC's channel inputs from the perfetto input trace, read
 * on demand: the emul backend calls the value function every time a channel
 * is read, and the function returns the latest trace value at the current
 * simulated time. Values are millivolts, the same unit the Zephyr ADC emul's
 * value functions use, so the emulator applies the channel's reference and
 * gain on the way out like real hardware. Track values before the first
 * trace event read as 0 mV.
 *
 * Track format: "adc.<channel>" counter tracks, same naming the tests use.
 *
 * Native sim only: the emulated ADC and the perfetto trace reader are
 * native_sim features.
 */

#include <stdio.h>

#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/adc/adc_emul.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

#include "nsi_hws_models_if.h"
#include "nsi_perfetto_trace.h"

LOG_MODULE_REGISTER(adc_emul_trace, LOG_LEVEL_INF);

#if DT_HAS_COMPAT_STATUS_OKAY(zephyr_adc_emul)
#define ADC_EMUL_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(zephyr_adc_emul)
#define ADC_EMUL_NCHANNELS DT_PROP(ADC_EMUL_NODE, nchannels)
#define ADC_EMUL_DEVICE DEVICE_DT_GET(ADC_EMUL_NODE)
#else
#define ADC_EMUL_NCHANNELS 0
#endif

// Value function for every emulated ADC channel: the latest counter value of
// the "adc.<n>" track at the current simulated time, in millivolts. The
// emulator converts it to the requested resolution using the channel's
// reference and gain.
static int adc_emul_trace_read(const struct device *dev, unsigned int chan,
    void *data, uint32_t *result) {
    char track_name[16];
    int len = snprintf(track_name, sizeof(track_name), "adc.%u", chan);
    if (len <= 0 || len >= (int)sizeof(track_name)) {
        return -EINVAL;
    }

    int64_t millivolts = 0;
    uint64_t now_ns = nsi_hws_get_time() * 1000U;
    if (!nsi_perfetto_trace_get_counter_int(track_name, now_ns, &millivolts)) {
        // No trace for this channel: the input floats at 0 mV.
        *result = 0;
        return 0;
    }

    if (millivolts < 0) {
        millivolts = 0;
    }
    *result = (uint32_t)millivolts;
    return 0;
}

static int adc_emul_trace_init(void) {
    #if DT_HAS_COMPAT_STATUS_OKAY(zephyr_adc_emul)
    if (!device_is_ready(ADC_EMUL_DEVICE)) {
        LOG_WRN("emulated ADC not ready; trace inputs disabled");
        return 0;
    }
    for (unsigned int chan = 0; chan < ADC_EMUL_NCHANNELS; chan++) {
        int ret = adc_emul_value_func_set(ADC_EMUL_DEVICE, chan,
            adc_emul_trace_read, NULL);
        if (ret != 0) {
            LOG_WRN("failed to wire channel %u to the input trace: %d", chan, ret);
        }
    }
    LOG_INF("ADC inputs follow the perfetto input trace in millivolts "
        "(%u channels)", (unsigned int)ADC_EMUL_NCHANNELS);
    #endif
    return 0;
}

SYS_INIT(adc_emul_trace_init, APPLICATION, 98);
