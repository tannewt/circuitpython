// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2019 Dan Halbert for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2026 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include <string.h>

#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>

#include "py/runtime.h"
#include "bindings/zephyr_kernel/__init__.h"
#include "shared-bindings/_bleio/__init__.h"
#include "shared-bindings/_bleio/Characteristic.h"
#include "shared-bindings/_bleio/Descriptor.h"
#include "shared-bindings/_bleio/Service.h"
#include "common-hal/_bleio/__init__.h"
#include "common-hal/_bleio/Connection.h"
#include "supervisor/shared/tick.h"

// Context for synchronous GATT client read.
typedef struct {
    uint8_t *buf;
    size_t buf_len;
    size_t read_len;
    volatile bool done;
    volatile int err;
} gattc_read_ctx_t;

// File-scope state for synchronous GATT read/write.
static gattc_read_ctx_t *active_read_ctx;
static struct bt_gatt_read_params read_params;

static uint8_t on_gattc_read(struct bt_conn *conn, uint8_t err,
    struct bt_gatt_read_params *params,
    const void *data, uint16_t length) {
    gattc_read_ctx_t *ctx = active_read_ctx;
    if (ctx == NULL) {
        return BT_GATT_ITER_STOP;
    }

    if (err) {
        ctx->err = err;
        ctx->done = true;
        return BT_GATT_ITER_STOP;
    }

    if (data == NULL || length == 0) {
        // End of read
        ctx->done = true;
        return BT_GATT_ITER_STOP;
    }

    size_t copy_len = length;
    if (ctx->read_len + copy_len > ctx->buf_len) {
        copy_len = ctx->buf_len - ctx->read_len;
    }
    if (copy_len > 0) {
        memcpy(ctx->buf + ctx->read_len, data, copy_len);
        ctx->read_len += copy_len;
    }

    ctx->done = true;
    return BT_GATT_ITER_STOP;
}

typedef struct {
    volatile bool done;
    volatile int err;
} gattc_write_ctx_t;

static gattc_write_ctx_t *active_write_ctx;

static void on_gattc_write(struct bt_conn *conn, uint8_t err,
    struct bt_gatt_write_params *params) {
    gattc_write_ctx_t *ctx = active_write_ctx;
    if (ctx == NULL) {
        return;
    }
    ctx->err = err;
    ctx->done = true;
}

static struct bt_gatt_write_params write_params;

uint16_t bleio_security_to_zephyr_perm(
    bleio_attribute_security_mode_t read_perm,
    bleio_attribute_security_mode_t write_perm,
    bleio_characteristic_properties_t props) {
    uint16_t perm = 0;

    if (props & CHAR_PROP_READ) {
        switch (read_perm) {
            case SECURITY_MODE_OPEN:
                perm |= BT_GATT_PERM_READ;
                break;
            case SECURITY_MODE_ENC_NO_MITM:
                perm |= BT_GATT_PERM_READ_ENCRYPT;
                break;
            case SECURITY_MODE_ENC_WITH_MITM:
                perm |= BT_GATT_PERM_READ_AUTHEN;
                break;
            case SECURITY_MODE_LESC_ENC_WITH_MITM:
                perm |= BT_GATT_PERM_READ_LESC;
                break;
            default:
                break;
        }
    }

    if (props & (CHAR_PROP_WRITE | CHAR_PROP_WRITE_NO_RESPONSE)) {
        switch (write_perm) {
            case SECURITY_MODE_OPEN:
                perm |= BT_GATT_PERM_WRITE;
                break;
            case SECURITY_MODE_ENC_NO_MITM:
                perm |= BT_GATT_PERM_WRITE_ENCRYPT;
                break;
            case SECURITY_MODE_ENC_WITH_MITM:
                perm |= BT_GATT_PERM_WRITE_AUTHEN;
                break;
            case SECURITY_MODE_LESC_ENC_WITH_MITM:
                perm |= BT_GATT_PERM_WRITE_LESC;
                break;
            default:
                break;
        }
    }

    return perm;
}

ssize_t bleio_char_read_cb(struct bt_conn *conn,
    const struct bt_gatt_attr *attr, void *buf, uint16_t len, uint16_t offset) {
    bleio_characteristic_obj_t *self = attr->user_data;
    return bt_gatt_attr_read(conn, attr, buf, len, offset,
        self->current_value, self->current_value_len);
}

ssize_t bleio_char_write_cb(struct bt_conn *conn,
    const struct bt_gatt_attr *attr, const void *buf, uint16_t len,
    uint16_t offset, uint8_t flags) {
    bleio_characteristic_obj_t *self = attr->user_data;
    if (offset + len > self->max_length) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    memcpy(self->current_value + offset, buf, len);
    if (offset + len > self->current_value_len) {
        self->current_value_len = offset + len;
    }
    return len;
}

void bleio_ccc_changed_cb(const struct bt_gatt_attr *attr, uint16_t value) {
    // Track subscription state if needed in the future.
    (void)attr;
    (void)value;
}

bleio_characteristic_properties_t common_hal_bleio_characteristic_get_properties(bleio_characteristic_obj_t *self) {
    return self->props;
}

mp_obj_tuple_t *common_hal_bleio_characteristic_get_descriptors(bleio_characteristic_obj_t *self) {
    return mp_obj_new_tuple(self->descriptor_list->len, self->descriptor_list->items);
}

bleio_service_obj_t *common_hal_bleio_characteristic_get_service(bleio_characteristic_obj_t *self) {
    return self->service;
}

bleio_uuid_obj_t *common_hal_bleio_characteristic_get_uuid(bleio_characteristic_obj_t *self) {
    return self->uuid;
}

size_t common_hal_bleio_characteristic_get_max_length(bleio_characteristic_obj_t *self) {
    return self->max_length;
}

size_t common_hal_bleio_characteristic_get_value(bleio_characteristic_obj_t *self, uint8_t *buf, size_t len) {
    if (self->service != NULL && self->service->is_remote) {
        // Remote characteristic: read via GATT client
        bleio_connection_obj_t *connection = MP_OBJ_TO_PTR(self->service->connection);
        if (connection == NULL || connection->connection == NULL ||
            connection->connection->conn == NULL) {
            mp_raise_bleio_BluetoothError(MP_ERROR_TEXT("Not connected"));
        }

        gattc_read_ctx_t ctx = {
            .buf = buf,
            .buf_len = len,
            .read_len = 0,
            .done = false,
            .err = 0,
        };
        active_read_ctx = &ctx;

        memset(&read_params, 0, sizeof(read_params));
        read_params.func = on_gattc_read;
        read_params.handle_count = 1;
        read_params.single.handle = self->handle;
        read_params.single.offset = 0;

        int err = bt_gatt_read(connection->connection->conn, &read_params);
        if (err != 0) {
            active_read_ctx = NULL;
            raise_zephyr_error(err);
        }

        while (!ctx.done) {
            RUN_BACKGROUND_TASKS;
        }
        active_read_ctx = NULL;

        if (ctx.err != 0) {
            raise_zephyr_error(ctx.err);
        }

        return ctx.read_len;
    }

    // Local characteristic
    size_t copy_len = self->current_value_len;
    if (copy_len > len) {
        copy_len = len;
    }
    memcpy(buf, self->current_value, copy_len);
    return copy_len;
}

void common_hal_bleio_characteristic_construct(bleio_characteristic_obj_t *self,
    bleio_service_obj_t *service, uint16_t handle, bleio_uuid_obj_t *uuid,
    bleio_characteristic_properties_t props,
    bleio_attribute_security_mode_t read_perm,
    bleio_attribute_security_mode_t write_perm,
    mp_int_t max_length, bool fixed_length,
    mp_buffer_info_t *initial_value_bufinfo,
    const char *user_description) {

    self->service = service;
    self->uuid = uuid;
    self->handle = handle;
    self->props = props;
    self->read_perm = read_perm;
    self->write_perm = write_perm;
    self->max_length = max_length;
    self->fixed_length = fixed_length;
    self->observer = mp_const_none;
    self->descriptor_list = mp_obj_new_list(0, NULL);

    // Allocate value buffer
    self->current_value = m_malloc(max_length);
    memset(self->current_value, 0, max_length);
    self->current_value_alloc = max_length;
    self->current_value_len = 0;

    // Copy initial value if provided
    if (initial_value_bufinfo != NULL && initial_value_bufinfo->len > 0) {
        size_t len = initial_value_bufinfo->len;
        if (len > (size_t)max_length) {
            len = max_length;
        }
        memcpy(self->current_value, initial_value_bufinfo->buf, len);
        self->current_value_len = len;
    }

    // Convert UUID to Zephyr format
    bleio_uuid_to_zephyr(uuid, &self->zephyr_uuid);

    if (service->is_remote) {
        // Remote characteristic: just add to the service's list
        mp_obj_list_append(MP_OBJ_FROM_PTR(service->characteristic_list),
            MP_OBJ_FROM_PTR(self));
    } else {
        common_hal_bleio_service_add_characteristic(service, self,
            initial_value_bufinfo, user_description);
    }
}

bool common_hal_bleio_characteristic_deinited(bleio_characteristic_obj_t *self) {
    return self->service == NULL;
}

void common_hal_bleio_characteristic_deinit(bleio_characteristic_obj_t *self) {
    // Nothing to do - service handles unregistration
}

void common_hal_bleio_characteristic_set_cccd(bleio_characteristic_obj_t *self, bool notify, bool indicate) {
    // Client-side only operation
    mp_raise_NotImplementedError(NULL);
}

void common_hal_bleio_characteristic_set_value(bleio_characteristic_obj_t *self, mp_buffer_info_t *bufinfo) {
    if (self->service != NULL && self->service->is_remote) {
        // Remote characteristic: write via GATT client
        bleio_connection_obj_t *connection = MP_OBJ_TO_PTR(self->service->connection);
        if (connection == NULL || connection->connection == NULL ||
            connection->connection->conn == NULL) {
            mp_raise_bleio_BluetoothError(MP_ERROR_TEXT("Not connected"));
        }

        if (self->props & CHAR_PROP_WRITE_NO_RESPONSE) {
            int err = bt_gatt_write_without_response(
                connection->connection->conn,
                self->handle,
                bufinfo->buf, bufinfo->len, false);
            if (err != 0) {
                raise_zephyr_error(err);
            }
        } else {
            gattc_write_ctx_t ctx = {
                .done = false,
                .err = 0,
            };
            active_write_ctx = &ctx;

            memset(&write_params, 0, sizeof(write_params));
            write_params.func = on_gattc_write;
            write_params.handle = self->handle;
            write_params.offset = 0;
            write_params.data = bufinfo->buf;
            write_params.length = bufinfo->len;

            int err = bt_gatt_write(connection->connection->conn, &write_params);
            if (err != 0) {
                active_write_ctx = NULL;
                raise_zephyr_error(err);
            }

            while (!ctx.done) {
                RUN_BACKGROUND_TASKS;
            }
            active_write_ctx = NULL;

            if (ctx.err != 0) {
                raise_zephyr_error(ctx.err);
            }
        }
        return;
    }

    // Local characteristic
    size_t len = bufinfo->len;
    if (len > self->max_length) {
        len = self->max_length;
    }
    memcpy(self->current_value, bufinfo->buf, len);
    self->current_value_len = len;

    // If NOTIFY and service is registered, send notification
    if ((self->props & CHAR_PROP_NOTIFY) && self->service != NULL &&
        self->service->registered) {
        bt_gatt_notify(NULL, &self->service->attrs[self->value_attr_index],
            self->current_value, self->current_value_len);
    }
}

void common_hal_bleio_characteristic_add_descriptor(bleio_characteristic_obj_t *self,
    bleio_descriptor_obj_t *descriptor) {
    mp_obj_list_append(MP_OBJ_FROM_PTR(self->descriptor_list),
        MP_OBJ_FROM_PTR(descriptor));
    // Descriptors added after characteristic construction would need
    // service re-registration; for now the common case is handled by
    // Service.add_characteristic which adds descriptors at registration time.
}

void bleio_characteristic_set_observer(bleio_characteristic_obj_t *self, mp_obj_t observer) {
    self->observer = observer;
}

void bleio_characteristic_clear_observer(bleio_characteristic_obj_t *self) {
    self->observer = mp_const_none;
}
