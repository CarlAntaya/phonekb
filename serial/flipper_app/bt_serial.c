/**
 * USB <-> Bluetooth Serial Bridge for Flipper Zero (Momentum firmware)
 *
 *   PC  <--USB serial (COM port)-->  Flipper  <--BLE-->  Phone (Chrome page)
 *
 * The Flipper's USB port becomes a plain serial port while the app runs
 * (the Flipper CLI is paused). Bytes go both ways unchanged.
 *
 * BLE framing: every write / indication starts with one type byte.
 *   Phone -> Flipper
 *     0x01 DATA   payload goes to the PC
 *     0x02 HELLO  phone just connected; Flipper replies with PROBE
 *     0x03 SIZE   1 byte: how many bytes of the PROBE arrived (sets chunk size)
 *   Flipper -> Phone
 *     0x01 DATA   payload came from the PC
 *     0x02 PROBE  243 bytes; tells the phone how big an indication fits
 *     0x03 STATUS 1 byte: bit0 = a program on the PC has the port open (DTR)
 *
 * Phone -> Flipper uses the serial service's flow-control characteristic
 * (credit based). Flipper -> phone sends one indication at a time and waits
 * for the phone's confirmation.
 */

#include <furi.h>
#include <furi_hal_usb.h>
#include <furi_hal_bt.h>
#include <furi_hal_usb_cdc.h>
#include <cli/cli_vcp.h>
#include <gui/gui.h>
#include <gui/elements.h>
#include <input/input.h>
#include <bt/bt_service/bt.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>

#include "bt_serial_profile.h"

#define TAG "BtSerial"

#define KEYS_PATH APP_DATA_PATH(".bt_serial.keys")

#define VCP_CH          0
#define USB_PKT         CDC_DATA_SZ // 64
#define BLE_PKT_MAX     BLE_SVC_SERIAL_CHAR_VALUE_LEN_MAX // 243
#define BLE_CHUNK_SAFE  20 // payload that always fits (default ATT MTU 23)
#define BLE_RX_CREDITS  1024 // phone may send this many bytes before waiting
#define STREAM_SIZE     4096
#define BLE_TX_TIMEOUT  1500
#define USB_TX_TIMEOUT  250

#define PKT_DATA   0x01
#define PKT_HELLO  0x02 // phone->flipper
#define PKT_PROBE  0x02 // flipper->phone
#define PKT_SIZE   0x03 // phone->flipper
#define PKT_STATUS 0x03 // flipper->phone

typedef enum {
    EvtStop = (1 << 0),
    EvtCdcRx = (1 << 1),
    EvtCdcTxDone = (1 << 2),
    EvtBleRx = (1 << 3),
    EvtBleTxDone = (1 << 4),
    EvtBtStatus = (1 << 5),
    EvtCtrlLine = (1 << 6),
    EvtHello = (1 << 7),
    EvtSize = (1 << 8),
    EvtForget = (1 << 9),
} WorkerEvt;
#define EVT_ALL                                                                        \
    (EvtStop | EvtCdcRx | EvtCdcTxDone | EvtBleRx | EvtBleTxDone | EvtBtStatus |      \
     EvtCtrlLine | EvtHello | EvtSize | EvtForget)

typedef enum {
    PhoneOff,
    PhoneAdvertising,
    PhoneConnected, // link up, waiting for HELLO (pairing happens here)
    PhoneReady, // HELLO/SIZE done, data flows
} PhoneState;

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* input_queue;
    NotificationApp* notifications;
    Bt* bt;
    CliVcp* cli_vcp;
    FuriHalBleProfileBase* profile;
    FuriThread* worker;

    FuriStreamBuffer* usb2ble; // PC -> phone
    FuriStreamBuffer* ble2usb; // phone -> PC

    // written from callbacks, read by worker / GUI
    volatile BtStatus bt_status;
    volatile uint8_t probe_len;
    volatile uint32_t ble_rx_overflow;

    // worker / GUI state
    volatile PhoneState phone;
    volatile bool pc_open;
    volatile uint32_t pc_to_phone;
    volatile uint32_t phone_to_pc;
    volatile uint32_t dropped;
    volatile uint16_t chunk; // BLE payload bytes per indication
} App;

/* ---------------- USB CDC callbacks (interrupt context) ---------------- */

static void cdc_tx_done(void* ctx) {
    App* app = ctx;
    furi_thread_flags_set(furi_thread_get_id(app->worker), EvtCdcTxDone);
}

static void cdc_rx(void* ctx) {
    App* app = ctx;
    furi_thread_flags_set(furi_thread_get_id(app->worker), EvtCdcRx);
}

static void cdc_state(void* ctx, CdcState state) {
    UNUSED(ctx);
    UNUSED(state);
}

static void cdc_ctrl_line(void* ctx, CdcCtrlLine lines) {
    UNUSED(lines);
    App* app = ctx;
    furi_thread_flags_set(furi_thread_get_id(app->worker), EvtCtrlLine);
}

static void cdc_config(void* ctx, struct usb_cdc_line_coding* config) {
    UNUSED(ctx);
    UNUSED(config); // baud rate is irrelevant: it's a virtual port
}

static CdcCallbacks cdc_callbacks = {
    .tx_ep_callback = cdc_tx_done,
    .rx_ep_callback = cdc_rx,
    .state_callback = cdc_state,
    .ctrl_line_callback = cdc_ctrl_line,
    .config_callback = cdc_config,
};

/* ---------------- BLE callbacks (BLE event thread) ---------------- */

static uint16_t ble_serial_callback(SerialServiceEvent event, void* ctx) {
    App* app = ctx;
    if(event.event == SerialServiceEventTypeDataReceived && event.data.size > 0) {
        uint8_t* buf = event.data.buffer;
        uint16_t size = event.data.size;
        switch(buf[0]) {
        case PKT_DATA:
            if(size > 1) {
                size_t sent = furi_stream_buffer_send(app->ble2usb, buf + 1, size - 1, 0);
                if(sent < (size_t)(size - 1)) app->ble_rx_overflow += (size - 1) - sent;
                furi_thread_flags_set(furi_thread_get_id(app->worker), EvtBleRx);
            }
            break;
        case PKT_HELLO:
            furi_thread_flags_set(furi_thread_get_id(app->worker), EvtHello);
            break;
        case PKT_SIZE:
            if(size > 1) {
                app->probe_len = buf[1];
                furi_thread_flags_set(furi_thread_get_id(app->worker), EvtSize);
            }
            break;
        default:
            break;
        }
        return furi_stream_buffer_spaces_available(app->ble2usb);
    } else if(event.event == SerialServiceEventTypeDataSent) {
        furi_thread_flags_set(furi_thread_get_id(app->worker), EvtBleTxDone);
    }
    return 0;
}

static void bt_status_callback(BtStatus status, void* ctx) {
    App* app = ctx;
    app->bt_status = status;
    furi_thread_flags_set(furi_thread_get_id(app->worker), EvtBtStatus);
}

/* ---------------- worker ---------------- */

static void worker_reset_ble_link(App* app) {
    // fresh flow-control credits for the phone on every new connection
    ble_profile_bt_serial_set_callback(app->profile, BLE_RX_CREDITS, ble_serial_callback, app);
}

static int32_t worker_thread(void* ctx) {
    App* app = ctx;
    uint8_t usb_buf[USB_PKT];
    uint8_t ble_buf[BLE_PKT_MAX];

    bool ble_tx_busy = false;
    uint32_t ble_tx_at = 0;
    bool usb_tx_busy = false;
    uint32_t usb_tx_at = 0;
    bool cdc_pending = true; // check once at start
    bool status_dirty = false;
    bool probe_pending = false;

    app->pc_open = furi_hal_cdc_get_ctrl_line_state(VCP_CH) & CdcCtrlLineDTR;

    for(;;) {
        uint32_t ev = furi_thread_flags_wait(EVT_ALL, FuriFlagWaitAny, 20);
        if(ev & FuriFlagError) ev = 0; // timeout: just poll
        if(ev & EvtStop) break;

        if(ev & EvtBtStatus) {
            BtStatus st = app->bt_status;
            if(st == BtStatusConnected) {
                if(app->phone < PhoneConnected) {
                    app->phone = PhoneConnected;
                    worker_reset_ble_link(app);
                }
            } else {
                app->phone = (st == BtStatusAdvertising) ? PhoneAdvertising : PhoneOff;
                ble_tx_busy = false;
                probe_pending = false;
                furi_stream_buffer_reset(app->usb2ble);
            }
            view_port_update(app->view_port);
        }

        if(ev & EvtForget) {
            app->phone = PhoneOff;
            bt_disconnect(app->bt);
            furi_delay_ms(200);
            furi_hal_bt_stop_advertising();
            bt_forget_bonded_devices(app->bt);
            furi_hal_bt_start_advertising();
            notification_message(app->notifications, &sequence_single_vibro);
        }

        if(ev & EvtHello) {
            // phone (re)started its session: measure how big an indication can be
            app->phone = PhoneConnected;
            app->chunk = BLE_CHUNK_SAFE;
            probe_pending = true;
            ble_tx_busy = false;
            furi_stream_buffer_reset(app->usb2ble);
        }

        if(ev & EvtSize) {
            uint16_t got = app->probe_len;
            // got = bytes of the PROBE that arrived, including the type byte
            uint16_t chunk = (got > 1) ? (got - 1) : BLE_CHUNK_SAFE;
            if(chunk > BLE_PKT_MAX - 1) chunk = BLE_PKT_MAX - 1;
            if(chunk < BLE_CHUNK_SAFE) chunk = BLE_CHUNK_SAFE;
            app->chunk = chunk;
            app->phone = PhoneReady;
            status_dirty = true;
            FURI_LOG_I(TAG, "Phone ready, chunk %u", chunk);
            notification_message(app->notifications, &sequence_blink_green_100);
            view_port_update(app->view_port);
        }

        if(ev & (EvtCtrlLine | EvtCdcTxDone)) {
            bool open = furi_hal_cdc_get_ctrl_line_state(VCP_CH) & CdcCtrlLineDTR;
            if(open != app->pc_open) {
                app->pc_open = open;
                status_dirty = true;
                view_port_update(app->view_port);
            }
        }

        if(ev & EvtCdcTxDone) usb_tx_busy = false;
        if(ev & EvtBleTxDone) ble_tx_busy = false;
        if(ev & EvtCdcRx) cdc_pending = true;

        uint32_t now = furi_get_tick();
        if(ble_tx_busy && (now - ble_tx_at) > BLE_TX_TIMEOUT) ble_tx_busy = false;
        if(usb_tx_busy && (now - usb_tx_at) > USB_TX_TIMEOUT) usb_tx_busy = false;

        /* ---- PC -> Flipper: read USB while there is room to keep it ---- */
        while(cdc_pending && furi_stream_buffer_spaces_available(app->usb2ble) >= USB_PKT) {
            int32_t n = furi_hal_cdc_receive(VCP_CH, usb_buf, USB_PKT);
            if(n <= 0) {
                cdc_pending = false;
                break;
            }
            if(app->phone == PhoneReady) {
                furi_stream_buffer_send(app->usb2ble, usb_buf, n, 0);
            } else {
                app->dropped += n; // nobody to send it to
                view_port_update(app->view_port);
            }
        }

        /* ---- Flipper -> phone: one indication at a time ---- */
        if(!ble_tx_busy && app->phone >= PhoneConnected) {
            size_t len = 0;
            if(probe_pending) {
                ble_buf[0] = PKT_PROBE;
                for(size_t i = 1; i < BLE_PKT_MAX; i++)
                    ble_buf[i] = (uint8_t)i;
                len = BLE_PKT_MAX;
                probe_pending = false;
            } else if(app->phone == PhoneReady && status_dirty) {
                ble_buf[0] = PKT_STATUS;
                ble_buf[1] = app->pc_open ? 1 : 0;
                len = 2;
                status_dirty = false;
            } else if(app->phone == PhoneReady) {
                size_t n = furi_stream_buffer_receive(app->usb2ble, ble_buf + 1, app->chunk, 0);
                if(n) {
                    ble_buf[0] = PKT_DATA;
                    len = n + 1;
                    app->pc_to_phone += n;
                    view_port_update(app->view_port);
                }
            }
            if(len) {
                if(ble_profile_bt_serial_tx(app->profile, ble_buf, len)) {
                    ble_tx_busy = true;
                    ble_tx_at = now;
                } else {
                    FURI_LOG_W(TAG, "BLE tx failed");
                }
            }
        }

        /* ---- phone -> PC ---- */
        if(!usb_tx_busy) {
            size_t n = furi_stream_buffer_receive(app->ble2usb, usb_buf, USB_PKT, 0);
            if(n) {
                if(app->pc_open) {
                    furi_hal_cdc_send(VCP_CH, usb_buf, n);
                    usb_tx_busy = true;
                    usb_tx_at = now;
                    app->phone_to_pc += n;
                } else {
                    app->dropped += n; // no program on the PC is listening
                }
                view_port_update(app->view_port);
            }
        }
        if(app->phone >= PhoneConnected && furi_stream_buffer_is_empty(app->ble2usb)) {
            // gives the phone a fresh batch of credits if it ran out
            ble_profile_bt_serial_notify_buffer_is_empty(app->profile);
        }
    }
    return 0;
}

/* ---------------- GUI ---------------- */

static void draw_callback(Canvas* canvas, void* ctx) {
    App* app = ctx;
    char line[40];
    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "USB <-> BT Serial");

    canvas_set_font(canvas, FontSecondary);
    const char* phone = "Bluetooth off";
    switch(app->phone) {
    case PhoneAdvertising:
        phone = "Waiting for phone";
        break;
    case PhoneConnected:
        phone = "Connecting/pairing";
        break;
    case PhoneReady:
        phone = "Connected";
        break;
    default:
        break;
    }
    snprintf(line, sizeof(line), "Phone: %s", phone);
    canvas_draw_str(canvas, 2, 22, line);
    snprintf(line, sizeof(line), "PC port: %s", app->pc_open ? "open" : "closed");
    canvas_draw_str(canvas, 2, 32, line);
    snprintf(line, sizeof(line), "PC>Phone: %lu B", (unsigned long)app->pc_to_phone);
    canvas_draw_str(canvas, 2, 42, line);
    snprintf(line, sizeof(line), "Phone>PC: %lu B", (unsigned long)app->phone_to_pc);
    canvas_draw_str(canvas, 2, 52, line);
    if(app->dropped) {
        snprintf(line, sizeof(line), "drop %lu", (unsigned long)app->dropped);
        canvas_draw_str_aligned(canvas, 126, 52, AlignRight, AlignBottom, line);
    }
    canvas_draw_str(canvas, 2, 63, "Hold < forget phone");
}

static void input_callback(InputEvent* event, void* ctx) {
    App* app = ctx;
    furi_message_queue_put(app->input_queue, event, FuriWaitForever);
}

/* ---------------- main ---------------- */

int32_t bt_serial_app(void* p) {
    UNUSED(p);
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));
    app->chunk = BLE_CHUNK_SAFE;

    app->input_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->usb2ble = furi_stream_buffer_alloc(STREAM_SIZE, 1);
    app->ble2usb = furi_stream_buffer_alloc(STREAM_SIZE, 1);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, draw_callback, app);
    view_port_input_callback_set(app->view_port, input_callback, app);
    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    app->worker = furi_thread_alloc_ex("BtSerialWorker", 2048, worker_thread, app);

    // --- USB: turn the Flipper's port into a plain serial port ---
    FuriHalUsbInterface* usb_prev = furi_hal_usb_get_config();
    app->cli_vcp = furi_record_open(RECORD_CLI_VCP);
    cli_vcp_disable(app->cli_vcp);
    furi_hal_usb_unlock();
    furi_check(furi_hal_usb_set_config(&usb_cdc_single, NULL));

    // --- Bluetooth: our own profile, name and pairing keys ---
    app->bt = furi_record_open(RECORD_BT);
    bt_disconnect(app->bt);
    furi_delay_ms(200); // let the 2nd core update its NVM
    bt_keys_storage_set_storage_path(app->bt, KEYS_PATH);
    app->profile = bt_profile_start(app->bt, ble_profile_bt_serial, NULL);
    furi_check(app->profile);
    furi_hal_bt_start_advertising();

    furi_thread_start(app->worker);
    worker_reset_ble_link(app);
    furi_hal_cdc_set_callbacks(VCP_CH, &cdc_callbacks, app);
    bt_set_status_changed_callback(app->bt, bt_status_callback, app);

    InputEvent event;
    for(bool running = true; running;) {
        if(furi_message_queue_get(app->input_queue, &event, 100) != FuriStatusOk) continue;
        if(event.key == InputKeyBack && event.type == InputTypeShort) running = false;
        if(event.key == InputKeyBack && event.type == InputTypeLong) running = false;
        if(event.key == InputKeyLeft && event.type == InputTypeLong) {
            furi_thread_flags_set(furi_thread_get_id(app->worker), EvtForget);
        }
    }

    // --- teardown ---
    bt_set_status_changed_callback(app->bt, NULL, NULL);
    furi_hal_cdc_set_callbacks(VCP_CH, NULL, NULL);
    furi_thread_flags_set(furi_thread_get_id(app->worker), EvtStop);
    furi_thread_join(app->worker);
    furi_thread_free(app->worker);

    bt_disconnect(app->bt);
    furi_delay_ms(200);
    bt_keys_storage_set_default_path(app->bt);
    furi_check(bt_profile_restore_default(app->bt));
    furi_record_close(RECORD_BT);

    furi_hal_usb_unlock();
    furi_check(furi_hal_usb_set_config(usb_prev ? usb_prev : &usb_cdc_single, NULL));
    cli_vcp_enable(app->cli_vcp);
    furi_record_close(RECORD_CLI_VCP);

    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    furi_stream_buffer_free(app->usb2ble);
    furi_stream_buffer_free(app->ble2usb);
    furi_message_queue_free(app->input_queue);
    free(app);
    return 0;
}
