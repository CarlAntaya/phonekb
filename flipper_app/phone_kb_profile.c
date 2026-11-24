#include "phone_kb_profile.h"

#include <furi.h>
#include <furi_hal_version.h>
#include <gap.h>
#include <ble/core/ble_defs.h>
#include <services/dev_info_service.h>
#include <services/battery_service.h>
#include <services/serial_service.h>

typedef struct {
    FuriHalBleProfileBase base;
    BleServiceDevInfo* dev_info_svc;
    BleServiceBattery* battery_svc;
    BleServiceSerial* serial_svc;
} BleProfilePhoneKb;
_Static_assert(offsetof(BleProfilePhoneKb, base) == 0, "Wrong layout");

static FuriHalBleProfileBase* ble_profile_phone_kb_start(FuriHalBleProfileParams params) {
    UNUSED(params);
    BleProfilePhoneKb* profile = malloc(sizeof(BleProfilePhoneKb));
    profile->base.config = ble_profile_phone_kb;
    profile->dev_info_svc = ble_svc_dev_info_start();
    profile->battery_svc = ble_svc_battery_start(true);
    profile->serial_svc = ble_svc_serial_start();
    return &profile->base;
}

static void ble_profile_phone_kb_stop(FuriHalBleProfileBase* profile) {
    furi_check(profile);
    furi_check(profile->config == ble_profile_phone_kb);
    BleProfilePhoneKb* p = (BleProfilePhoneKb*)profile;
    ble_svc_battery_stop(p->battery_svc);
    ble_svc_dev_info_stop(p->dev_info_svc);
    ble_svc_serial_stop(p->serial_svc);
    free(p); // allocated in ble_profile_phone_kb_start
}

// 7.5 ms .. 45 ms connection interval: low latency for mouse movement
#define CONNECTION_INTERVAL_MIN (0x06)
#define CONNECTION_INTERVAL_MAX (0x24)

static const GapConfig phone_kb_template_config = {
#ifdef PHONE_KB_LEGACY_GAP
    // Momentum mntm-007 and older: build with cdefines=["PHONE_KB_LEGACY_GAP"]
    .adv_service_uuid = 0xFE60,
#else
    .adv_service =
        {
            .UUID_Type = UUID_TYPE_16,
            // Not the Flipper (0x308x) UUID, so the official Flipper app
            // won't try to grab this connection.
            .Service_UUID_16 = 0xFE60,
        },
#endif
    .appearance_char = 0x03C1, // Keyboard
    .bonding_mode = true,
    // Flipper shows a 6-digit PIN, you type it into Android's pairing dialog.
    // The serial characteristics require an authenticated link, so random
    // nearby devices can't inject keystrokes into your PC.
    .pairing_method = GapPairingPinCodeShow,
    .conn_param =
        {
            .conn_int_min = CONNECTION_INTERVAL_MIN,
            .conn_int_max = CONNECTION_INTERVAL_MAX,
            .slave_latency = 0,
            .supervisor_timeout = 0,
        },
};

static void ble_profile_phone_kb_get_config(GapConfig* config, FuriHalBleProfileParams params) {
    UNUSED(params);
    furi_check(config);
    memcpy(config, &phone_kb_template_config, sizeof(GapConfig));

    // Own MAC, distinct from the system profile (+0) and the HID remote (+1),
    // so Android keeps a separate bond for this app.
    memcpy(config->mac_address, furi_hal_version_get_ble_mac(), sizeof(config->mac_address));
    config->mac_address[2] += 2;
    config->mac_address[0] ^= 0x5B;

    // First char of the local name is the AD type byte; keep it.
    snprintf(
        config->adv_name,
        sizeof(config->adv_name),
        "%cPhoneKB %s",
        furi_hal_version_get_ble_local_device_name_ptr()[0],
        furi_hal_version_get_name_ptr());
}

static const FuriHalBleProfileTemplate profile_callbacks = {
    .start = ble_profile_phone_kb_start,
    .stop = ble_profile_phone_kb_stop,
    .get_gap_config = ble_profile_phone_kb_get_config,
};

const FuriHalBleProfileTemplate* const ble_profile_phone_kb = &profile_callbacks;

void ble_profile_phone_kb_set_callback(
    FuriHalBleProfileBase* profile,
    uint16_t buff_size,
    SerialServiceEventCallback callback,
    void* context) {
    furi_check(profile && (profile->config == ble_profile_phone_kb));
    BleProfilePhoneKb* p = (BleProfilePhoneKb*)profile;
    ble_svc_serial_set_callbacks(p->serial_svc, buff_size, callback, context);
}

void ble_profile_phone_kb_notify_buffer_is_empty(FuriHalBleProfileBase* profile) {
    furi_check(profile && (profile->config == ble_profile_phone_kb));
    BleProfilePhoneKb* p = (BleProfilePhoneKb*)profile;
    ble_svc_serial_notify_buffer_is_empty(p->serial_svc);
}
