// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2025 Adafruit Industries LLC
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stddef.h>

#include "common-hal/microcontroller/Pin.h"

#include <zephyr/device.h>

// Pins needed by the widest bus (UART with tx/rx/rts/cts).
#define DYNAMIC_BUS_MAX_PINS 4

#if defined(CONFIG_PINCTRL)

#include <zephyr/drivers/pinctrl.h>

typedef struct {
    const struct device *dev;
    // Pin control configuration of the device. Mutable because
    // CONFIG_PINCTRL_DYNAMIC moves these to RAM so that states can be
    // swapped at runtime.
    struct pinctrl_dev_config *pcfg;
    // Pin control entries of the devicetree "default" state, or NULL when
    // the instance was enabled with disconnected pins so that CircuitPython
    // can route it dynamically to any pin.
    const pinctrl_soc_pin_t *dt_psels;
    uint8_t dt_psel_count;
} dynamic_bus_instance_t;

// Runtime bookkeeping for one allocatable bus instance. The arrays live for
// the life of the firmware because Zephyr's pinctrl API keeps pointers to
// them inside the device's pinctrl_dev_config.
typedef struct {
    pinctrl_soc_pin_t default_pins[DYNAMIC_BUS_MAX_PINS];
    pinctrl_soc_pin_t sleep_pins[DYNAMIC_BUS_MAX_PINS];
    struct pinctrl_state states[2];
    bool in_use;
    bool never_reset;
    // True when the instance's states were swapped for runtime-built entries.
    bool routed;
} dynamic_bus_state_t;

// Generated tables (board.c). Strong symbols are emitted for nRF boards with
// allocatable bus instances; dynamic_bus.c provides weak empty fallbacks.
extern const dynamic_bus_instance_t cp_dynamic_i2c_buses[];
extern const size_t cp_dynamic_i2c_bus_count;
extern dynamic_bus_state_t cp_dynamic_i2c_bus_states[];

extern const dynamic_bus_instance_t cp_dynamic_spi_buses[];
extern const size_t cp_dynamic_spi_bus_count;
extern dynamic_bus_state_t cp_dynamic_spi_bus_states[];

extern const dynamic_bus_instance_t cp_dynamic_uart_buses[];
extern const size_t cp_dynamic_uart_bus_count;
extern dynamic_bus_state_t cp_dynamic_uart_bus_states[];

#endif // CONFIG_PINCTRL

// Map a GPIO controller device to its hardware port index (0 for gpio0, 2 for
// gpio2, ...). Generated for nRF SoCs in board.c. Returns -1 if unknown.
#if defined(CONFIG_PINCTRL_NRF)
int cp_gpio_port_index(const struct device *port);
#endif

// The functions below return 0 on success and set *dev_out to the Zephyr
// device of an allocated instance. A negative errno is returned on failure:
//   -ENODEV: no compatible instance is free
//   -ENOSYS: dynamic pin routing is unsupported on this SoC
//   -EINVAL/-EIO: a pin or routing operation failed
// Optional pins may be NULL (left disconnected).
int dynamic_bus_i2c_allocate(const mcu_pin_obj_t *sda, const mcu_pin_obj_t *scl,
    const struct device **dev_out);
int dynamic_bus_spi_allocate(const mcu_pin_obj_t *clock, const mcu_pin_obj_t *mosi,
    const mcu_pin_obj_t *miso, const struct device **dev_out);
int dynamic_bus_uart_allocate(const mcu_pin_obj_t *tx, const mcu_pin_obj_t *rx,
    const mcu_pin_obj_t *rts, const mcu_pin_obj_t *cts, const struct device **dev_out);

// Release an instance previously returned by one of the allocate functions.
// Returns true when the instance had been dynamically routed, in which case
// the caller is responsible for resetting its pins (via reset_pin()). A
// non-routed (devicetree-matched) instance is cycled through deinit/init so
// that it is left in the state its devicetree definition describes.
bool dynamic_bus_release(const struct device *dev);

// Keep an allocated instance across soft reloads.
void dynamic_bus_never_reset(const struct device *dev);

// Release all allocated instances (except never-reset ones). Called from
// reset_all_pins() on soft reload.
void dynamic_bus_reset_all(void);
