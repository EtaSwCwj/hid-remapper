// Keyboard remapping relay for small nRF52 boards.
//
// Connects to a Bluetooth LE keyboard as a central, applies the fixed key
// layout from keymap.h and presents itself to a host (iPad, ...) as a
// Bluetooth LE keyboard. Everything is event driven so the CPU sleeps
// between radio events.

#include <errno.h>
#include <string.h>

#include <bluetooth/gatt_dm.h>
#include <bluetooth/scan.h>
#include <bluetooth/services/hogp.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/reboot.h>

#ifdef CONFIG_USB_DEVICE_STACK
#include <zephyr/usb/usb_device.h>
#endif

#include "battery.h"
#include "ble_out.h"
#include "hid_parse.h"
#include "remap.h"

LOG_MODULE_REGISTER(tag, LOG_LEVEL_INF);

#define CHK(X) ({ int _err = X; if (_err != 0) { LOG_ERR("%s returned %d (line %d)", #X, _err, __LINE__); } _err == 0; })

#define SCAN_DELAY_MS 1000
#define CLEAR_BONDS_BUTTON_PRESS_MS 3000
#define MAX_DESCRIPTOR_SIZE 512

// https://github.com/adafruit/Adafruit_nRF52_Bootloader/blob/master/src/main.c#L116
#define DFU_MAGIC_UF2_RESET 0x57

static struct bt_hogp hogp;

static bool scanning;
// true: only reconnect to the keyboard we know; false: accept a new keyboard
static bool peers_only = true;

// 15 ms connection interval: a compromise between latency and battery life
static const struct bt_le_conn_param conn_param = BT_LE_CONN_PARAM_INIT(12, 12, 6, 300);

static uint8_t descriptor[MAX_DESCRIPTOR_SIZE];

#define LED0_NODE DT_ALIAS(led0)
#define SW0_NODE DT_ALIAS(sw0)

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(SW0_NODE, gpios);
static struct gpio_callback button_cb_data;
static int64_t button_pressed_at;

static void scan_start_work_fn(struct k_work* work);
static K_WORK_DELAYABLE_DEFINE(scan_start_work, scan_start_work_fn);
static void scan_slow_work_fn(struct k_work* work);
static K_WORK_DELAYABLE_DEFINE(scan_slow_work, scan_slow_work_fn);
static bool scan_slow;
static const struct bt_le_scan_param scan_param_fast;

#define SCAN_FAST_PERIOD K_MINUTES(3)

// --- LED: lit while a new keyboard can be paired, short blink on events ---

static void led_off_work_fn(struct k_work* work) {
    gpio_pin_set_dt(&led, peers_only ? 0 : 1);
}
static K_WORK_DELAYABLE_DEFINE(led_off_work, led_off_work_fn);

static void led_blink(void) {
    gpio_pin_set_dt(&led, 1);
    k_work_reschedule(&led_off_work, K_MSEC(200));
}

static void led_update(void) {
    k_work_reschedule(&led_off_work, K_NO_WAIT);
}

// --- scanning for the keyboard ---

static void scan_stop(void) {
    if (scanning && CHK(bt_scan_stop())) {
        scanning = false;
    }
}

struct bond_count {
    int inputs;
};

static void process_bond(const struct bt_bond_info* info, void* user_data) {
    struct bond_count* count = user_data;

    if (ble_out_is_host_addr(&info->addr)) {
        // this bond is the host we're a keyboard for, not an input device
        return;
    }
    count->inputs++;
    CHK(bt_scan_filter_add(BT_SCAN_FILTER_TYPE_ADDR, &info->addr));
}

static struct bt_conn* keyboard_conn;

static void scan_start_work_fn(struct k_work* work) {
    scan_stop();

    if (keyboard_conn != NULL) {
        // one keyboard at a time
        led_update();
        return;
    }

    bt_scan_filter_remove_all();
    if (!CHK(bt_scan_filter_add(BT_SCAN_FILTER_TYPE_UUID, BT_UUID_HIDS))) {
        return;
    }

    struct bond_count count = { 0 };
    bt_foreach_bond(BT_ID_DEFAULT, process_bond, &count);

    uint8_t filter_mode = BT_SCAN_UUID_FILTER;
    if (peers_only && (count.inputs > 0)) {
        filter_mode |= BT_SCAN_ADDR_FILTER;
        LOG_INF("scanning for the known keyboard");
    } else {
        peers_only = false;
        LOG_INF("scanning for a new keyboard");
    }

    if (!CHK(bt_scan_filter_enable(filter_mode, true))) {
        return;
    }

    CHK(bt_scan_params_set((struct bt_le_scan_param*) &scan_param_fast));
    scan_slow = false;

    if (CHK(bt_scan_start(BT_SCAN_TYPE_SCAN_PASSIVE))) {
        scanning = true;
        k_work_reschedule(&scan_slow_work, SCAN_FAST_PERIOD);
    }

    led_update();
}

static void scan_filter_match(struct bt_scan_device_info* device_info, struct bt_scan_filter_match* filter_match, bool connectable) {
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(device_info->recv_info->addr, addr, sizeof(addr));
    LOG_INF("found %s", addr);
}

static void scan_connecting_error(struct bt_scan_device_info* device_info) {
    LOG_WRN("connecting failed");
    k_work_reschedule(&scan_start_work, K_MSEC(SCAN_DELAY_MS));
}

static void scan_connecting(struct bt_scan_device_info* device_info, struct bt_conn* conn) {
    scanning = false;
}

static void scan_filter_no_match(struct bt_scan_device_info* device_info, bool connectable) {
    // A bonded keyboard may use directed advertising when reconnecting.
    if (device_info->recv_info->adv_type == BT_GAP_ADV_TYPE_ADV_DIRECT_IND) {
        struct bt_conn* conn;

        scan_stop();
        if (CHK(bt_conn_le_create(device_info->recv_info->addr, BT_CONN_LE_CREATE_CONN, device_info->conn_param, &conn))) {
            bt_conn_unref(conn);
        }
    }
}

BT_SCAN_CB_INIT(scan_cb, scan_filter_match, scan_filter_no_match, scan_connecting_error, scan_connecting);

// Fast: listen 30 ms out of every 200 ms, the keyboard is found within a
// second or so. Slow: 30 ms out of every 1.28 s, used once the keyboard has
// been away for a while (it's switched off or asleep), to save the battery.
static const struct bt_le_scan_param scan_param_fast = {
    .type = BT_LE_SCAN_TYPE_PASSIVE,
    .options = BT_LE_SCAN_OPT_NONE,
    .interval = 0x0140,
    .window = 0x0030,
};

static const struct bt_le_scan_param scan_param_slow = {
    .type = BT_LE_SCAN_TYPE_PASSIVE,
    .options = BT_LE_SCAN_OPT_NONE,
    .interval = 0x0800,
    .window = 0x0030,
};

static void scan_slow_work_fn(struct k_work* work) {
    if (!scanning || !peers_only) {
        return;
    }
    if (CHK(bt_scan_stop())) {
        scanning = false;
        CHK(bt_scan_params_set((struct bt_le_scan_param*) &scan_param_slow));
        if (CHK(bt_scan_start(BT_SCAN_TYPE_SCAN_PASSIVE))) {
            scanning = true;
            scan_slow = true;
            LOG_INF("keyboard not around, scanning slowly");
        }
    }
}

static void scan_init(void) {
    struct bt_scan_init_param scan_init_param = {
        .scan_param = &scan_param_fast,
        .connect_if_match = 1,
        .conn_param = &conn_param,
    };

    bt_scan_init(&scan_init_param);
    bt_scan_cb_register(&scan_cb);
}

// --- keyboard: service discovery and reports ---

static uint8_t hogp_notify_cb(struct bt_hogp* hogp_, struct bt_hogp_rep_info* rep, uint8_t err, const uint8_t* data) {
    if (!data) {
        return BT_GATT_ITER_STOP;
    }

    remap_handle_report(bt_hogp_rep_id(rep), data, bt_hogp_rep_size(rep));

    return BT_GATT_ITER_CONTINUE;
}

static void hogp_map_read_cb(struct bt_hogp* hogp_, uint8_t err, const uint8_t* data, size_t size, size_t offset) {
    if (err) {
        LOG_ERR("report map read failed: %u", err);
        return;
    }

    if (data == NULL) {
        int fields = hid_parse_descriptor(descriptor, offset);
        LOG_INF("report map: %u bytes, %d input fields", (unsigned) offset, fields);
        led_blink();
        return;
    }

    if (offset + size > sizeof(descriptor)) {
        LOG_ERR("report map too long");
        return;
    }

    memcpy(descriptor + offset, data, size);

    bt_hogp_map_read(hogp_, hogp_map_read_cb, offset + size, K_NO_WAIT);
}

static void hogp_ready_work_fn(struct k_work* work) {
    struct bt_hogp_rep_info* rep = NULL;

    LOG_INF("keyboard ready");

    while (NULL != (rep = bt_hogp_rep_next(&hogp, rep))) {
        if (bt_hogp_rep_type(rep) == BT_HIDS_REPORT_TYPE_INPUT) {
            CHK(bt_hogp_rep_subscribe(&hogp, rep, hogp_notify_cb));
        }
    }

    bt_hogp_map_read(&hogp, hogp_map_read_cb, 0, K_NO_WAIT);
}
static K_WORK_DEFINE(hogp_ready_work, hogp_ready_work_fn);

static void hogp_ready_cb(struct bt_hogp* hogp_) {
    k_work_submit(&hogp_ready_work);
}

static void hogp_prep_error_cb(struct bt_hogp* hogp_, int err) {
    LOG_ERR("hogp error: %d", err);
}

static const struct bt_hogp_init_params hogp_init_params = {
    .ready_cb = hogp_ready_cb,
    .prep_error_cb = hogp_prep_error_cb,
};

static void discovery_completed_cb(struct bt_gatt_dm* dm, void* context) {
    CHK(bt_hogp_handles_assign(dm, &hogp));
    CHK(bt_gatt_dm_data_release(dm));
}

static void discovery_service_not_found_cb(struct bt_conn* conn, void* context) {
    LOG_WRN("HID service not found");
}

static void discovery_error_found_cb(struct bt_conn* conn, int err, void* context) {
    LOG_ERR("discovery failed: %d", err);
}

static const struct bt_gatt_dm_cb discovery_cb = {
    .completed = discovery_completed_cb,
    .service_not_found = discovery_service_not_found_cb,
    .error_found = discovery_error_found_cb,
};

// --- connections ---

static void connected(struct bt_conn* conn, uint8_t conn_err) {
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    if (ble_out_is_host(conn)) {
        ble_out_connected(conn, conn_err);
        return;
    }

    scanning = false;

    if (conn_err) {
        LOG_ERR("connecting to %s failed: %u", addr, conn_err);
        k_work_reschedule(&scan_start_work, K_MSEC(SCAN_DELAY_MS));
        return;
    }

    LOG_INF("keyboard connected: %s", addr);

    if (keyboard_conn != NULL) {
        bt_conn_unref(keyboard_conn);
    }
    keyboard_conn = bt_conn_ref(conn);

    CHK(bt_conn_set_security(conn, BT_SECURITY_L2));
}

static void disconnected(struct bt_conn* conn, uint8_t reason) {
    if (ble_out_is_host(conn)) {
        ble_out_disconnected(conn);
        return;
    }

    LOG_INF("keyboard disconnected (reason %u)", reason);

    if (bt_hogp_assign_check(&hogp)) {
        bt_hogp_release(&hogp);
    }

    if (keyboard_conn != NULL) {
        bt_conn_unref(keyboard_conn);
        keyboard_conn = NULL;
    }

    // nothing is held down any more
    remap_reset();

    k_work_reschedule(&scan_start_work, K_MSEC(SCAN_DELAY_MS));
}

static void security_changed(struct bt_conn* conn, bt_security_t level, enum bt_security_err err) {
    if (ble_out_is_host(conn)) {
        LOG_INF("host security level %u, err %d", level, err);
        return;
    }

    if (err) {
        LOG_ERR("keyboard security failed: level %u, err %d", level, err);
        // start over; the keyboard may have forgotten us
        bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        return;
    }

    LOG_INF("keyboard security level %u", level);
    peers_only = true;
    led_update();

    if (!CHK(bt_gatt_dm_start(conn, BT_UUID_HIDS, &discovery_cb, NULL))) {
        bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
}

static bool le_param_req(struct bt_conn* conn, struct bt_le_conn_param* param) {
    if (!ble_out_is_host(conn)) {
        // the keyboard asks for a range; take the slow end to save power
        param->interval_min = param->interval_max;
    }
    return true;
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
    .le_param_req = le_param_req,
    .security_changed = security_changed,
};

static void auth_cancel(struct bt_conn* conn) {
    LOG_WRN("pairing cancelled");
}

static void pairing_complete(struct bt_conn* conn, bool bonded) {
    LOG_INF("pairing complete, bonded=%d", bonded);
    if (bonded && ble_out_is_host(conn)) {
        ble_out_remember_host(conn);
    }
}

static void pairing_failed(struct bt_conn* conn, enum bt_security_err reason) {
    LOG_ERR("pairing failed: %d", reason);
}

static struct bt_conn_auth_cb conn_auth_callbacks = {
    .cancel = auth_cancel,
};

static struct bt_conn_auth_info_cb conn_auth_info_callbacks = {
    .pairing_complete = pairing_complete,
    .pairing_failed = pairing_failed,
};

// --- button: short press pairs a new keyboard, long press forgets everything ---

static void disconnect_conn(struct bt_conn* conn, void* data) {
    bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
}

static void pair_new_work_fn(struct k_work* work) {
    LOG_INF("pairing mode");
    peers_only = false;
    if (keyboard_conn != NULL) {
        bt_conn_disconnect(keyboard_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
    k_work_reschedule(&scan_start_work, K_MSEC(SCAN_DELAY_MS));
}
static K_WORK_DEFINE(pair_new_work, pair_new_work_fn);

static void clear_bonds_work_fn(struct k_work* work) {
    LOG_INF("clearing bonds");
    scan_stop();
    CHK(bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY));
    ble_out_forget_host();
    bt_conn_foreach(BT_CONN_TYPE_LE, disconnect_conn, NULL);
    peers_only = false;
    k_work_reschedule(&scan_start_work, K_MSEC(SCAN_DELAY_MS));
}
static K_WORK_DEFINE(clear_bonds_work, clear_bonds_work_fn);

static void button_cb(const struct device* dev, struct gpio_callback* cb, uint32_t pins) {
    if (gpio_pin_get_dt(&button)) {
        button_pressed_at = k_uptime_get();
    } else if (button_pressed_at != 0) {
        if (k_uptime_get() - button_pressed_at > CLEAR_BONDS_BUTTON_PRESS_MS) {
            k_work_submit(&clear_bonds_work);
        } else {
            k_work_submit(&pair_new_work);
        }
        button_pressed_at = 0;
    }
}

static void io_init(void) {
    if (device_is_ready(led.port)) {
        gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
    }

    if (device_is_ready(button.port)) {
        gpio_pin_configure_dt(&button, GPIO_INPUT);
        gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_BOTH);
        gpio_init_callback(&button_cb_data, button_cb, BIT(button.pin));
        gpio_add_callback(button.port, &button_cb_data);
    }
}

// --- settings: remember which bond is the host ---

static int tag_settings_set(const char* name, size_t len, settings_read_cb read_cb, void* cb_arg) {
    if (strcmp(name, "host") == 0) {
        bt_addr_le_t host_addr;
        if ((len == sizeof(host_addr)) && (read_cb(cb_arg, &host_addr, sizeof(host_addr)) == sizeof(host_addr))) {
            ble_out_set_host_addr(&host_addr);
        }
    }
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(remapper, "remapper", NULL, tag_settings_set, NULL, NULL);

#ifdef CONFIG_USB_DEVICE_STACK
// Development boards with USB: logs go to a serial port, and sending 'B' on
// that port restarts the board into its UF2 bootloader.
static void usb_console_thread_fn(void* p1, void* p2, void* p3) {
    const struct device* console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

    int ticks = 0;

    while (true) {
        unsigned char c;
        while (device_is_ready(console) && (uart_poll_in(console, &c) == 0)) {
            if (c == 'B') {
                sys_reboot(DFU_MAGIC_UF2_RESET);
            }
        }
        if (++ticks >= 25) {
            ticks = 0;
            LOG_INF("status: keyboard=%d host=%d host_known=%d scanning=%d slow=%d peers_only=%d batt=%umV/%u%% uptime=%us",
                    keyboard_conn != NULL, ble_out_host_connected(), ble_out_host_known(), scanning, scan_slow, peers_only,
                    battery_millivolts(), battery_percent(),
                    (unsigned) (k_uptime_get() / 1000));
        }
        k_msleep(200);
    }
}
K_THREAD_DEFINE(usb_console_thread, 1024, usb_console_thread_fn, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
#endif

void main(void) {
#ifdef CONFIG_USB_DEVICE_STACK
    usb_enable(NULL);
#endif

    LOG_INF("keyboard relay starting");

    io_init();

    bt_hogp_init(&hogp, &hogp_init_params);

    CHK(bt_conn_auth_cb_register(&conn_auth_callbacks));
    CHK(bt_conn_auth_info_cb_register(&conn_auth_info_callbacks));

    if (!CHK(bt_enable(NULL))) {
        return;
    }

    ble_out_init();
    battery_init();

    settings_load();

    scan_init();

    k_work_reschedule(&scan_start_work, K_MSEC(SCAN_DELAY_MS));
    ble_out_adv_start();
}
