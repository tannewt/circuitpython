// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2025 Adafruit Industries LLC
//
// SPDX-License-Identifier: MIT

// Dynamic peripheral allocation: pick a free bus instance enabled in the
// devicetree, route it to the requested pins at runtime and hand the Zephyr
// device to a busio object. The generated tables in board.c describe the
// allocatable instances for the board.
//
// This works on nRF SoCs because their pin control encoding is a simple
// bit-packed value (see zephyr/dt-bindings/pinctrl/nrf-pinctrl.h and
// soc/nordic/common/pinctrl_soc.h) that can be computed at runtime, and any
// peripheral function can be routed to (almost) any pin via PSEL.

#include "common-hal/busio/dynamic_bus.h"

#include <errno.h>
#include <stddef.h>

#include <zephyr/device.h>

#if defined(CONFIG_PINCTRL)

#include <zephyr/drivers/pinctrl.h>

// Weak fallbacks for boards whose generated board.c does not emit strong
// tables (non-nRF SoCs and boards without allocatable instances).
__attribute__((weak)) const dynamic_bus_instance_t cp_dynamic_i2c_buses[1] = {{0}};
__attribute__((weak)) const size_t cp_dynamic_i2c_bus_count = 0;
__attribute__((weak)) dynamic_bus_state_t cp_dynamic_i2c_bus_states[1];

__attribute__((weak)) const dynamic_bus_instance_t cp_dynamic_spi_buses[1] = {{0}};
__attribute__((weak)) const size_t cp_dynamic_spi_bus_count = 0;
__attribute__((weak)) dynamic_bus_state_t cp_dynamic_spi_bus_states[1];

__attribute__((weak)) const dynamic_bus_instance_t cp_dynamic_uart_buses[1] = {{0}};
__attribute__((weak)) const size_t cp_dynamic_uart_bus_count = 0;
__attribute__((weak)) dynamic_bus_state_t cp_dynamic_uart_bus_states[1];

#if defined(CONFIG_PINCTRL_NRF) && defined(CONFIG_PINCTRL_DYNAMIC) && \
    defined(CONFIG_DEVICE_DEINIT_SUPPORT)

#include <zephyr/dt-bindings/pinctrl/nrf-pinctrl.h>

// Bit positions/fields replicated from nrf-pinctrl.h so that entries can be
// encoded at runtime instead of by the DT macros.
#define NRF_PSEL_FUN(fun)       (((uint32_t)(fun) & NRF_FUN_MSK) << NRF_FUN_POS)
#define NRF_PSEL_PIN(pin)       (((uint32_t)(pin) & NRF_PIN_MSK) << NRF_PIN_POS)
#define NRF_PSEL_DISCONNECT(fun) \
    ((uint32_t)NRF_PIN_DISCONNECTED | NRF_PSEL_FUN(fun))
#define NRF_PSEL_PULL_UP        ((uint32_t)NRF_PULL_UP << NRF_PULL_POS)
#define NRF_PSEL_LP             ((uint32_t)NRF_LP_ENABLE << NRF_LP_POS)
#define NRF_PSEL_CLOCKPIN       BIT(NRF_CLOCKPIN_ENABLE_POS)
// Pin + function bits, used to compare a request against a devicetree state.
#define NRF_PSEL_PINFUN_MASK    \
    (((uint32_t)NRF_PIN_MSK << NRF_PIN_POS) | ((uint32_t)NRF_FUN_MSK << NRF_FUN_POS))

// Check that a pin object can be encoded (its GPIO controller is known).
static bool nrf_pin_ok(const mcu_pin_obj_t *pin) {
    if (pin == NULL) {
        return true;
    }
    return cp_gpio_port_index(pin->port) >= 0;
}

// Encode one nRF pin control entry. pin may be NULL to leave the signal
// disconnected. pull_up enables the internal pull resistor (used for I2C and
// UART RX).
static pinctrl_soc_pin_t nrf_psel_encode(uint32_t fun, const mcu_pin_obj_t *pin, bool pull_up) {
    uint32_t psel;

    if (pin != NULL) {
        int port = cp_gpio_port_index(pin->port);
        uint32_t absolute_pin = (uint32_t)port * 32U + pin->number;
        psel = NRF_PSEL_PIN(absolute_pin) | NRF_PSEL_FUN(fun);
        // On nRF54 the GPIO pin clock must be enabled for signals that drive
        // the pad. The pinctrl driver ignores this bit where unsupported
        // (nRF52/nRF53).
        switch (fun) {
            case NRF_FUN_TWIM_SDA:
            case NRF_FUN_TWIM_SCL:
            case NRF_FUN_SPIM_SCK:
            case NRF_FUN_SPIM_MOSI:
            case NRF_FUN_UART_TX:
                psel |= NRF_PSEL_CLOCKPIN;
                break;
            default:
                break;
        }
    } else {
        psel = NRF_PSEL_DISCONNECT(fun);
    }

    if (pull_up) {
        psel |= NRF_PSEL_PULL_UP;
    }

    return psel;
}

// Check whether a set of requested entries matches the devicetree default
// state of an instance (pin + function only; configuration bits ignored).
static bool nrf_psels_match(const pinctrl_soc_pin_t *psels, uint8_t count,
    const pinctrl_soc_pin_t *requested, uint8_t requested_count) {
    uint32_t mask = NRF_PSEL_PINFUN_MASK;

    for (uint8_t i = 0; i < requested_count; i++) {
        bool found = false;
        for (uint8_t j = 0; j < count; j++) {
            if ((psels[j] & mask) == (requested[i] & mask)) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    // The DT state must not use extra pins either.
    for (uint8_t j = 0; j < count; j++) {
        bool found = false;
        for (uint8_t i = 0; i < requested_count; i++) {
            if ((psels[j] & mask) == (requested[i] & mask)) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

// Re-point an instance at the requested pins: de-initialize the device, swap
// its pinctrl states for entries built at runtime, then initialize it again
// (initialization re-applies the "default" state).
static int dynamic_bus_route(const dynamic_bus_instance_t *inst, dynamic_bus_state_t *state,
    const pinctrl_soc_pin_t *pins, uint8_t pin_count) {
    if (pin_count > DYNAMIC_BUS_MAX_PINS) {
        return -EINVAL;
    }

    int ret = device_deinit(inst->dev);
    if (ret < 0 && ret != -ENOSYS && ret != -EPERM) {
        return ret;
    }

    // The Zephyr pinctrl core requires the same set of state ids to be
    // provided, so mirror the ids the device currently has.
    uint8_t state_cnt = MIN(inst->pcfg->state_cnt, ARRAY_SIZE(state->states));
    if (state_cnt == 0) {
        return -ENOENT;
    }

    for (uint8_t i = 0; i < pin_count; i++) {
        state->default_pins[i] = pins[i];
        // Sleep state: same pins, low power (input buffer disconnected).
        state->sleep_pins[i] = pins[i] | NRF_PSEL_LP;
    }
    for (uint8_t s = 0; s < state_cnt; s++) {
        uint8_t id = inst->pcfg->states[s].id;
        state->states[s].id = id;
        if (id == PINCTRL_STATE_SLEEP) {
            state->states[s].pins = state->sleep_pins;
        } else {
            state->states[s].pins = state->default_pins;
        }
        state->states[s].pin_cnt = pin_count;
    }

    ret = pinctrl_update_states(inst->pcfg, state->states, state_cnt);
    if (ret < 0) {
        return ret;
    }

    ret = device_init(inst->dev);
    if (ret < 0 && ret != -EALREADY) {
        return ret;
    }

    return 0;
}

// Allocate an instance from a pool. Instances that the board enabled with
// disconnected pins are routed dynamically to the requested pins. Instances
// with fixed devicetree pins are only used when the request matches their
// existing state exactly.
static int dynamic_bus_allocate(const dynamic_bus_instance_t *buses, size_t count,
    dynamic_bus_state_t *states, const pinctrl_soc_pin_t *pins, uint8_t pin_count,
    const struct device **dev_out) {
    for (size_t i = 0; i < count; i++) {
        dynamic_bus_state_t *state = &states[i];
        if (state->in_use) {
            continue;
        }

        if (buses[i].dt_psels == NULL) {
            int ret = dynamic_bus_route(&buses[i], state, pins, pin_count);
            if (ret < 0) {
                continue;
            }
            state->in_use = true;
            state->routed = true;
            *dev_out = buses[i].dev;
            return 0;
        }

        if (nrf_psels_match(buses[i].dt_psels, buses[i].dt_psel_count, pins, pin_count)) {
            // Already routed to these pins by the devicetree; just claim it.
            state->in_use = true;
            state->routed = false;
            *dev_out = buses[i].dev;
            return 0;
        }
    }
    return -ENODEV;
}

static bool dynamic_bus_state_find(const struct device *dev, dynamic_bus_state_t **state_out) {
    for (size_t i = 0; i < cp_dynamic_i2c_bus_count; i++) {
        if (cp_dynamic_i2c_buses[i].dev == dev) {
            *state_out = &cp_dynamic_i2c_bus_states[i];
            return true;
        }
    }
    for (size_t i = 0; i < cp_dynamic_spi_bus_count; i++) {
        if (cp_dynamic_spi_buses[i].dev == dev) {
            *state_out = &cp_dynamic_spi_bus_states[i];
            return true;
        }
    }
    for (size_t i = 0; i < cp_dynamic_uart_bus_count; i++) {
        if (cp_dynamic_uart_buses[i].dev == dev) {
            *state_out = &cp_dynamic_uart_bus_states[i];
            return true;
        }
    }
    return false;
}

bool dynamic_bus_release(const struct device *dev) {
    dynamic_bus_state_t *state = NULL;
    if (!dynamic_bus_state_find(dev, &state)) {
        return false;
    }
    bool routed = state->routed;
    // De-init so that the driver applies its sleep state and gives up the
    // pins. For non-routed instances re-initialize immediately to restore
    // the devicetree pin configuration.
    (void)device_deinit(dev);
    if (!routed) {
        (void)device_init(dev);
    }
    state->in_use = false;
    state->never_reset = false;
    state->routed = false;
    return routed;
}

void dynamic_bus_never_reset(const struct device *dev) {
    dynamic_bus_state_t *state = NULL;
    if (dynamic_bus_state_find(dev, &state)) {
        state->never_reset = true;
    }
}

static void dynamic_bus_pool_reset(const dynamic_bus_instance_t *buses, size_t count,
    dynamic_bus_state_t *states) {
    for (size_t i = 0; i < count; i++) {
        dynamic_bus_state_t *state = &states[i];
        if (state->in_use && !state->never_reset) {
            (void)device_deinit(buses[i].dev);
            if (!state->routed) {
                (void)device_init(buses[i].dev);
            }
            state->in_use = false;
            state->never_reset = false;
            state->routed = false;
        }
    }
}

void dynamic_bus_reset_all(void) {
    dynamic_bus_pool_reset(cp_dynamic_i2c_buses, cp_dynamic_i2c_bus_count, cp_dynamic_i2c_bus_states);
    dynamic_bus_pool_reset(cp_dynamic_spi_buses, cp_dynamic_spi_bus_count, cp_dynamic_spi_bus_states);
    dynamic_bus_pool_reset(cp_dynamic_uart_buses, cp_dynamic_uart_bus_count, cp_dynamic_uart_bus_states);
}

int dynamic_bus_i2c_allocate(const mcu_pin_obj_t *sda, const mcu_pin_obj_t *scl,
    const struct device **dev_out) {
    pinctrl_soc_pin_t pins[2];
    if (!nrf_pin_ok(sda) || !nrf_pin_ok(scl)) {
        return -EINVAL;
    }
    pins[0] = nrf_psel_encode(NRF_FUN_TWIM_SDA, sda, true);
    pins[1] = nrf_psel_encode(NRF_FUN_TWIM_SCL, scl, true);
    return dynamic_bus_allocate(cp_dynamic_i2c_buses, cp_dynamic_i2c_bus_count,
        cp_dynamic_i2c_bus_states, pins, 2, dev_out);
}

int dynamic_bus_spi_allocate(const mcu_pin_obj_t *clock, const mcu_pin_obj_t *mosi,
    const mcu_pin_obj_t *miso, const struct device **dev_out) {
    pinctrl_soc_pin_t pins[3];
    if (!nrf_pin_ok(clock) || !nrf_pin_ok(mosi) || !nrf_pin_ok(miso)) {
        return -EINVAL;
    }
    pins[0] = nrf_psel_encode(NRF_FUN_SPIM_SCK, clock, false);
    pins[1] = nrf_psel_encode(NRF_FUN_SPIM_MOSI, mosi, false);
    pins[2] = nrf_psel_encode(NRF_FUN_SPIM_MISO, miso, false);
    return dynamic_bus_allocate(cp_dynamic_spi_buses, cp_dynamic_spi_bus_count,
        cp_dynamic_spi_bus_states, pins, 3, dev_out);
}

int dynamic_bus_uart_allocate(const mcu_pin_obj_t *tx, const mcu_pin_obj_t *rx,
    const mcu_pin_obj_t *rts, const mcu_pin_obj_t *cts, const struct device **dev_out) {
    pinctrl_soc_pin_t pins[4];
    if (!nrf_pin_ok(tx) || !nrf_pin_ok(rx) || !nrf_pin_ok(rts) || !nrf_pin_ok(cts)) {
        return -EINVAL;
    }
    pins[0] = nrf_psel_encode(NRF_FUN_UART_TX, tx, false);
    pins[1] = nrf_psel_encode(NRF_FUN_UART_RX, rx, true);
    pins[2] = nrf_psel_encode(NRF_FUN_UART_RTS, rts, false);
    pins[3] = nrf_psel_encode(NRF_FUN_UART_CTS, cts, false);
    return dynamic_bus_allocate(cp_dynamic_uart_buses, cp_dynamic_uart_bus_count,
        cp_dynamic_uart_bus_states, pins, 4, dev_out);
}

#else // nRF runtime routing not available

int dynamic_bus_i2c_allocate(const mcu_pin_obj_t *sda, const mcu_pin_obj_t *scl,
    const struct device **dev_out) {
    ARG_UNUSED(sda);
    ARG_UNUSED(scl);
    ARG_UNUSED(dev_out);
    return -ENOSYS;
}

int dynamic_bus_spi_allocate(const mcu_pin_obj_t *clock, const mcu_pin_obj_t *mosi,
    const mcu_pin_obj_t *miso, const struct device **dev_out) {
    ARG_UNUSED(clock);
    ARG_UNUSED(mosi);
    ARG_UNUSED(miso);
    ARG_UNUSED(dev_out);
    return -ENOSYS;
}

int dynamic_bus_uart_allocate(const mcu_pin_obj_t *tx, const mcu_pin_obj_t *rx,
    const mcu_pin_obj_t *rts, const mcu_pin_obj_t *cts, const struct device **dev_out) {
    ARG_UNUSED(tx);
    ARG_UNUSED(rx);
    ARG_UNUSED(rts);
    ARG_UNUSED(cts);
    ARG_UNUSED(dev_out);
    return -ENOSYS;
}

bool dynamic_bus_release(const struct device *dev) {
    ARG_UNUSED(dev);
    return false;
}

void dynamic_bus_never_reset(const struct device *dev) {
    ARG_UNUSED(dev);
}

void dynamic_bus_reset_all(void) {
}

#endif // CONFIG_PINCTRL_NRF && CONFIG_PINCTRL_DYNAMIC && CONFIG_DEVICE_DEINIT_SUPPORT

#else // !CONFIG_PINCTRL

int dynamic_bus_i2c_allocate(const mcu_pin_obj_t *sda, const mcu_pin_obj_t *scl,
    const struct device **dev_out) {
    ARG_UNUSED(sda);
    ARG_UNUSED(scl);
    ARG_UNUSED(dev_out);
    return -ENOSYS;
}

int dynamic_bus_spi_allocate(const mcu_pin_obj_t *clock, const mcu_pin_obj_t *mosi,
    const mcu_pin_obj_t *miso, const struct device **dev_out) {
    ARG_UNUSED(clock);
    ARG_UNUSED(mosi);
    ARG_UNUSED(miso);
    ARG_UNUSED(dev_out);
    return -ENOSYS;
}

int dynamic_bus_uart_allocate(const mcu_pin_obj_t *tx, const mcu_pin_obj_t *rx,
    const mcu_pin_obj_t *rts, const mcu_pin_obj_t *cts, const struct device **dev_out) {
    ARG_UNUSED(tx);
    ARG_UNUSED(rx);
    ARG_UNUSED(rts);
    ARG_UNUSED(cts);
    ARG_UNUSED(dev_out);
    return -ENOSYS;
}

bool dynamic_bus_release(const struct device *dev) {
    ARG_UNUSED(dev);
    return false;
}

void dynamic_bus_never_reset(const struct device *dev) {
    ARG_UNUSED(dev);
}

void dynamic_bus_reset_all(void) {
}

#endif // CONFIG_PINCTRL
