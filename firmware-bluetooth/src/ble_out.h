#ifndef _BLE_OUT_H_
#define _BLE_OUT_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/bluetooth/addr.h>
#include <zephyr/bluetooth/conn.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bluetooth HID output: the remapper acts as a BLE keyboard (peripheral role)
// towards a host, in addition to being a central for the input devices.

int ble_out_init(void);
void ble_out_adv_start(void);

// True if this connection is the one where we are the peripheral (to the host).
bool ble_out_is_host(struct bt_conn* conn);
void ble_out_connected(struct bt_conn* conn, uint8_t conn_err);
void ble_out_disconnected(struct bt_conn* conn);
bool ble_out_host_connected(void);
// True if a host has been paired (whether or not it's connected right now).
bool ble_out_host_known(void);

// Remember which bond belongs to the host so that the scanner doesn't treat it as an input device.
void ble_out_remember_host(struct bt_conn* conn);
void ble_out_set_host_addr(const bt_addr_le_t* addr);
bool ble_out_is_host_addr(const bt_addr_le_t* addr);
void ble_out_forget_host(void);

bool ble_out_send(uint8_t report_id, const uint8_t* data, uint8_t len);

#ifdef __cplusplus
}
#endif

#endif
