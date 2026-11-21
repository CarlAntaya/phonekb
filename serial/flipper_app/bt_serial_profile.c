#include "bt_serial_profile.h"

#include <furi.h>
#include <furi_hal_version.h>
#include <gap.h>
#include <ble/ble.h>
#include <services/dev_info_service.h>
#include <services/battery_service.h>
#include <services/serial_service.h>

typedef struct {
    FuriHalBleProfileBase base;
    BleServiceDevInfo* dev_info_svc;
    BleServiceBattery* battery_svc;
    BleServiceSerial* serial_svc;
} BleProfileBtSerial;
_Static_assert(offsetof(BleProfileBtSerial, base) == 0, "Wrong layout");

static FuriHalBleProfileBase* ble_profile_bt_serial_start(FuriHalBleProfileParams params) {
    UNUSED(params);
    BleProfileBtSerial* profile = malloc(sizeof(BleProfileBtSerial));
    profile->base.config = ble_profile_bt_serial;
    profile->dev_info_svc = ble_svc_dev_info_start();
    profile->battery_svc = ble_svc_battery_start(true);
    profile->serial_svc = ble_svc_serial_start();
    return &profile->base;
}

static void ble_profile_bt_serial_stop(FuriHalBleProfileBase* profile) {
    furi_check(profile);
    furi_check(profile->config == ble_profile_bt_serial);
    BleProfileBtSerial* p = (BleProfileBtSerial*)profile;
    ble_svc_battery_stop(p->battery_svc);
    ble_svc_dev_info_stop(p->dev_info_svc);
    ble_svc_serial_stop(p->serial_svc);
}

// 7.5 ms .. 45 ms connection interval: snappy for a terminal
#define CONNECTION_INTERVAL_MIN (0x06)
#define CONNECTION_INTERVAL_MAX (0x24)

static const GapConfig bt_serial_template_config = {
    .adv_service =
        {
            .UUID_Type = UUID_TYPE_16,
            // Not the Flipper (0x308x) UUID, so the official Flipper app
            // won't try to grab this connection.
            .Service_UUID_16 = 0xFE60,
        },
    .appearance_char = 0x0080, // Generic computer
    .bonding_mode = true,
    // Flipper shows a 6-digit PIN, you type it into Android's pairing dialog.
    // The serial characteristics require an authenticated (encrypted) link.
    .pairing_method = GapPairingPinCodeShow,
    .conn_param =
        {
            .conn_int_min = CONNECTION_INTERVAL_MIN,
            .conn_int_max = CONNECTION_INTERVAL_MAX,
            .slave_latency = 0,
            .supervisor_timeout = 0,
        },
};

static void ble_profile_bt_serial_get_config(GapConfig* config, FuriHalBleProfileParams params) {
    UNUSED(params);
    furi_check(config);
    memcpy(config, &bt_serial_template_config, sizeof(GapConfig));

    // Own MAC, distinct from the system profile (+0), the HID remote (+1)
    // and Phone Keyboard (+2), so Android keeps a separate bond for this app.
    memcpy(config->mac_address, furi_hal_version_get_ble_mac(), sizeof(config->mac_address));
    config->mac_address[2] += 3;
    config->mac_address[0] ^= 0x3C;

    // First char of the local name is the AD type byte; keep it.
    snprintf(
        config->adv_name,
        sizeof(config->adv_name),
        "%cSerialBT %s",
        furi_hal_version_get_ble_local_device_name_ptr()[0],
        furi_hal_version_get_name_ptr());
}

static const FuriHalBleProfileTemplate profile_callbacks = {
    .start = ble_profile_bt_serial_start,
    .stop = ble_profile_bt_serial_stop,
    .get_gap_config = ble_profile_bt_serial_get_config,
};

const FuriHalBleProfileTemplate* const ble_profile_bt_serial = &profile_callbacks;

void ble_profile_bt_serial_set_callback(
    FuriHalBleProfileBase* profile,
    uint16_t buff_size,
    SerialServiceEventCallback callback,
    void* context) {
    furi_check(profile && (profile->config == ble_profile_bt_serial));
    BleProfileBtSerial* p = (BleProfileBtSerial*)profile;
    ble_svc_serial_set_callbacks(p->serial_svc, buff_size, callback, context);
}

void ble_profile_bt_serial_notify_buffer_is_empty(FuriHalBleProfileBase* profile) {
    furi_check(profile && (profile->config == ble_profile_bt_serial));
    BleProfileBtSerial* p = (BleProfileBtSerial*)profile;
    ble_svc_serial_notify_buffer_is_empty(p->serial_svc);
}

bool ble_profile_bt_serial_tx(FuriHalBleProfileBase* profile, uint8_t* data, uint16_t size) {
    furi_check(profile && (profile->config == ble_profile_bt_serial));
    BleProfileBtSerial* p = (BleProfileBtSerial*)profile;
    return ble_svc_serial_update_tx(p->serial_svc, data, size);
}
