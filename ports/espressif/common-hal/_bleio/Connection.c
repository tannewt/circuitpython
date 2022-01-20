/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2018 Dan Halbert for Adafruit Industries
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

#include "shared-bindings/_bleio/Connection.h"

#include <string.h>
#include <stdio.h>

#include "shared/runtime/interrupt_char.h"
#include "py/gc.h"
#include "py/objlist.h"
#include "py/objstr.h"
#include "py/qstr.h"
#include "py/runtime.h"
#include "shared-bindings/_bleio/__init__.h"
#include "shared-bindings/_bleio/Adapter.h"
#include "shared-bindings/_bleio/Attribute.h"
#include "shared-bindings/_bleio/Characteristic.h"
#include "shared-bindings/_bleio/Service.h"
#include "shared-bindings/_bleio/UUID.h"
#include "supervisor/shared/tick.h"

#include "common-hal/_bleio/bonding.h"


bool common_hal_bleio_connection_get_paired(bleio_connection_obj_t *self) {
    if (self->connection == NULL) {
        return false;
    }
    return self->connection->pair_status == PAIR_PAIRED;
}

bool common_hal_bleio_connection_get_connected(bleio_connection_obj_t *self) {
    if (self->connection == NULL) {
        return false;
    }
    return self->connection->conn_handle != BLE_CONN_HANDLE_INVALID;
}

void common_hal_bleio_connection_disconnect(bleio_connection_internal_t *self) {
}

void common_hal_bleio_connection_pair(bleio_connection_internal_t *self, bool bond) {

}

mp_float_t common_hal_bleio_connection_get_connection_interval(bleio_connection_internal_t *self) {
    while (self->conn_params_updating && !mp_hal_is_interrupted()) {
        RUN_BACKGROUND_TASKS;
    }
    return 1.25f * self->conn_params.min_conn_interval;
}

// Return the current negotiated MTU length, minus overhead.
mp_int_t common_hal_bleio_connection_get_max_packet_length(bleio_connection_internal_t *self) {
    return (self->mtu == 0 ? BLE_GATT_ATT_MTU_DEFAULT : self->mtu) - 3;
}

void common_hal_bleio_connection_set_connection_interval(bleio_connection_internal_t *self, mp_float_t new_interval) {
    self->conn_params_updating = true;
    uint16_t interval = new_interval / 1.25f;
    self->conn_params.min_conn_interval = interval;
    self->conn_params.max_conn_interval = interval;
}

mp_obj_tuple_t *common_hal_bleio_connection_discover_remote_services(bleio_connection_obj_t *self, mp_obj_t service_uuids_whitelist) {
    discover_remote_services(self->connection, service_uuids_whitelist);
    bleio_connection_ensure_connected(self);
    // Convert to a tuple and then clear the list so the callee will take ownership.
    mp_obj_tuple_t *services_tuple =
        mp_obj_new_tuple(self->connection->remote_service_list->len,
            self->connection->remote_service_list->items);
    mp_obj_list_clear(MP_OBJ_FROM_PTR(self->connection->remote_service_list));

    return services_tuple;
}

uint16_t bleio_connection_get_conn_handle(bleio_connection_obj_t *self) {
    if (self == NULL || self->connection == NULL) {
        return BLE_CONN_HANDLE_INVALID;
    }
    return self->connection->conn_handle;
}

mp_obj_t bleio_connection_new_from_internal(bleio_connection_internal_t *internal) {
    if (internal->connection_obj != mp_const_none) {
        return internal->connection_obj;
    }
    bleio_connection_obj_t *connection = m_new_obj(bleio_connection_obj_t);
    connection->base.type = &bleio_connection_type;
    connection->connection = internal;
    internal->connection_obj = connection;

    return MP_OBJ_FROM_PTR(connection);
}

// Find the connection that uses the given conn_handle. Return NULL if not found.
bleio_connection_internal_t *bleio_conn_handle_to_connection(uint16_t conn_handle) {
    bleio_connection_internal_t *connection;
    for (size_t i = 0; i < BLEIO_TOTAL_CONNECTION_COUNT; i++) {
        connection = &bleio_connections[i];
        if (connection->conn_handle == conn_handle) {
            return connection;
        }
    }

    return NULL;
}
