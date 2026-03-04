// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2019 Dan Halbert for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2026 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/sys/slist.h>

#include "py/runtime.h"
#include "bindings/zephyr_kernel/__init__.h"
#include "shared-bindings/_bleio/__init__.h"
#include "shared-bindings/_bleio/Characteristic.h"
#include "shared-bindings/_bleio/Connection.h"
#include "shared-bindings/_bleio/Service.h"
#include "shared-bindings/_bleio/UUID.h"
#include "common-hal/_bleio/__init__.h"
#include "common-hal/_bleio/Characteristic.h"
#include "supervisor/port_heap.h"
#include "supervisor/shared/tick.h"

// Discovery context passed through Zephyr callbacks.
typedef struct {
    volatile bool done;
    volatile int err;
} discovery_context_t;

// Convert a Zephyr bt_uuid to a CircuitPython bleio UUID object.
// Must be called OUTSIDE Zephyr callback context.
static bleio_uuid_obj_t *bleio_uuid_from_zephyr(const struct bt_uuid *zuuid) {
    bleio_uuid_obj_t *uuid = mp_obj_malloc(bleio_uuid_obj_t, &bleio_uuid_type);
    if (zuuid->type == BT_UUID_TYPE_16) {
        common_hal_bleio_uuid_construct(uuid, BT_UUID_16(zuuid)->val, NULL);
    } else {
        common_hal_bleio_uuid_construct(uuid, 0, BT_UUID_128(zuuid)->val);
    }
    return uuid;
}

// Temporary storage for discovered services before creating CP objects.
// Each node is port_malloc'd in the callback and appended to a sys_slist.
typedef struct {
    sys_snode_t node;
    struct bt_uuid_128 uuid;
    uint16_t start_handle;
    uint16_t end_handle;
} discovered_service_t;

// Temporary storage for discovered characteristics before creating CP objects.
typedef struct {
    sys_snode_t node;
    struct bt_uuid_128 uuid;
    uint16_t value_handle;
    uint8_t properties;
} discovered_char_t;

// File-scope discovery state for synchronous blocking.
static discovery_context_t *active_discovery_ctx;
static sys_slist_t discovered_list;
static struct bt_gatt_discover_params discovery_params;

static uint8_t on_service_discovered(struct bt_conn *conn,
    const struct bt_gatt_attr *attr,
    struct bt_gatt_discover_params *params) {
    discovery_context_t *ctx = active_discovery_ctx;

    if (attr == NULL) {
        ctx->done = true;
        return BT_GATT_ITER_STOP;
    }

    discovered_service_t *ds = port_malloc(sizeof(discovered_service_t), false);
    if (ds == NULL) {
        ctx->done = true;
        return BT_GATT_ITER_STOP;
    }

    struct bt_gatt_service_val *svc_val = (struct bt_gatt_service_val *)attr->user_data;
    ds->start_handle = attr->handle;
    ds->end_handle = svc_val->end_handle;

    // Copy UUID into our storage
    if (svc_val->uuid->type == BT_UUID_TYPE_16) {
        ds->uuid.uuid.type = BT_UUID_TYPE_16;
        ((struct bt_uuid_16 *)&ds->uuid)->val = BT_UUID_16(svc_val->uuid)->val;
    } else {
        memcpy(&ds->uuid, svc_val->uuid, sizeof(struct bt_uuid_128));
    }

    sys_slist_append(&discovered_list, &ds->node);

    return BT_GATT_ITER_CONTINUE;
}

static uint8_t on_characteristic_discovered(struct bt_conn *conn,
    const struct bt_gatt_attr *attr,
    struct bt_gatt_discover_params *params) {
    discovery_context_t *ctx = active_discovery_ctx;

    if (attr == NULL) {
        ctx->done = true;
        return BT_GATT_ITER_STOP;
    }

    discovered_char_t *dc = port_malloc(sizeof(discovered_char_t), false);
    if (dc == NULL) {
        ctx->done = true;
        return BT_GATT_ITER_STOP;
    }

    struct bt_gatt_chrc *chrc = (struct bt_gatt_chrc *)attr->user_data;
    dc->value_handle = chrc->value_handle;
    dc->properties = chrc->properties;

    // Copy UUID into our storage
    if (chrc->uuid->type == BT_UUID_TYPE_16) {
        dc->uuid.uuid.type = BT_UUID_TYPE_16;
        ((struct bt_uuid_16 *)&dc->uuid)->val = BT_UUID_16(chrc->uuid)->val;
    } else {
        memcpy(&dc->uuid, chrc->uuid, sizeof(struct bt_uuid_128));
    }

    sys_slist_append(&discovered_list, &dc->node);

    return BT_GATT_ITER_CONTINUE;
}

// Create CircuitPython characteristic objects from the discovered_list.
// Called OUTSIDE the Zephyr callback context so MP allocations are safe.
// Drains and frees all nodes from the list.
static void create_characteristics_from_discovered(bleio_service_obj_t *service) {
    sys_snode_t *node;
    while ((node = sys_slist_get(&discovered_list)) != NULL) {
        discovered_char_t *dc = CONTAINER_OF(node, discovered_char_t, node);

        bleio_uuid_obj_t *uuid = bleio_uuid_from_zephyr(&dc->uuid.uuid);

        bleio_characteristic_properties_t props = 0;
        if (dc->properties & BT_GATT_CHRC_BROADCAST) {
            props |= CHAR_PROP_BROADCAST;
        }
        if (dc->properties & BT_GATT_CHRC_READ) {
            props |= CHAR_PROP_READ;
        }
        if (dc->properties & BT_GATT_CHRC_WRITE_WITHOUT_RESP) {
            props |= CHAR_PROP_WRITE_NO_RESPONSE;
        }
        if (dc->properties & BT_GATT_CHRC_WRITE) {
            props |= CHAR_PROP_WRITE;
        }
        if (dc->properties & BT_GATT_CHRC_NOTIFY) {
            props |= CHAR_PROP_NOTIFY;
        }
        if (dc->properties & BT_GATT_CHRC_INDICATE) {
            props |= CHAR_PROP_INDICATE;
        }

        bleio_characteristic_obj_t *characteristic =
            mp_obj_malloc(bleio_characteristic_obj_t, &bleio_characteristic_type);

        // Use max_length=20 for remote chars - reads go over the wire.
        common_hal_bleio_characteristic_construct(
            characteristic, service,
            dc->value_handle, uuid, props,
            SECURITY_MODE_OPEN, SECURITY_MODE_OPEN,
            20, false, NULL, NULL);

        port_free(dc);
    }
}

// Helper to drain and free all nodes from the discovered_list.
static void free_discovered_list(void) {
    sys_snode_t *node;
    while ((node = sys_slist_get(&discovered_list)) != NULL) {
        port_free(node);
    }
}

// Discover characteristics for a single remote service.
static void discover_characteristics_for_service(struct bt_conn *conn,
    discovery_context_t *ctx, bleio_service_obj_t *service) {
    // Need at least 2 handles: one for the service declaration, one for a characteristic
    if (service->end_handle <= service->start_handle) {
        return;
    }

    sys_slist_init(&discovered_list);
    ctx->done = false;
    ctx->err = 0;

    memset(&discovery_params, 0, sizeof(discovery_params));
    discovery_params.uuid = NULL;
    discovery_params.start_handle = service->start_handle + 1;
    discovery_params.end_handle = service->end_handle;
    discovery_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;
    discovery_params.func = on_characteristic_discovered;

    int err = bt_gatt_discover(conn, &discovery_params);
    if (err != 0) {
        free_discovered_list();
        // -ENOENT means no characteristics found in the range, which is fine.
        if (err == -ENOENT) {
            return;
        }
        raise_zephyr_error(err);
    }

    while (!ctx->done) {
        RUN_BACKGROUND_TASKS;
    }

    // Create CP objects outside of callback context where MP allocations are safe.
    // This drains and frees the list nodes.
    create_characteristics_from_discovered(service);
}

void common_hal_bleio_connection_pair(bleio_connection_internal_t *self, bool bond) {
    mp_raise_NotImplementedError(NULL);
}

void common_hal_bleio_connection_disconnect(bleio_connection_internal_t *self) {
    if (self == NULL || self->conn == NULL) {
        return;
    }

    int err = bt_conn_disconnect(self->conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    if (err != 0 && err != -ENOTCONN) {
        raise_zephyr_error(err);
    }

    // The connection may now be disconnecting; force connections tuple rebuild.
    common_hal_bleio_adapter_obj.connection_objs = NULL;
}

bool common_hal_bleio_connection_get_connected(bleio_connection_obj_t *self) {
    if (self == NULL || self->connection == NULL) {
        return false;
    }

    bleio_connection_internal_t *connection = self->connection;
    if (connection->conn == NULL) {
        return false;
    }

    struct bt_conn_info info;
    if (bt_conn_get_info(connection->conn, &info) != 0) {
        return false;
    }

    return info.state == BT_CONN_STATE_CONNECTED || info.state == BT_CONN_STATE_DISCONNECTING;
}

mp_int_t common_hal_bleio_connection_get_max_packet_length(bleio_connection_internal_t *self) {
    if (self == NULL || self->conn == NULL) {
        return 20;
    }

    uint16_t mtu = bt_gatt_get_mtu(self->conn);
    if (mtu < 3) {
        return 20;
    }
    return mtu - 3;
}

bool common_hal_bleio_connection_get_paired(bleio_connection_obj_t *self) {
    return false;
}

mp_obj_tuple_t *common_hal_bleio_connection_discover_remote_services(bleio_connection_obj_t *self, mp_obj_t service_uuids_whitelist) {
    bleio_connection_internal_t *connection = self->connection;
    if (connection == NULL || connection->conn == NULL) {
        mp_raise_bleio_BluetoothError(MP_ERROR_TEXT("Not connected"));
    }

    discovery_context_t ctx;
    ctx.done = false;
    ctx.err = 0;

    active_discovery_ctx = &ctx;
    sys_slist_init(&discovered_list);

    // Discover primary services (callback appends nodes to discovered_list)
    memset(&discovery_params, 0, sizeof(discovery_params));
    discovery_params.uuid = NULL;
    discovery_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
    discovery_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
    discovery_params.type = BT_GATT_DISCOVER_PRIMARY;
    discovery_params.func = on_service_discovered;

    int err = bt_gatt_discover(connection->conn, &discovery_params);
    if (err != 0) {
        free_discovered_list();
        active_discovery_ctx = NULL;
        raise_zephyr_error(err);
    }

    while (!ctx.done) {
        RUN_BACKGROUND_TASKS;
    }

    // Create CP service objects outside callback context where MP allocations are safe.
    // Drain and free the discovered service nodes.
    mp_obj_list_t *service_list = mp_obj_new_list(0, NULL);
    sys_snode_t *snode;
    while ((snode = sys_slist_get(&discovered_list)) != NULL) {
        discovered_service_t *ds = CONTAINER_OF(snode, discovered_service_t, node);
        bleio_uuid_obj_t *uuid = bleio_uuid_from_zephyr(&ds->uuid.uuid);
        bleio_service_obj_t *service = mp_obj_malloc(bleio_service_obj_t, &bleio_service_type);
        common_hal_bleio_service_from_remote_service(service, self, uuid, false);
        service->start_handle = ds->start_handle;
        service->end_handle = ds->end_handle;
        mp_obj_list_append(MP_OBJ_FROM_PTR(service_list), MP_OBJ_FROM_PTR(service));
        port_free(ds);
    }

    // Filter by whitelist if provided
    mp_obj_list_t *result_list = service_list;
    if (service_uuids_whitelist != mp_const_none) {
        mp_obj_list_t *filtered = mp_obj_new_list(0, NULL);
        mp_obj_iter_buf_t iter_buf;
        mp_obj_t iterable = mp_getiter(service_uuids_whitelist, &iter_buf);
        mp_obj_t uuid_obj;
        while ((uuid_obj = mp_iternext(iterable)) != MP_OBJ_STOP_ITERATION) {
            bleio_uuid_obj_t *filter_uuid = mp_arg_validate_type(uuid_obj, &bleio_uuid_type, MP_QSTR_uuid);
            for (size_t i = 0; i < result_list->len; i++) {
                bleio_service_obj_t *svc = MP_OBJ_TO_PTR(result_list->items[i]);
                if (common_hal_bleio_uuid_get_size(svc->uuid) == common_hal_bleio_uuid_get_size(filter_uuid)) {
                    uint8_t svc_uuid128[16], filter_uuid128[16];
                    common_hal_bleio_uuid_get_uuid128(svc->uuid, svc_uuid128);
                    common_hal_bleio_uuid_get_uuid128(filter_uuid, filter_uuid128);
                    if (memcmp(svc_uuid128, filter_uuid128, 16) == 0) {
                        mp_obj_list_append(MP_OBJ_FROM_PTR(filtered), result_list->items[i]);
                    }
                }
            }
        }
        result_list = filtered;
    }

    // Discover characteristics for each service
    for (size_t i = 0; i < result_list->len; i++) {
        bleio_service_obj_t *svc = MP_OBJ_TO_PTR(result_list->items[i]);
        if (svc->start_handle < svc->end_handle) {
            discover_characteristics_for_service(connection->conn, &ctx, svc);
        }
    }

    active_discovery_ctx = NULL;

    return mp_obj_new_tuple(result_list->len, result_list->items);
}

mp_float_t common_hal_bleio_connection_get_connection_interval(bleio_connection_internal_t *self) {
    mp_raise_NotImplementedError(NULL);
}

void common_hal_bleio_connection_set_connection_interval(bleio_connection_internal_t *self, mp_float_t new_interval) {
    mp_raise_NotImplementedError(NULL);
}

mp_obj_t bleio_connection_new_from_internal(bleio_connection_internal_t *connection) {
    if (connection == NULL) {
        return mp_const_none;
    }

    if (connection->connection_obj != mp_const_none) {
        return connection->connection_obj;
    }

    bleio_connection_obj_t *connection_obj = mp_obj_malloc(bleio_connection_obj_t, &bleio_connection_type);
    connection_obj->connection = connection;
    connection_obj->disconnect_reason = 0;
    connection->connection_obj = MP_OBJ_FROM_PTR(connection_obj);

    return connection->connection_obj;
}
