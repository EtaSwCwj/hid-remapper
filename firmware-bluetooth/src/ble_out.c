#include "ble_out.h"

#include <string.h>

#include <bluetooth/services/hids.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

LOG_MODULE_REGISTER(ble_out, LOG_LEVEL_INF);

// These must match firmware/src/our_descriptor.cc (keyboard/mouse descriptor).
#define REPORT_ID_KEYBOARD 2
#define REPORT_ID_CONSUMER 3
#define REPORT_ID_LEDS 98

#define KEYBOARD_REPORT_SIZE 16
#define CONSUMER_REPORT_SIZE 3
#define LEDS_REPORT_SIZE 1

#define KEYBOARD_REPORT_IDX 0
#define CONSUMER_REPORT_IDX 1

BT_HIDS_DEF(hids_obj, KEYBOARD_REPORT_SIZE, CONSUMER_REPORT_SIZE, LEDS_REPORT_SIZE);

// Keyboard and consumer collections copied from our_report_descriptor_kb_mouse.
// The mouse part is left out: the BLE report map is limited to 255 bytes and
// the host should see a plain keyboard.
static const uint8_t report_map[] = {
    0x05, 0x01,                // Usage Page (Generic Desktop Ctrls)
    0x09, 0x06,                // Usage (Keyboard)
    0xA1, 0x01,                // Collection (Application)
    0x85, REPORT_ID_KEYBOARD,  //   Report ID (REPORT_ID_KEYBOARD)
    0x05, 0x07,                //   Usage Page (Kbrd/Keypad)
    0x19, 0xE0,                //   Usage Minimum (0xE0)
    0x29, 0xE7,                //   Usage Maximum (0xE7)
    0x15, 0x00,                //   Logical Minimum (0)
    0x25, 0x01,                //   Logical Maximum (1)
    0x75, 0x01,                //   Report Size (1)
    0x95, 0x08,                //   Report Count (8)
    0x81, 0x02,                //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x19, 0x04,                //   Usage Minimum (0x04)
    0x29, 0x73,                //   Usage Maximum (0x73)
    0x95, 0x70,                //   Report Count (112)
    0x81, 0x02,                //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x19, 0x87,                //   Usage Minimum (0x87)
    0x29, 0x8B,                //   Usage Maximum (0x8B)
    0x95, 0x05,                //   Report Count (5)
    0x81, 0x02,                //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x09, 0x90,                //   Usage (0x90)
    0x09, 0x91,                //   Usage (0x91)
    0x95, 0x02,                //   Report Count (2)
    0x81, 0x02,                //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x95, 0x01,                //   Report Count (1)
    0x81, 0x03,                //   Input (Const,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x85, REPORT_ID_LEDS,      //   Report ID (REPORT_ID_LEDS)
    0x05, 0x08,                //   Usage Page (LEDs)
    0x95, 0x05,                //   Report Count (5)
    0x19, 0x01,                //   Usage Minimum (Num Lock)
    0x29, 0x05,                //   Usage Maximum (Kana)
    0x91, 0x02,                //   Output (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x95, 0x01,                //   Report Count (1)
    0x75, 0x03,                //   Report Size (3)
    0x91, 0x03,                //   Output (Const,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0xC0,                      // End Collection

    0x05, 0x0C,                // Usage Page (Consumer)
    0x09, 0x01,                // Usage (Consumer Control)
    0xA1, 0x01,                // Collection (Application)
    0x85, REPORT_ID_CONSUMER,  //   Report ID (REPORT_ID_CONSUMER)
    0x15, 0x00,                //   Logical Minimum (0)
    0x25, 0x01,                //   Logical Maximum (1)
    0x09, 0xB5,                //   Usage (Scan Next Track)
    0x09, 0xB6,                //   Usage (Scan Previous Track)
    0x09, 0xB7,                //   Usage (Stop)
    0x09, 0xCD,                //   Usage (Play/Pause)
    0x09, 0xE2,                //   Usage (Mute)
    0x09, 0xE9,                //   Usage (Volume Increment)
    0x09, 0xEA,                //   Usage (Volume Decrement)
    0x09, 0x30,                //   Usage (Power)
    0x09, 0x40,                //   Usage (Menu)
    0x09, 0x6F,                //   Usage (Display Brightness Increment)
    0x09, 0x70,                //   Usage (Display Brightness Decrement)
    0x09, 0x71,                //   Usage (Display Brightness)
    0x09, 0xB8,                //   Usage (Eject)
    0x09, 0xCF,                //   Usage (Voice Command)
    0x0A, 0x21, 0x02,          //   Usage (AC Search)
    0x0A, 0x23, 0x02,          //   Usage (AC Home)
    0x0A, 0x9D, 0x02,          //   Usage (Keyboard Layout Select / Globe)
    0x75, 0x01,                //   Report Size (1)
    0x95, 0x11,                //   Report Count (17)
    0x81, 0x02,                //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x05, 0x0B,                //   Usage Page (Telephony)
    0x09, 0x2F,                //   Usage (Phone Mute)
    0x95, 0x01,                //   Report Count (1)
    0x81, 0x02,                //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x95, 0x06,                //   Report Count (6)
    0x81, 0x03,                //   Input (Const,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0xC0,                      // End Collection
};

BUILD_ASSERT(sizeof(report_map) <= 255, "BLE report map too long");

static struct bt_conn* host_conn;
static atomic_t in_flight = ATOMIC_INIT(0);

static uint8_t pending_data[2][KEYBOARD_REPORT_SIZE];
static bool pending[2];
static bt_addr_le_t host_addr;
static bool host_addr_valid;

static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE, 0xC1, 0x03),  // keyboard
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA_BYTES(BT_DATA_UUID16_ALL, BT_UUID_16_ENCODE(BT_UUID_HIDS_VAL)),
};

static const struct bt_data sd[] = {
    BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static void adv_work_fn(struct k_work* work);
static K_WORK_DELAYABLE_DEFINE(adv_work, adv_work_fn);

// Advertise quickly for a while so the host reconnects at once, then slowly
// (about once a second) so that waiting for an absent host costs very little.
#define ADV_FAST_PERIOD K_MINUTES(3)
#define ADV_SLOW BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONNECTABLE, BT_GAP_ADV_SLOW_INT_MIN, BT_GAP_ADV_SLOW_INT_MAX, NULL)

static void adv_slow_work_fn(struct k_work* work);
static K_WORK_DELAYABLE_DEFINE(adv_slow_work, adv_slow_work_fn);

static void adv_work_fn(struct k_work* work) {
    if (host_conn != NULL) {
        return;
    }

    bt_le_adv_stop();
    int err = bt_le_adv_start(BT_LE_ADV_CONN, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
    if (err == 0) {
        LOG_INF("advertising as keyboard");
        k_work_reschedule(&adv_slow_work, ADV_FAST_PERIOD);
    } else if (err != -EALREADY) {
        LOG_ERR("bt_le_adv_start failed: %d", err);
        k_work_reschedule(&adv_work, K_SECONDS(2));
    }
}

static void adv_slow_work_fn(struct k_work* work) {
    if (host_conn != NULL) {
        return;
    }

    bt_le_adv_stop();
    int err = bt_le_adv_start(ADV_SLOW, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
    if (err == 0) {
        LOG_INF("host not around, advertising slowly");
    } else {
        LOG_ERR("slow bt_le_adv_start failed: %d", err);
        k_work_reschedule(&adv_work, K_SECONDS(2));
    }
}

static void leds_handler(struct bt_hids_rep* rep, struct bt_conn* conn, bool write) {
}

int ble_out_init(void) {
    static struct bt_hids_init_param hids_init_param;

    memset(&hids_init_param, 0, sizeof(hids_init_param));

    hids_init_param.rep_map.data = report_map;
    hids_init_param.rep_map.size = sizeof(report_map);

    hids_init_param.info.bcd_hid = 0x0111;
    hids_init_param.info.b_country_code = 0x00;
    hids_init_param.info.flags = (BT_HIDS_REMOTE_WAKE | BT_HIDS_NORMALLY_CONNECTABLE);

    hids_init_param.inp_rep_group_init.reports[KEYBOARD_REPORT_IDX].id = REPORT_ID_KEYBOARD;
    hids_init_param.inp_rep_group_init.reports[KEYBOARD_REPORT_IDX].size = KEYBOARD_REPORT_SIZE;
    hids_init_param.inp_rep_group_init.reports[CONSUMER_REPORT_IDX].id = REPORT_ID_CONSUMER;
    hids_init_param.inp_rep_group_init.reports[CONSUMER_REPORT_IDX].size = CONSUMER_REPORT_SIZE;
    hids_init_param.inp_rep_group_init.cnt = 2;

    hids_init_param.outp_rep_group_init.reports[0].id = REPORT_ID_LEDS;
    hids_init_param.outp_rep_group_init.reports[0].size = LEDS_REPORT_SIZE;
    hids_init_param.outp_rep_group_init.reports[0].handler = leds_handler;
    hids_init_param.outp_rep_group_init.cnt = 1;

    hids_init_param.is_kb = false;
    hids_init_param.is_mouse = false;

    int err = bt_hids_init(&hids_obj, &hids_init_param);
    if (err) {
        LOG_ERR("bt_hids_init failed: %d", err);
    }
    return err;
}

void ble_out_adv_start(void) {
    k_work_reschedule(&adv_work, K_MSEC(200));
}

bool ble_out_is_host(struct bt_conn* conn) {
    if (conn == host_conn) {
        return true;
    }

    struct bt_conn_info info;
    if (bt_conn_get_info(conn, &info)) {
        return false;
    }
    return info.role == BT_CONN_ROLE_PERIPHERAL;
}

bool ble_out_host_connected(void) {
    return host_conn != NULL;
}

bool ble_out_host_known(void) {
    return host_addr_valid;
}

void ble_out_connected(struct bt_conn* conn, uint8_t conn_err) {
    if (conn_err) {
        LOG_ERR("host connection failed: %u", conn_err);
        ble_out_adv_start();
        return;
    }

    LOG_INF("host connected");

    if (host_conn != NULL) {
        bt_conn_unref(host_conn);
    }
    host_conn = bt_conn_ref(conn);
    atomic_set(&in_flight, 0);
    pending[0] = false;
    pending[1] = false;

    int err = bt_hids_connected(&hids_obj, conn);
    if (err) {
        LOG_ERR("bt_hids_connected failed: %d", err);
    }

    err = bt_conn_set_security(conn, BT_SECURITY_L2);
    if (err) {
        LOG_WRN("bt_conn_set_security failed: %d", err);
    }
}

void ble_out_disconnected(struct bt_conn* conn) {
    LOG_INF("host disconnected");

    int err = bt_hids_disconnected(&hids_obj, conn);
    if (err) {
        LOG_ERR("bt_hids_disconnected failed: %d", err);
    }

    if (host_conn != NULL) {
        bt_conn_unref(host_conn);
        host_conn = NULL;
    }

    ble_out_adv_start();
}

void ble_out_set_host_addr(const bt_addr_le_t* addr) {
    bt_addr_le_copy(&host_addr, addr);
    host_addr_valid = true;
}

void ble_out_remember_host(struct bt_conn* conn) {
    const bt_addr_le_t* addr = bt_conn_get_dst(conn);

    if (host_addr_valid && (bt_addr_le_cmp(&host_addr, addr) == 0)) {
        return;
    }

    ble_out_set_host_addr(addr);

    int err = settings_save_one("remapper/host", &host_addr, sizeof(host_addr));
    if (err) {
        LOG_ERR("saving host address failed: %d", err);
    }
}

bool ble_out_is_host_addr(const bt_addr_le_t* addr) {
    return host_addr_valid && (bt_addr_le_cmp(&host_addr, addr) == 0);
}

void ble_out_forget_host(void) {
    host_addr_valid = false;
    settings_delete("remapper/host");
}

// bt_hids_inp_rep_send() blocks when the stack runs out of TX buffers, which
// would stall the main loop. So we only keep a few notifications in flight
// and remember the latest report of each kind to send once there's room.
#define MAX_IN_FLIGHT 3

static const uint8_t report_sizes_by_idx[2] = { KEYBOARD_REPORT_SIZE, CONSUMER_REPORT_SIZE };

static void flush_work_fn(struct k_work* work);
static K_WORK_DELAYABLE_DEFINE(flush_work, flush_work_fn);
static K_MUTEX_DEFINE(send_mutex);

static void notify_complete_cb(struct bt_conn* conn, void* user_data) {
    if (atomic_get(&in_flight) > 0) {
        atomic_dec(&in_flight);
    }
    if (pending[0] || pending[1]) {
        k_work_reschedule(&flush_work, K_NO_WAIT);
    }
}

static bool try_send(uint8_t idx, const uint8_t* data) {
    if (atomic_get(&in_flight) >= MAX_IN_FLIGHT) {
        return false;
    }

    atomic_inc(&in_flight);
    int err = bt_hids_inp_rep_send(&hids_obj, host_conn, idx, data, report_sizes_by_idx[idx], notify_complete_cb);
    if (err) {
        atomic_dec(&in_flight);
        // not subscribed yet or similar; nothing to retry
    }
    return true;
}

void ble_out_poll(void) {
    if (host_conn == NULL) {
        return;
    }

    k_mutex_lock(&send_mutex, K_FOREVER);
    for (uint8_t idx = 0; idx < 2; idx++) {
        if (pending[idx] && try_send(idx, pending_data[idx])) {
            pending[idx] = false;
        }
    }
    k_mutex_unlock(&send_mutex);
}

static void flush_work_fn(struct k_work* work) {
    ble_out_poll();
    if ((host_conn != NULL) && (pending[0] || pending[1])) {
        k_work_reschedule(&flush_work, K_MSEC(5));
    }
}

bool ble_out_send(uint8_t report_id, const uint8_t* data, uint8_t len) {
    uint8_t idx;

    if (host_conn == NULL) {
        return false;
    }

    switch (report_id) {
        case REPORT_ID_KEYBOARD:
            idx = KEYBOARD_REPORT_IDX;
            break;
        case REPORT_ID_CONSUMER:
            idx = CONSUMER_REPORT_IDX;
            break;
        default:
            return false;
    }

    if (len != report_sizes_by_idx[idx]) {
        return false;
    }

    k_mutex_lock(&send_mutex, K_FOREVER);
    if (pending[idx] || !try_send(idx, data)) {
        // keep only the latest state; it's sent once there's room
        memcpy(pending_data[idx], data, len);
        pending[idx] = true;
        k_work_reschedule(&flush_work, K_MSEC(5));
    }
    k_mutex_unlock(&send_mutex);

    return true;
}
