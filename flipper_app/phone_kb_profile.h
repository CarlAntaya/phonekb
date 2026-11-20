#pragma once

#include <furi_ble/profile_interface.h>
#include <services/serial_service.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * BLE profile for Phone Keyboard.
 *
 * Exposes the standard Flipper serial GATT service (same UUIDs as the system
 * serial profile) but under its own template, name and MAC address. Because
 * this is NOT the system serial profile, the Bt service will not attach an RPC
 * session to it, so every byte the phone writes comes straight to this app.
 */
extern const FuriHalBleProfileTemplate* const ble_profile_phone_kb;

void ble_profile_phone_kb_set_callback(
    FuriHalBleProfileBase* profile,
    uint16_t buff_size,
    SerialServiceEventCallback callback,
    void* context);

void ble_profile_phone_kb_notify_buffer_is_empty(FuriHalBleProfileBase* profile);

#ifdef __cplusplus
}
#endif
