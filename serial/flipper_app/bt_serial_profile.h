#pragma once

#include <furi_ble/profile_interface.h>
#include <services/serial_service.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * BLE profile for the USB <-> Bluetooth serial bridge.
 *
 * Exposes the standard Flipper serial GATT service (same UUIDs as the system
 * serial profile) but under its own template, name and MAC address. Because
 * this is NOT the system serial profile, the Bt service does not attach an RPC
 * session to it, so every byte the phone writes comes straight to this app.
 */
extern const FuriHalBleProfileTemplate* const ble_profile_bt_serial;

void ble_profile_bt_serial_set_callback(
    FuriHalBleProfileBase* profile,
    uint16_t buff_size,
    SerialServiceEventCallback callback,
    void* context);

void ble_profile_bt_serial_notify_buffer_is_empty(FuriHalBleProfileBase* profile);

bool ble_profile_bt_serial_tx(FuriHalBleProfileBase* profile, uint8_t* data, uint16_t size);

#ifdef __cplusplus
}
#endif
