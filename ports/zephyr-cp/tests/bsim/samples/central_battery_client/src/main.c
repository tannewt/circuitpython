/*
 * SPDX-FileCopyrightText: 2026 Scott Shawcroft for Adafruit Industries
 * SPDX-License-Identifier: MIT
 *
 * Zephyr BLE central that connects to a device named "CPSVC",
 * discovers Battery Service (0x180F), reads Battery Level (0x2A19),
 * prints the value, and disconnects.
 */

#include <stddef.h>
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

static struct bt_conn *default_conn;
static struct bt_gatt_discover_params discover_params;
static struct bt_gatt_read_params read_params;

/* Battery Service UUID: 0x180F */
static struct bt_uuid_16 bas_uuid = BT_UUID_INIT_16(0x180F);
/* Battery Level Characteristic UUID: 0x2A19 */
static struct bt_uuid_16 bat_level_uuid = BT_UUID_INIT_16(0x2A19);

static uint16_t bat_level_handle;

static void start_scan(void);

/* Check if advertisement data contains the name "CPSVC" */
static bool ad_has_name(struct net_buf_simple *ad, const char *name) {
    size_t name_len = strlen(name);

    while (ad->len > 1) {
        uint8_t field_len = net_buf_simple_pull_u8(ad);
        if (field_len == 0 || field_len > ad->len) {
            break;
        }
        uint8_t type = net_buf_simple_pull_u8(ad);
        field_len--;

        if ((type == BT_DATA_NAME_COMPLETE || type == BT_DATA_NAME_SHORTENED) &&
            field_len == name_len &&
            memcmp(ad->data, name, name_len) == 0) {
            return true;
        }
        net_buf_simple_pull(ad, field_len);
    }
    return false;
}

static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
    struct net_buf_simple *ad) {
    if (default_conn) {
        return;
    }

    /* Only interested in connectable devices */
    if (type != BT_GAP_ADV_TYPE_ADV_IND &&
        type != BT_GAP_ADV_TYPE_ADV_DIRECT_IND) {
        return;
    }

    /* Clone ad data so we can parse it without affecting the original */
    struct net_buf_simple ad_copy;
    uint8_t ad_buf[64];
    size_t copy_len = ad->len < sizeof(ad_buf) ? ad->len : sizeof(ad_buf);
    memcpy(ad_buf, ad->data, copy_len);
    net_buf_simple_init_with_data(&ad_copy, ad_buf, copy_len);

    if (!ad_has_name(&ad_copy, "CPSVC")) {
        return;
    }

    char addr_str[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
    printk("Found CPSVC device: %s (RSSI %d)\n", addr_str, rssi);

    if (bt_le_scan_stop()) {
        return;
    }

    int err = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN,
        BT_LE_CONN_PARAM_DEFAULT, &default_conn);
    if (err) {
        printk("Create conn failed (%d)\n", err);
        start_scan();
    }
}

static void start_scan(void) {
    int err = bt_le_scan_start(BT_LE_SCAN_PASSIVE, device_found);
    if (err) {
        printk("Scanning failed to start (err %d)\n", err);
        return;
    }
    printk("Scanning started\n");
}

static uint8_t read_battery_level_cb(struct bt_conn *conn, uint8_t err,
    struct bt_gatt_read_params *params,
    const void *data, uint16_t length) {
    if (err) {
        printk("Read failed (err %d)\n", err);
    } else if (data && length >= 1) {
        uint8_t level = ((const uint8_t *)data)[0];
        printk("Battery Level: %d\n", level);
    } else {
        printk("Read returned no data\n");
    }

    /* Disconnect after reading */
    bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);

    return BT_GATT_ITER_STOP;
}

static uint8_t discover_char_cb(struct bt_conn *conn,
    const struct bt_gatt_attr *attr,
    struct bt_gatt_discover_params *params) {
    if (!attr) {
        printk("Characteristic discovery complete\n");
        return BT_GATT_ITER_STOP;
    }

    struct bt_gatt_chrc *chrc = (struct bt_gatt_chrc *)attr->user_data;

    if (bt_uuid_cmp(chrc->uuid, &bat_level_uuid.uuid) == 0) {
        bat_level_handle = chrc->value_handle;
        printk("Found Battery Level characteristic, handle: %u\n", bat_level_handle);

        /* Read the battery level */
        read_params.func = read_battery_level_cb;
        read_params.handle_count = 1;
        read_params.single.handle = bat_level_handle;
        read_params.single.offset = 0;

        int err = bt_gatt_read(conn, &read_params);
        if (err) {
            printk("Read request failed (err %d)\n", err);
        }
        return BT_GATT_ITER_STOP;
    }

    return BT_GATT_ITER_CONTINUE;
}

static uint8_t discover_service_cb(struct bt_conn *conn,
    const struct bt_gatt_attr *attr,
    struct bt_gatt_discover_params *params) {
    if (!attr) {
        printk("Service discovery complete, BAS not found\n");
        bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        return BT_GATT_ITER_STOP;
    }

    printk("Found Battery Service, handle: %u\n", attr->handle);

    /* Now discover characteristics within this service */
    discover_params.uuid = &bat_level_uuid.uuid;
    discover_params.start_handle = attr->handle + 1;
    discover_params.end_handle = 0xFFFF;
    discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;
    discover_params.func = discover_char_cb;

    int err = bt_gatt_discover(conn, &discover_params);
    if (err) {
        printk("Characteristic discovery failed (err %d)\n", err);
    }

    return BT_GATT_ITER_STOP;
}

static void connected(struct bt_conn *conn, uint8_t err) {
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    if (err) {
        printk("Failed to connect to %s (%u)\n", addr, err);
        bt_conn_unref(default_conn);
        default_conn = NULL;
        start_scan();
        return;
    }

    if (conn != default_conn) {
        return;
    }

    printk("Connected: %s\n", addr);

    /* Discover Battery Service */
    discover_params.uuid = &bas_uuid.uuid;
    discover_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
    discover_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
    discover_params.type = BT_GATT_DISCOVER_PRIMARY;
    discover_params.func = discover_service_cb;

    err = bt_gatt_discover(conn, &discover_params);
    if (err) {
        printk("Service discovery failed (err %d)\n", err);
    }
}

static void disconnected(struct bt_conn *conn, uint8_t reason) {
    char addr[BT_ADDR_LE_STR_LEN];

    if (conn != default_conn) {
        return;
    }

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    printk("Disconnected: %s (reason 0x%02x)\n", addr, reason);

    bt_conn_unref(default_conn);
    default_conn = NULL;
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
};

int main(void) {
    int err = bt_enable(NULL);
    if (err) {
        printk("Bluetooth init failed (err %d)\n", err);
        return 0;
    }

    printk("Bluetooth initialized\n");
    start_scan();
    return 0;
}
