// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Hackaday
//
// SPDX-License-Identifier: MIT

#include "shared-bindings/itc97/ITC97Bus.h"
#include "shared-module/itc97/ITC97Bus.h"

#include <string.h>

#include "py/mphal.h"
#include "py/runtime.h"
#include "shared-bindings/busio/SPI.h"
#include "shared-bindings/digitalio/DigitalInOut.h"
#include "shared-bindings/microcontroller/Pin.h"
#include "supervisor/shared/tick.h"

#define ITC97_PLANE_CHUNK (60)
#define ITC97_REGISTER_BAUDRATE (50000)

static void itc97_set_power(itc97_itc97bus_obj_t *self, bool off) {
    if (self->power.pin != NULL) {
        // The panel power switch is active-low.
        common_hal_digitalio_digitalinout_set_value(&self->power, off);
    }
}

static void itc97_set_chip_selects(itc97_itc97bus_obj_t *self, bool cs1, bool cs2) {
    common_hal_digitalio_digitalinout_set_value(&self->chip_select_1, cs1);
    common_hal_digitalio_digitalinout_set_value(&self->chip_select_2, cs2);
}

static void itc97_shutdown(itc97_itc97bus_obj_t *self) {
    mp_hal_delay_ms(200);
    common_hal_digitalio_digitalinout_set_value(&self->command, false);
    itc97_set_chip_selects(self, false, false);
    mp_hal_delay_ms(150);
    common_hal_digitalio_digitalinout_set_value(&self->reset, false);
    itc97_set_power(self, true);
    mp_hal_delay_ms(300);
    itc97_set_chip_selects(self, true, true);
    common_hal_digitalio_digitalinout_set_value(&self->reset, true);
    self->last_command = 0;
    self->plane_active = false;
}

void common_hal_itc97_itc97bus_set_busy_pins(itc97_itc97bus_obj_t *self,
    digitalio_digitalinout_obj_t *busy_1, digitalio_digitalinout_obj_t *busy_2, bool busy_state) {
    self->busy_1 = busy_1;
    self->busy_2 = busy_2;
    self->busy_state = busy_state;
}

// Called with the SPI lock held and both chip selects high. These are the only
// BUSY waits needed during initialization: after the complete booster ramp, and
// for BUSY assertion after the trigger. Waiting between ramp writes deadlocks.
static void itc97_wait_for_busy(itc97_itc97bus_obj_t *self, bool active, uint32_t timeout_ms) {
    uint64_t start = supervisor_ticks_ms64();
    while (true) {
        bool busy1 = self->busy_1 != NULL &&
            common_hal_digitalio_digitalinout_get_value(self->busy_1) == self->busy_state;
        bool busy2 = self->busy_2 != NULL &&
            common_hal_digitalio_digitalinout_get_value(self->busy_2) == self->busy_state;
        bool ready = active ?
            ((self->busy_1 == NULL || busy1) && (self->busy_2 == NULL || busy2)) :
            (!busy1 && !busy2);
        if ((self->busy_1 != NULL || self->busy_2 != NULL) && ready) {
            return;
        }
        if (mp_hal_is_interrupted() || supervisor_ticks_ms64() - start >= timeout_ms) {
            itc97_shutdown(self);
            common_hal_busio_spi_unlock(&self->bus);
            mp_raise_RuntimeError(active ? MP_ERROR_TEXT("ITC97 BUSY did not assert after trigger") :
                MP_ERROR_TEXT("ITC97 booster did not become ready"));
        }
        mp_hal_delay_ms(1);
    }
}

void common_hal_itc97_itc97bus_construct(itc97_itc97bus_obj_t *self,
    const mcu_pin_obj_t *sck, const mcu_pin_obj_t *mosi,
    const mcu_pin_obj_t *chip_select_1, const mcu_pin_obj_t *chip_select_2,
    const mcu_pin_obj_t *command, const mcu_pin_obj_t *reset,
    const mcu_pin_obj_t *power, uint32_t baudrate, uint8_t polarity, uint8_t phase) {

    common_hal_busio_spi_construct(&self->bus, sck, mosi, NULL, false);

    self->frequency = baudrate;
    self->polarity = polarity;
    self->phase = phase;
    self->last_command = 0;
    self->plane_active = false;
    self->plane_bytes_sent = 0;
    self->ram_pointer_valid = false;
    self->busy_1 = NULL;
    self->busy_2 = NULL;

    common_hal_digitalio_digitalinout_construct(&self->chip_select_1, chip_select_1);
    common_hal_digitalio_digitalinout_switch_to_output(&self->chip_select_1, true, DRIVE_MODE_PUSH_PULL);
    common_hal_digitalio_digitalinout_construct(&self->chip_select_2, chip_select_2);
    common_hal_digitalio_digitalinout_switch_to_output(&self->chip_select_2, true, DRIVE_MODE_PUSH_PULL);
    common_hal_digitalio_digitalinout_construct(&self->command, command);
    common_hal_digitalio_digitalinout_switch_to_output(&self->command, false, DRIVE_MODE_PUSH_PULL);
    common_hal_digitalio_digitalinout_construct(&self->reset, reset);
    common_hal_digitalio_digitalinout_switch_to_output(&self->reset, true, DRIVE_MODE_PUSH_PULL);

    if (power != NULL) {
        common_hal_digitalio_digitalinout_construct(&self->power, power);
        common_hal_digitalio_digitalinout_switch_to_output(&self->power, true, DRIVE_MODE_PUSH_PULL);
    } else {
        self->power.pin = NULL;
    }

    #if CIRCUITPY_BULK_RESET
    common_hal_never_reset_pin(sck);
    common_hal_never_reset_pin(mosi);
    common_hal_never_reset_pin(chip_select_1);
    common_hal_never_reset_pin(chip_select_2);
    common_hal_never_reset_pin(command);
    common_hal_never_reset_pin(reset);
    if (power != NULL) {
        common_hal_never_reset_pin(power);
    }
    common_hal_busio_spi_never_reset(&self->bus);
    #endif
}

void common_hal_itc97_itc97bus_deinit(itc97_itc97bus_obj_t *self) {
    if (self->base.type != &itc97_itc97bus_type) {
        return;
    }

    itc97_shutdown(self);
    common_hal_busio_spi_deinit(&self->bus);
    common_hal_digitalio_digitalinout_deinit(&self->chip_select_1);
    common_hal_digitalio_digitalinout_deinit(&self->chip_select_2);
    common_hal_digitalio_digitalinout_deinit(&self->command);
    common_hal_digitalio_digitalinout_deinit(&self->reset);
    if (self->power.pin != NULL) {
        common_hal_digitalio_digitalinout_deinit(&self->power);
    }
    memset(self, 0, sizeof(*self));
}

bool common_hal_itc97_itc97bus_reset(mp_obj_t self_in) {
    itc97_itc97bus_obj_t *self = MP_OBJ_TO_PTR(self_in);

    // The booster rails need at least 1500 ms to bleed down between power cycles.
    itc97_set_power(self, true);
    itc97_set_chip_selects(self, true, true);
    common_hal_digitalio_digitalinout_set_value(&self->command, false);
    common_hal_digitalio_digitalinout_set_value(&self->reset, true);
    mp_hal_delay_ms(1500);

    itc97_set_power(self, false);
    mp_hal_delay_ms(800);

    common_hal_digitalio_digitalinout_set_value(&self->reset, true);
    mp_hal_delay_ms(50);
    common_hal_digitalio_digitalinout_set_value(&self->reset, false);
    mp_hal_delay_ms(100);
    common_hal_digitalio_digitalinout_set_value(&self->reset, true);
    mp_hal_delay_ms(400);

    itc97_set_chip_selects(self, true, true);
    common_hal_digitalio_digitalinout_set_value(&self->command, false);
    self->last_command = 0;
    self->plane_active = false;
    self->plane_bytes_sent = 0;
    self->ram_pointer_valid = false;
    return true;
}

bool common_hal_itc97_itc97bus_bus_free(mp_obj_t self_in) {
    itc97_itc97bus_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!common_hal_busio_spi_try_lock(&self->bus)) {
        return false;
    }
    common_hal_busio_spi_unlock(&self->bus);
    return true;
}

bool common_hal_itc97_itc97bus_begin_transaction(mp_obj_t self_in) {
    itc97_itc97bus_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!common_hal_busio_spi_try_lock(&self->bus)) {
        return false;
    }
    itc97_set_chip_selects(self, true, true);
    common_hal_digitalio_digitalinout_set_value(&self->command, false);
    return true;
}

void common_hal_itc97_itc97bus_send(mp_obj_t self_in, display_byte_type_t byte_type,
    display_chip_select_behavior_t chip_select, const uint8_t *data, uint32_t data_length) {
    itc97_itc97bus_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (data_length == 0) {
        return;
    }

    if (byte_type == DISPLAY_COMMAND) {
        // The reference uses a slow register bus and fast SPI only for planes.
        common_hal_busio_spi_configure(&self->bus, ITC97_REGISTER_BAUDRATE, self->polarity, self->phase, 8);
        if (self->plane_active && data[0] != 0x10 && data[0] != 0x11 && data[0] != 0x12) {
            // Let both frame RAM planes settle before the register sequence.
            mp_hal_delay_ms(660);
        }
        if (data[0] == 0x10 || data[0] == 0x11) {
            // RAM_RW must be reloaded before *each* plane, not only at startup.
            if (self->ram_pointer_valid) {
                const uint8_t ram_command = 0x12;
                common_hal_digitalio_digitalinout_set_value(&self->command, false);
                itc97_set_chip_selects(self, false, false);
                common_hal_busio_spi_write(&self->bus, &ram_command, 1);
                itc97_set_chip_selects(self, true, true);
                common_hal_digitalio_digitalinout_set_value(&self->command, true);
                itc97_set_chip_selects(self, false, false);
                common_hal_busio_spi_write(&self->bus, self->ram_pointer, sizeof(self->ram_pointer));
                itc97_set_chip_selects(self, true, true);
            }
        }
        if (data[0] == 0x15) {
            mp_hal_delay_ms(100);
            itc97_wait_for_busy(self, false, 10000);
        }
        common_hal_digitalio_digitalinout_set_value(&self->command, false);
        itc97_set_chip_selects(self, false, false);
        common_hal_busio_spi_write(&self->bus, data, data_length);
        itc97_set_chip_selects(self, true, true);
        self->last_command = data[0];
        self->plane_active = data[0] == 0x10 || data[0] == 0x11;
        if (self->plane_active) {
            self->plane_bytes_sent = 0;
        }
        return;
    }

    if (self->last_command == 0x12 && data_length == sizeof(self->ram_pointer)) {
        memcpy(self->ram_pointer, data, sizeof(self->ram_pointer));
        self->ram_pointer_valid = true;
    }
    common_hal_digitalio_digitalinout_set_value(&self->command, true);

    if (self->plane_active) {
        common_hal_busio_spi_configure(&self->bus, self->frequency, self->polarity, self->phase, 8);
        uint32_t offset = 0;
        while (offset < data_length) {
            uint32_t chunk = data_length - offset;
            uint32_t remaining = ITC97_PLANE_CHUNK - self->plane_bytes_sent % ITC97_PLANE_CHUNK;
            if (chunk > remaining) {
                chunk = remaining;
            }
            if ((self->plane_bytes_sent / ITC97_PLANE_CHUNK) % 2 == 0) {
                itc97_set_chip_selects(self, false, true);
            } else {
                itc97_set_chip_selects(self, true, false);
            }
            common_hal_busio_spi_write(&self->bus, data + offset, chunk);
            itc97_set_chip_selects(self, true, true);
            self->plane_bytes_sent += chunk;
            offset += chunk;
        }
    } else {
        itc97_set_chip_selects(self, false, false);
        common_hal_busio_spi_write(&self->bus, data, data_length);
        itc97_set_chip_selects(self, true, true);
    }

    if (self->last_command == 0x15) {
        // An idle-high sample immediately after the trigger is not completion.
        // Ensure both COGs have entered BUSY before EPaperDisplay can poll them.
        mp_hal_delay_ms(5);
        itc97_wait_for_busy(self, true, 2000);
    }

    // The stop sequence clears register 0x09. Use it as the cue to run the panel
    // shutdown sequence, since EPaperDisplay has no separate bus-stop callback.
    if (self->last_command == 0x09 && data[0] == 0x00) {
        itc97_shutdown(self);
    }
}

void common_hal_itc97_itc97bus_end_transaction(mp_obj_t self_in) {
    itc97_itc97bus_obj_t *self = MP_OBJ_TO_PTR(self_in);
    itc97_set_chip_selects(self, true, true);
    common_hal_busio_spi_unlock(&self->bus);
}

void common_hal_itc97_itc97bus_collect_ptrs(mp_obj_t self_in) {
    (void)self_in;
}
