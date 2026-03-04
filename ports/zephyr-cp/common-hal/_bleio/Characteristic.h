// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2019 Dan Halbert for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2026 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

#include <zephyr/bluetooth/gatt.h>

#include "py/obj.h"
#include "py/objlist.h"
#include "shared-bindings/_bleio/Attribute.h"
#include "shared-module/_bleio/Characteristic.h"
#include "common-hal/_bleio/Descriptor.h"
#include "common-hal/_bleio/Service.h"
#include "common-hal/_bleio/UUID.h"

typedef struct _bleio_characteristic_obj {
    mp_obj_base_t base;
    bleio_service_obj_t *service;
    bleio_uuid_obj_t *uuid;
    mp_obj_t observer;
    uint8_t *current_value;
    uint16_t current_value_len;
    uint16_t current_value_alloc;
    uint16_t max_length;
    uint16_t def_handle;
    uint16_t handle;
    bleio_characteristic_properties_t props;
    bleio_attribute_security_mode_t read_perm;
    bleio_attribute_security_mode_t write_perm;
    mp_obj_list_t *descriptor_list;
    uint16_t user_desc_handle;
    uint16_t cccd_handle;
    uint16_t sccd_handle;
    bool fixed_length;
    // Zephyr GATT server fields:
    struct bt_gatt_chrc zephyr_chrc;
    struct bt_uuid_128 zephyr_uuid;
    struct bt_gatt_ccc_managed_user_data zephyr_ccc;
    size_t value_attr_index;  // index in service attrs for notify
} bleio_characteristic_obj_t;

void bleio_characteristic_set_observer(bleio_characteristic_obj_t *self, mp_obj_t observer);
void bleio_characteristic_clear_observer(bleio_characteristic_obj_t *self);

// Callbacks used by Service.c when building GATT attr table
ssize_t bleio_char_read_cb(struct bt_conn *conn,
    const struct bt_gatt_attr *attr, void *buf, uint16_t len, uint16_t offset);
ssize_t bleio_char_write_cb(struct bt_conn *conn,
    const struct bt_gatt_attr *attr, const void *buf, uint16_t len,
    uint16_t offset, uint8_t flags);
void bleio_ccc_changed_cb(const struct bt_gatt_attr *attr, uint16_t value);

// Permission mapping helper
uint16_t bleio_security_to_zephyr_perm(
    bleio_attribute_security_mode_t read_perm,
    bleio_attribute_security_mode_t write_perm,
    bleio_characteristic_properties_t props);
