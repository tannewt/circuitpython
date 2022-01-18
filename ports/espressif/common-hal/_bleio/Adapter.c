/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2018 Dan Halbert for Adafruit Industries
 * Copyright (c) 2016 Glenn Ruben Bakke
 * Copyright (c) 2018 Artur Pacholec
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

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "py/gc.h"
#include "py/objstr.h"
#include "py/runtime.h"
#include "supervisor/shared/bluetooth/bluetooth.h"
#include "supervisor/shared/safe_mode.h"
#include "supervisor/shared/tick.h"
#include "supervisor/usb.h"
#include "shared-bindings/_bleio/__init__.h"
#include "shared-bindings/_bleio/Adapter.h"
#include "shared-bindings/_bleio/Address.h"
#include "shared-bindings/nvm/ByteArray.h"
#include "shared-bindings/_bleio/Connection.h"
#include "shared-bindings/_bleio/ScanEntry.h"
#include "shared-bindings/time/__init__.h"

bleio_connection_internal_t bleio_connections[BLEIO_TOTAL_CONNECTION_COUNT];

static void bluetooth_adapter_background(void *data) {
    supervisor_bluetooth_background();
    bleio_background();
}

void common_hal_bleio_adapter_set_enabled(bleio_adapter_obj_t *self, bool enabled) {
    const bool is_enabled = common_hal_bleio_adapter_get_enabled(self);

    // Don't enable or disable twice
    if (is_enabled == enabled) {
        return;
    }
}

bool common_hal_bleio_adapter_get_enabled(bleio_adapter_obj_t *self) {

    return false;
}

bleio_address_obj_t *common_hal_bleio_adapter_get_address(bleio_adapter_obj_t *self) {
    return NULL;
}

bool common_hal_bleio_adapter_set_address(bleio_adapter_obj_t *self, bleio_address_obj_t *address) {
    return false;
}

mp_obj_str_t *common_hal_bleio_adapter_get_name(bleio_adapter_obj_t *self) {
    return NULL;
}

void common_hal_bleio_adapter_set_name(bleio_adapter_obj_t *self, const char *name) {
}

mp_obj_t common_hal_bleio_adapter_start_scan(bleio_adapter_obj_t *self, uint8_t *prefixes, size_t prefix_length, bool extended, mp_int_t buffer_size, mp_float_t timeout, mp_float_t interval, mp_float_t window, mp_int_t minimum_rssi, bool active) {
    if (self->scan_results != NULL) {
        if (!shared_module_bleio_scanresults_get_done(self->scan_results)) {
            mp_raise_bleio_BluetoothError(translate("Scan already in progess. Stop with stop_scan."));
        }
        self->scan_results = NULL;
    }

    self->scan_results = shared_module_bleio_new_scanresults(buffer_size, prefixes, prefix_length, minimum_rssi);
    size_t max_packet_size = extended ? BLE_GAP_SCAN_BUFFER_EXTENDED_MAX_SUPPORTED : BLE_GAP_SCAN_BUFFER_MAX;

    return MP_OBJ_FROM_PTR(self->scan_results);
}

void common_hal_bleio_adapter_stop_scan(bleio_adapter_obj_t *self) {
    if (self->scan_results == NULL) {
        return;
    }
    // stop
    shared_module_bleio_scanresults_set_done(self->scan_results, true);
    // remove event handler
    self->scan_results = NULL;
}

mp_obj_t common_hal_bleio_adapter_connect(bleio_adapter_obj_t *self, bleio_address_obj_t *address, mp_float_t timeout) {
    mp_raise_bleio_BluetoothError(translate("Failed to connect: internal error"));

    return mp_const_none;
}

uint32_t _common_hal_bleio_adapter_start_advertising(bleio_adapter_obj_t *self,
    bool connectable, bool anonymous, uint32_t timeout, float interval,
    const uint8_t *advertising_data, uint16_t advertising_data_len,
    const uint8_t *scan_response_data, uint16_t scan_response_data_len,
    mp_int_t tx_power, const bleio_address_obj_t *directed_to) {

    return -1;
}


void common_hal_bleio_adapter_start_advertising(bleio_adapter_obj_t *self, bool connectable,
    bool anonymous, uint32_t timeout, mp_float_t interval,
    mp_buffer_info_t *advertising_data_bufinfo, mp_buffer_info_t *scan_response_data_bufinfo,
    mp_int_t tx_power, const bleio_address_obj_t *directed_to) {
}

void common_hal_bleio_adapter_stop_advertising(bleio_adapter_obj_t *self) {
}

bool common_hal_bleio_adapter_get_advertising(bleio_adapter_obj_t *self) {
    return self->current_advertising_data != NULL;
}

bool common_hal_bleio_adapter_get_connected(bleio_adapter_obj_t *self) {
    for (size_t i = 0; i < BLEIO_TOTAL_CONNECTION_COUNT; i++) {
        bleio_connection_internal_t *connection = &bleio_connections[i];
        if (connection->conn_handle != BLE_CONN_HANDLE_INVALID) {
            return true;
        }
    }
    return false;
}

mp_obj_t common_hal_bleio_adapter_get_connections(bleio_adapter_obj_t *self) {
    if (self->connection_objs != NULL) {
        return self->connection_objs;
    }
    size_t total_connected = 0;
    mp_obj_t items[BLEIO_TOTAL_CONNECTION_COUNT];
    for (size_t i = 0; i < BLEIO_TOTAL_CONNECTION_COUNT; i++) {
        bleio_connection_internal_t *connection = &bleio_connections[i];
        if (connection->conn_handle != BLE_CONN_HANDLE_INVALID) {
            if (connection->connection_obj == mp_const_none) {
                connection->connection_obj = bleio_connection_new_from_internal(connection);
            }
            items[total_connected] = connection->connection_obj;
            total_connected++;
        }
    }
    self->connection_objs = mp_obj_new_tuple(total_connected, items);
    return self->connection_objs;
}

void common_hal_bleio_adapter_erase_bonding(bleio_adapter_obj_t *self) {
    bonding_erase_storage();
}

bool common_hal_bleio_adapter_is_bonded_to_central(bleio_adapter_obj_t *self) {
    return bonding_peripheral_bond_count() > 0;
}

void bleio_adapter_gc_collect(bleio_adapter_obj_t *adapter) {
    // We divide by size_t so that we can scan each 32-bit aligned value to see
    // if it is a pointer. This allows us to change the structs without worrying
    // about collecting new pointers.
    gc_collect_root((void **)adapter, sizeof(bleio_adapter_obj_t) / (sizeof(size_t)));
    gc_collect_root((void **)bleio_connections, sizeof(bleio_connections) / (sizeof(size_t)));
}

void bleio_adapter_reset(bleio_adapter_obj_t *adapter) {
    common_hal_bleio_adapter_stop_scan(adapter);
    if (adapter->current_advertising_data != NULL) {
        common_hal_bleio_adapter_stop_advertising(adapter);
    }

    adapter->connection_objs = NULL;
    for (size_t i = 0; i < BLEIO_TOTAL_CONNECTION_COUNT; i++) {
        bleio_connection_internal_t *connection = &bleio_connections[i];
        // Disconnect all connections cleanly.
        if (connection->conn_handle != BLE_CONN_HANDLE_INVALID) {
            common_hal_bleio_connection_disconnect(connection);
        }
        connection->connection_obj = mp_const_none;
    }

    // Wait up to 125 ms (128 ticks) for disconnect to complete. This should be
    // greater than most connection intervals.
    bool any_connected = false;
    uint64_t start_ticks = supervisor_ticks_ms64();
    while (any_connected && supervisor_ticks_ms64() - start_ticks < 128) {
        any_connected = false;
        for (size_t i = 0; i < BLEIO_TOTAL_CONNECTION_COUNT; i++) {
            bleio_connection_internal_t *connection = &bleio_connections[i];
            any_connected |= connection->conn_handle != BLE_CONN_HANDLE_INVALID;
        }
    }
}
