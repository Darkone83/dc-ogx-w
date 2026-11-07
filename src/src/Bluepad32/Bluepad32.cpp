#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <pico/mutex.h>
#include <pico/cyw43_arch.h>
#include <pico/time.h>

#include <btstack.h>
#include <ble/gatt_client.h>
#include <ble/gatt-service/hids_host.h>
#include "btstack_run_loop.h"
#include "gap.h"
#include "uni.h"
#include "bt/uni_bt.h"
#include "bt/uni_bt_bredr.h"
#include "bt/uni_bt_le.h"
#include "uni_hid_device.h"

#include "sdkconfig.h"

/** Core0 status/LED and USB mux read a coherent radio-owned connection flag. */
static std::atomic<bool> s_bt_any_connected_cached{false};
// Diagnostics stay independent of the RemoteControl mailbox. A calibration
// snapshot can go stale even when the raw HID callback is still running.
static std::atomic<uint32_t> s_diag_ready_events{0}, s_diag_disconnect_events{0};
static std::atomic<uint32_t> s_diag_callbacks{0}, s_diag_gamepad_reports{0};
static std::atomic<uint32_t> s_diag_last_report_ms{0};
static std::atomic<int32_t> s_diag_bt_slot{-1}, s_diag_output_slot{-1};
static std::atomic<uint32_t> s_diag_type{0}, s_diag_hids_cid{0};
static std::atomic<bool> s_diag_ble{false};
// Passive connection diagnostics. No Bluetooth writes, polling, or extra timers.
// Every field is atomic because Core1 records events and Core0 serves /api/status.
enum : uint32_t { DIAG_NONE, DIAG_LINK, DIAG_PAIRING, DIAG_READY, DIAG_REPORTS, DIAG_DISCONNECTED, DIAG_REJECTED };
enum : uint32_t { FAIL_NONE, FAIL_PAIRING, FAIL_REENCRYPT, FAIL_AUTH, FAIL_NO_OUTPUT, FAIL_MODE, FAIL_EARLY_DISCONNECT, FAIL_ENCRYPTION, FAIL_LINK };
static std::atomic<uint32_t> s_diag_stage{DIAG_NONE}, s_diag_stage_ms{0}, s_diag_ready_ms{0};
static std::atomic<uint32_t> s_diag_fail{FAIL_NONE}, s_diag_fail_code{0}, s_diag_fail_ms{0};
static std::atomic<uint32_t> s_diag_pairing_state{0}, s_diag_pairing_status{0}, s_diag_pairing_reason{0};
static std::atomic<uint32_t> s_diag_disconnect_reason{0}, s_diag_conn_handle{0xffff};
static std::atomic<uint32_t> s_diag_vid{0}, s_diag_pid{0};
static std::atomic<uint8_t> s_diag_name[25]{};
// Read-only HIDS-host telemetry from the pinned BTstack C implementation.
// Kinds: 1 HIDS ready, 2 CCCD request, 3 CCCD result,
//        4 notification, 5 GET result, 6 Exit Suspend, 7 GET request.
static std::atomic<uint32_t> s_hids_setup_status{UINT32_MAX};
static std::atomic<uint32_t> s_hids_cccd_attempts{0}, s_hids_cccd_acks{0};
static std::atomic<uint32_t> s_hids_cccd_ok{0}, s_hids_cccd_errors{0};
static std::atomic<uint32_t> s_hids_cccd_request_status{UINT32_MAX};
static std::atomic<uint32_t> s_hids_cccd_last_status{UINT32_MAX};
static std::atomic<uint32_t> s_hids_notifications{0}, s_hids_get_results{0};
/* Passive wildcard listener: counts all GATT notifications before HIDS report matching. */
static std::atomic<uint32_t> s_raw_gatt_notifications{0}, s_raw_gatt_last_ms{0};
static std::atomic<uint32_t> s_raw_gatt_last_value_handle{0};
static gatt_client_notification_t s_raw_gatt_listener{};
static void diag_raw_gatt_event(uint8_t packet_type, uint16_t, uint8_t* packet, uint16_t size) {
    if (packet_type != HCI_EVENT_PACKET || size < 12 ||
        hci_event_packet_get_type(packet) != GATT_EVENT_NOTIFICATION) return;
    if (gatt_event_notification_get_handle(packet) !=
        s_diag_conn_handle.load(std::memory_order_relaxed)) return;
    s_raw_gatt_notifications.fetch_add(1, std::memory_order_relaxed);
    s_raw_gatt_last_ms.store(to_ms_since_boot(get_absolute_time()), std::memory_order_relaxed);
    s_raw_gatt_last_value_handle.store(
        gatt_event_notification_get_value_handle(packet), std::memory_order_relaxed);
}
static std::atomic<uint32_t> s_hids_last_origin{0}, s_hids_last_report_id{0};
static std::atomic<uint32_t> s_hids_last_notification_ms{0};
static std::atomic<uint32_t> s_hids_last_get_ms{0};
static std::atomic<uint32_t> s_hids_exit_suspend_calls{0};
static std::atomic<uint32_t> s_hids_exit_suspend_status{UINT32_MAX};
static std::atomic<uint32_t> s_hids_get_requests{0}, s_hids_get_rejected{0};
static std::atomic<uint32_t> s_hids_get_request_status{UINT32_MAX};

extern "C" void dc_ogx_hids_trace_event(uint16_t conn_handle, uint8_t event, uint16_t detail) {
    // Called synchronously on Core1 by BTstack, without allocation, locks or I/O.
    if (conn_handle != s_diag_conn_handle.load(std::memory_order_relaxed)) return;
    const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    switch (event) {
        case 1: s_hids_setup_status.store(detail, std::memory_order_relaxed); break;
        case 2:
            s_hids_cccd_attempts.fetch_add(1, std::memory_order_relaxed);
            s_hids_cccd_request_status.store(detail, std::memory_order_relaxed);
            if (detail != 0) s_hids_cccd_errors.fetch_add(1, std::memory_order_relaxed);
            break;
        case 3:
            s_hids_cccd_acks.fetch_add(1, std::memory_order_relaxed);
            s_hids_cccd_last_status.store(detail, std::memory_order_relaxed);
            if (detail == 0) s_hids_cccd_ok.fetch_add(1, std::memory_order_relaxed);
            else s_hids_cccd_errors.fetch_add(1, std::memory_order_relaxed);
            break;
        case 4:
            s_hids_notifications.fetch_add(1, std::memory_order_relaxed);
            s_hids_last_notification_ms.store(now_ms, std::memory_order_relaxed);
            s_hids_last_origin.store(1, std::memory_order_relaxed);
            s_hids_last_report_id.store(detail, std::memory_order_relaxed);
            break;
        case 5:
            s_hids_get_results.fetch_add(1, std::memory_order_relaxed);
            s_hids_last_get_ms.store(now_ms, std::memory_order_relaxed);
            s_hids_last_origin.store(2, std::memory_order_relaxed);
            s_hids_last_report_id.store(detail, std::memory_order_relaxed);
            break;
        case 6:
            s_hids_exit_suspend_calls.fetch_add(1, std::memory_order_relaxed);
            s_hids_exit_suspend_status.store(detail, std::memory_order_relaxed);
            break;
        case 7:
            s_hids_get_requests.fetch_add(1, std::memory_order_relaxed);
            s_hids_get_request_status.store(detail, std::memory_order_relaxed);
            if (detail != 0) s_hids_get_rejected.fetch_add(1, std::memory_order_relaxed);
            break;
    }
}

static uint32_t diag_now() { return to_ms_since_boot(get_absolute_time()); }
static void diag_stage(uint32_t stage) {
    s_diag_stage.store(stage, std::memory_order_relaxed);
    s_diag_stage_ms.store(diag_now(), std::memory_order_relaxed);
}
static void diag_failure(uint32_t kind, uint32_t code) {
    s_diag_fail.store(kind, std::memory_order_relaxed);
    s_diag_fail_code.store(code, std::memory_order_relaxed);
    s_diag_fail_ms.store(diag_now(), std::memory_order_relaxed);
}
// Only printable, JSON-safe ASCII is exposed over HTTP; never forward raw BT names.
static void diag_set_name(const char* name) {
    bool finished = false;
    for (unsigned i = 0; i < 25; ++i) {
        uint8_t ch = 0;
        if (i != 24 && !finished && name && name[i]) {
            ch = static_cast<uint8_t>(name[i]);
            if (ch < 32 || ch > 126 || ch == '\"' || ch == '\\') ch = '_';
        } else finished = true;
        s_diag_name[i].store(ch, std::memory_order_relaxed);
    }
}
// Before Bluepad32 reaches DEVICE_READY, only the HCI handle may be known.
static void diag_new_link(uint16_t handle, bool le) {
    s_diag_callbacks.store(0, std::memory_order_relaxed);
    s_diag_gamepad_reports.store(0, std::memory_order_relaxed);
    s_diag_last_report_ms.store(0, std::memory_order_relaxed);
    s_diag_ready_ms.store(0, std::memory_order_relaxed);
    s_diag_fail.store(FAIL_NONE, std::memory_order_relaxed);
    s_diag_fail_code.store(0, std::memory_order_relaxed);
    s_diag_fail_ms.store(0, std::memory_order_relaxed);
    s_diag_pairing_state.store(0, std::memory_order_relaxed);
    s_diag_pairing_status.store(0, std::memory_order_relaxed);
    s_diag_pairing_reason.store(0, std::memory_order_relaxed);
    s_diag_disconnect_reason.store(0, std::memory_order_relaxed);
    s_diag_conn_handle.store(handle, std::memory_order_relaxed);
    s_diag_bt_slot.store(-1, std::memory_order_relaxed);
    s_diag_output_slot.store(-1, std::memory_order_relaxed);
    s_diag_type.store(0, std::memory_order_relaxed);
    s_diag_vid.store(0, std::memory_order_relaxed);
    s_diag_pid.store(0, std::memory_order_relaxed);
    s_diag_hids_cid.store(0, std::memory_order_relaxed);
    s_hids_setup_status.store(UINT32_MAX, std::memory_order_relaxed);
    s_hids_cccd_attempts.store(0, std::memory_order_relaxed);
    s_hids_cccd_acks.store(0, std::memory_order_relaxed);
    s_hids_cccd_ok.store(0, std::memory_order_relaxed);
    s_hids_cccd_errors.store(0, std::memory_order_relaxed);
    s_hids_cccd_request_status.store(UINT32_MAX, std::memory_order_relaxed);
    s_hids_cccd_last_status.store(UINT32_MAX, std::memory_order_relaxed);
    s_hids_notifications.store(0, std::memory_order_relaxed);
    s_raw_gatt_notifications.store(0, std::memory_order_relaxed);
    s_raw_gatt_last_ms.store(0, std::memory_order_relaxed);
    s_raw_gatt_last_value_handle.store(0, std::memory_order_relaxed);
    s_hids_get_results.store(0, std::memory_order_relaxed);
    s_hids_last_origin.store(0, std::memory_order_relaxed);
    s_hids_last_report_id.store(0, std::memory_order_relaxed);
    s_hids_last_notification_ms.store(0, std::memory_order_relaxed);
    s_hids_last_get_ms.store(0, std::memory_order_relaxed);
    s_hids_exit_suspend_calls.store(0, std::memory_order_relaxed);
    s_hids_exit_suspend_status.store(UINT32_MAX, std::memory_order_relaxed);
    s_hids_get_requests.store(0, std::memory_order_relaxed);
    s_hids_get_rejected.store(0, std::memory_order_relaxed);
    s_hids_get_request_status.store(UINT32_MAX, std::memory_order_relaxed);
    s_diag_ble.store(le, std::memory_order_relaxed);
    diag_set_name(nullptr);
    diag_stage(DIAG_LINK);
}
static void diag_connected_device(uni_hid_device_t* device) {
    const bool new_attempt = s_diag_conn_handle.load(std::memory_order_relaxed) != device->conn.handle ||
        s_diag_stage.load(std::memory_order_relaxed) == DIAG_DISCONNECTED ||
        s_diag_stage.load(std::memory_order_relaxed) == DIAG_NONE;
    if (new_attempt)
        diag_new_link(device->conn.handle, device->conn.handle != UNI_BT_CONN_HANDLE_INVALID &&
                      gap_get_connection_type(device->conn.handle) == GAP_CONNECTION_LE);
    // The HCI listener can record SMP errors before Bluepad32 calls connected.
    // Preserve them when the higher-level callback finally arrives.
    s_diag_conn_handle.store(device->conn.handle, std::memory_order_relaxed);
    s_diag_bt_slot.store(uni_hid_device_get_idx_for_instance(device), std::memory_order_relaxed);
    s_diag_type.store(static_cast<uint32_t>(device->controller_type), std::memory_order_relaxed);
    s_diag_vid.store(static_cast<uint32_t>(device->vendor_id), std::memory_order_relaxed);
    s_diag_pid.store(static_cast<uint32_t>(device->product_id), std::memory_order_relaxed);
    s_diag_hids_cid.store(static_cast<uint32_t>(device->hids_cid), std::memory_order_relaxed);
    s_diag_ble.store(device->conn.handle != UNI_BT_CONN_HANDLE_INVALID &&
                     gap_get_connection_type(device->conn.handle) == GAP_CONNECTION_LE,
                     std::memory_order_relaxed);
    diag_set_name(device->name);
}
// Observe existing BTstack events only; the upstream event handlers still own pairing.
static btstack_packet_callback_registration_t s_diag_hci_listener{};
static btstack_packet_callback_registration_t s_diag_sm_listener{};
static void diag_hci_event(uint8_t type, uint16_t, uint8_t* packet, uint16_t size) {
    if (type != HCI_EVENT_PACKET || !packet || size < 3) return;
    const uint8_t event = hci_event_packet_get_type(packet);
    if (event == HCI_EVENT_LE_META && size >= 6 &&
        hci_event_le_meta_get_subevent_code(packet) == HCI_SUBEVENT_LE_CONNECTION_COMPLETE) {
        const uint8_t status = hci_subevent_le_connection_complete_get_status(packet);
        const uint16_t handle = hci_subevent_le_connection_complete_get_connection_handle(packet);
        if (status == ERROR_CODE_SUCCESS) {
            // Can precede the Bluepad32 on_device_connected callback by the entire GATT setup.
            auto* device = uni_hid_device_get_instance_for_connection_handle(handle);
            if (device) diag_connected_device(device);
            else diag_new_link(handle, true);
        } else {
            // Failed HCI connection has no valid handle to correlate to a controller.
            diag_new_link(0xffff, true);
            diag_failure(FAIL_LINK, status);
            diag_stage(DIAG_REJECTED);
        }
    } else if (event == HCI_EVENT_DISCONNECTION_COMPLETE && size >= 6 &&
        hci_event_disconnection_complete_get_connection_handle(packet) == s_diag_conn_handle.load(std::memory_order_relaxed)) {
        s_diag_disconnect_reason.store(hci_event_disconnection_complete_get_reason(packet), std::memory_order_relaxed);
        if (s_diag_stage.load(std::memory_order_relaxed) < DIAG_READY &&
            s_diag_fail.load(std::memory_order_relaxed) == FAIL_NONE)
            diag_failure(FAIL_EARLY_DISCONNECT, 0);
        diag_stage(DIAG_DISCONNECTED);
    } else if (event == HCI_EVENT_AUTHENTICATION_COMPLETE && size >= 5 &&
        hci_event_authentication_complete_get_connection_handle(packet) == s_diag_conn_handle.load(std::memory_order_relaxed)) {
        const uint8_t status = hci_event_authentication_complete_get_status(packet);
        if (status != ERROR_CODE_SUCCESS) diag_failure(FAIL_AUTH, status);
    } else if (event == HCI_EVENT_ENCRYPTION_CHANGE && size >= 6 &&
        hci_event_encryption_change_get_connection_handle(packet) == s_diag_conn_handle.load(std::memory_order_relaxed)) {
        const uint8_t status = hci_event_encryption_change_get_status(packet);
        if (status != ERROR_CODE_SUCCESS) diag_failure(FAIL_ENCRYPTION, status);
    }
}
static void diag_sm_event(uint8_t type, uint16_t, uint8_t* packet, uint16_t size) {
    if (type != HCI_EVENT_PACKET || !packet || size < 3) return;
    const uint8_t event = hci_event_packet_get_type(packet);
    if (event == SM_EVENT_PAIRING_STARTED) {
        const uint16_t handle = sm_event_pairing_started_get_handle(packet);
        if (handle == s_diag_conn_handle.load(std::memory_order_relaxed)) {
            s_diag_pairing_state.store(1, std::memory_order_relaxed);
            diag_stage(DIAG_PAIRING);
        }
    } else if (event == SM_EVENT_PAIRING_COMPLETE) {
        if (sm_event_pairing_complete_get_handle(packet) != s_diag_conn_handle.load(std::memory_order_relaxed)) return;
        const uint8_t status = sm_event_pairing_complete_get_status(packet);
        s_diag_pairing_status.store(status, std::memory_order_relaxed);
        const bool already_ready = s_diag_ready_ms.load(std::memory_order_relaxed) != 0;
        const bool ignored_post_ready_timeout =
            status == ERROR_CODE_CONNECTION_TIMEOUT && already_ready;
        s_diag_pairing_state.store(ignored_post_ready_timeout ? 4u :
                                   (status == ERROR_CODE_SUCCESS ? 2u : 3u),
                                   std::memory_order_relaxed);
        if (status != ERROR_CODE_SUCCESS && !ignored_post_ready_timeout) {
            const uint8_t reason = sm_event_pairing_complete_get_reason(packet);
            s_diag_pairing_reason.store(reason, std::memory_order_relaxed);
            diag_failure(FAIL_PAIRING, status);
        }
    } else if (event == SM_EVENT_REENCRYPTION_COMPLETE) {
        if (sm_event_reencryption_complete_get_handle(packet) != s_diag_conn_handle.load(std::memory_order_relaxed)) return;
        const uint8_t status = sm_event_reencryption_complete_get_status(packet);
        if (status != ERROR_CODE_SUCCESS) diag_failure(FAIL_REENCRYPT, status);
    }
}


#include "Bluepad32/Bluepad32.h"
#include "Bluepad32/ClassicPairingDebug.h"
#include "Board/board_api.h"
#include "Board/ogxm_log.h"
#if defined(CONFIG_OGXM_NETWORK)
#include "NetworkService/NetworkService.h"
#include "RemoteControl/RemoteControl.h"
#endif
#include "Input/InputSlot.h"
#include "USBHost/HostDriver/FlydigiApex4Wukong/FlydigiApex4WukongBtProbe.h"
#include "USBHost/HostDriver/FlydigiApex4Wukong/FlydigiApex4WukongBt.h"
#include "USBHost/HostDriver/GameSirCyclone2/Cyclone2BtProbe.h"
#include "parser/uni_hid_parser_ds5.h"
#include "controller/uni_controller.h"
#include "parser/uni_hid_parser_wii.h"
#include "Gamepad/MotionImu.h"
#include "USBDevice/DeviceDriver/MotionOutputActive.h"
#include "USBDevice/DeviceDriver/Steam/SteamActive.h"
#include "USBDevice/DeviceDriver/Steam/SteamBtReport.h"
#include "USBDevice/DeviceDriver/Steam/SteamPassthrough.h"
#include "USBDevice/DeviceDriver/Steam/SteamTouchpad.h"

#ifndef CONFIG_BLUEPAD32_PLATFORM_CUSTOM
    #error "Pico W must use BLUEPAD32_PLATFORM_CUSTOM"
#endif

static_assert((CONFIG_BLUEPAD32_MAX_DEVICES >= MAX_GAMEPADS),
              "Bluepad32 must allow at least as many BT devices as USB gamepad slots");

namespace bluepad32 {

// Updated exclusively by the serialized Core1 radio run loop.
static bool s_remote_wifi_mode = false;
static bool remote_bt_allowed() {
#if defined(CONFIG_OGXM_NETWORK)
    return !s_remote_wifi_mode && RemoteControl::bluetooth_input_allowed();
#else
    return !s_remote_wifi_mode;
#endif
}

#if defined(CONFIG_OGXM_DEBUG)
static void log_ogx_slots(const char* tag) {
    printf("\n%s\n", tag ? tag : "[INPUT SLOTS]");
    for (uint8_t i = 0; i < MAX_GAMEPADS; ++i) {
        const InputSlot::State st = InputSlot::get(i);
        if (st.transport == InputTransport::USB) {
            printf("INPUT SLOT %u\ntransport=USB\ndriver=%s\naddr=%u\ninstance=%u\n",
                   static_cast<unsigned>(i), InputSlot::driver_name(st.physical_driver),
                   static_cast<unsigned>(st.usb_addr), static_cast<unsigned>(st.usb_instance));
        } else {
            printf("INPUT SLOT %u\ntransport=%s\n", static_cast<unsigned>(i),
                   InputSlot::transport_name(st.transport));
        }
    }
}
#endif

/** Prefer a free OGX pad; never reuse a USB-owned pad for Bluetooth output. */
static int resolve_bt_output_pad_idx(int preferred_idx) {
    if (preferred_idx >= 0 && preferred_idx < static_cast<int>(MAX_GAMEPADS) &&
        !InputSlot::usb_owns(static_cast<uint8_t>(preferred_idx))) {
        return preferred_idx;
    }
    for (uint8_t i = 0; i < MAX_GAMEPADS; ++i) {
        if (!InputSlot::usb_owns(i)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

static bool bp32_is_switch_joycon(const uni_hid_device_t* d) {
    return d != nullptr && (d->controller_type == CONTROLLER_TYPE_SwitchJoyConLeft ||
                            d->controller_type == CONTROLLER_TYPE_SwitchJoyConRight);
}

static bool bp32_is_joycon_pair_secondary(const uni_hid_device_t* d) {
    if (uni_hid_parser_switch2_is_ble_device(d))
        return uni_hid_parser_switch2_is_joycon_pair_secondary(d);
    if (bp32_is_switch_joycon(d))
        return uni_hid_parser_switch_is_joycon_pair_secondary(d);
    return false;
}

static int bp32_get_gamepad_output_idx(uni_hid_device_t* d) {
    if (uni_hid_parser_switch2_is_ble_device(d))
        return uni_hid_parser_switch2_get_gamepad_output_idx(d);
    if (bp32_is_switch_joycon(d))
        return uni_hid_parser_switch_get_gamepad_output_idx(d);
    return uni_hid_device_get_idx_for_instance(d);
}

static int bp32_get_pair_partner_idx(uni_hid_device_t* d) {
    if (uni_hid_parser_switch2_is_ble_device(d))
        return uni_hid_parser_switch2_get_pair_partner_idx(d);
    if (bp32_is_switch_joycon(d))
        return uni_hid_parser_switch_get_pair_partner_idx(d);
    return -1;
}

static void bp32_disconnect_controller_and_joycon_partner(uni_hid_device_t* d) {
    if (!d)
        return;
    const int partner_idx = bp32_get_pair_partner_idx(d);
    if (partner_idx >= 0 && partner_idx < CONFIG_BLUEPAD32_MAX_DEVICES) {
        uni_hid_device_t* partner = uni_hid_device_get_instance_for_idx(partner_idx);
        if (partner && partner != d) {
            printf("[BP32] Disconnect combo: also disconnecting Joy-Con partner slot %d\n", partner_idx);
            uni_hid_device_disconnect(partner);
        }
    }
    uni_hid_device_disconnect(d);
}

static constexpr uint32_t FEEDBACK_TIME_MS = 250;
static constexpr uint32_t LED_CHECK_TIME_MS = 500;
/** Idle pairing health check — restarts BR/LE scan if they died during long USB suspend (e.g. 360 standby). */
static constexpr uint32_t PAIRING_WATCHDOG_MS = 45000;
/** If no HID input report reaches us for this long while "connected", the BT link is zombie
 *  (L2CAP stops delivering; OG Xbox then holds last USB report). Force disconnect so user can reconnect. */
static constexpr uint32_t BT_INPUT_STALL_DISCONNECT_MS = 8000;
/** BLE Xbox: host→pad output while idle (no rumble) or controller sleeps link ~1 min */
static constexpr uint32_t XBOX_BLE_KEEPALIVE_MS = 12000;
/** Switch 2 Pro BLE drops link (HCI 0x08) without periodic vibration writes. */
static constexpr uint32_t SW2_BLE_KEEPALIVE_MS = 8;

/** One-second rumble when a pad becomes ready so the user knows it is connected. */
static constexpr uint16_t CONNECT_RUMBLE_DURATION_MS = 1000;
static constexpr uint8_t CONNECT_RUMBLE_WEAK = 160;
static constexpr uint8_t CONNECT_RUMBLE_STRONG = 160;
/** DS4: defer FF slightly — early output reports can destabilize the link (see s_ps4_rumble_ok_ms). */
static constexpr uint16_t CONNECT_RUMBLE_DELAY_PS4_MS = 1200;
static constexpr uint16_t CONNECT_RUMBLE_DELAY_DEFAULT_MS = 300;

static uint32_t s_last_bt_input_ms[CONFIG_BLUEPAD32_MAX_DEVICES]{};
/* BTstack/HIDS reports stop after the first Xbox GET reply in the currently
 * observed failure. Keep a bounded backup GET available from the application's
 * existing Core1 feedback timer, without modifying upstream Bluepad32 code. */
static bool s_bt_seen_hid_report[CONFIG_BLUEPAD32_MAX_DEVICES]{};
static uint32_t s_xbox_ble_last_recovery_get_ms[CONFIG_BLUEPAD32_MAX_DEVICES]{};
static uint32_t s_xbox_ble_ka_last_ms[CONFIG_BLUEPAD32_MAX_DEVICES]{};
static uint32_t s_sw2_ble_ka_last_ms[CONFIG_BLUEPAD32_MAX_DEVICES]{};
/** Ignore Start+Select disconnect combo for this long after connect (DS4 can glitch both on first reports). */
static uint32_t s_bt_disconnect_combo_grace_until_ms[MAX_GAMEPADS]{};
/** DS4 BT: delay rumble output (host can request rumble immediately; early FF reports can drop link). */
static uint32_t s_ps4_rumble_ok_ms[MAX_GAMEPADS]{};

struct BTDevice {
    bool connected{false};
    Gamepad* gamepad{nullptr};
};

BTDevice bt_devices_[CONFIG_BLUEPAD32_MAX_DEVICES];
btstack_timer_source_t feedback_timer_;
btstack_timer_source_t led_timer_;
bool led_timer_set_{false};
bool feedback_timer_set_{false};

/** Core0 USB mux may run before Core1 finishes uni_init(); BTstack asserts if
 *  execute_on_main_thread is used with the_run_loop == NULL (boot loop with pad plugged). */
static std::atomic<bool> s_btstack_run_loop_ready{false};
/** Wired USB asked to silence BT before the stack was up — apply after init. */
static std::atomic<bool> s_bt_quiet_for_usb_pending{false};

static constexpr uint32_t GPIO_PROCESS_INTERVAL_MS = 4;
static btstack_timer_source_t gpio_process_timer_;
static void (*gpio_process_cb_)(void*) = nullptr;
static void* gpio_process_ctx_ = nullptr;

static void (*s_pico_w_pio_usb_mux_tick)(void) = nullptr;
static btstack_timer_source_t s_pico_w_usb_mux_timer_;
static btstack_context_callback_registration_t s_pico_w_usb_mux_main_reg;

// Timer callbacks must not call sleep_* (Pico panics). TinyUSB host enumeration does; run mux on main thread.
static void pico_w_usb_mux_run_on_main(void* ctx) {
    (void)ctx;
    if (s_pico_w_pio_usb_mux_tick != nullptr) {
        s_pico_w_pio_usb_mux_tick();
    }
}

static void pico_w_usb_mux_timer_cb(btstack_timer_source_t* ts) {
    s_pico_w_usb_mux_main_reg.callback = pico_w_usb_mux_run_on_main;
    s_pico_w_usb_mux_main_reg.context = nullptr;
    btstack_run_loop_execute_on_main_thread(&s_pico_w_usb_mux_main_reg);
    btstack_run_loop_set_timer(ts, 1);
    btstack_run_loop_add_timer(ts);
}

static void gpio_process_timer_cb(btstack_timer_source_t* ts) {
    if (gpio_process_cb_ != nullptr && gpio_process_ctx_ != nullptr) {
        gpio_process_cb_(gpio_process_ctx_);
    }
    btstack_run_loop_set_timer(ts, GPIO_PROCESS_INTERVAL_MS);
    btstack_run_loop_add_timer(ts);
}

// PS5: touchpad click toggles adaptive triggers (per-controller state)
static bool adaptive_trigger_enabled_[MAX_GAMEPADS]{false};
static bool prev_touchpad_clicked_[MAX_GAMEPADS]{false};
// Defer sending adaptive trigger effect out of BT callback to avoid l2cap_send in callback (reduces input lag)
static bool pending_adaptive_trigger_send_[MAX_GAMEPADS]{false};

bool any_connected()
{
    return s_bt_any_connected_cached.load(std::memory_order_acquire);
}

HidFlowStats hid_flow_stats(uint32_t now_ms) {
    HidFlowStats r{};
    r.ready_events = s_diag_ready_events.load(std::memory_order_relaxed);
    r.disconnect_events = s_diag_disconnect_events.load(std::memory_order_relaxed);
    r.callbacks = s_diag_callbacks.load(std::memory_order_relaxed);
    r.gamepad_reports = s_diag_gamepad_reports.load(std::memory_order_relaxed);
    const uint32_t last = s_diag_last_report_ms.load(std::memory_order_relaxed);
    r.last_gamepad_age_ms = last ? static_cast<uint32_t>(now_ms - last) : UINT32_MAX;
    r.bt_slot = s_diag_bt_slot.load(std::memory_order_relaxed);
    r.output_slot = s_diag_output_slot.load(std::memory_order_relaxed);
    r.controller_type = s_diag_type.load(std::memory_order_relaxed);
    r.hids_cid = s_diag_hids_cid.load(std::memory_order_relaxed);
    r.bluetooth_le = s_diag_ble.load(std::memory_order_relaxed);
    for (unsigned i = 0; i < sizeof(r.name); ++i)
        r.name[i] = static_cast<char>(s_diag_name[i].load(std::memory_order_relaxed));
    r.name[sizeof(r.name) - 1] = 0;
    r.vendor_id = s_diag_vid.load(std::memory_order_relaxed);
    r.product_id = s_diag_pid.load(std::memory_order_relaxed);
    r.stage = s_diag_stage.load(std::memory_order_relaxed);
    const uint32_t event_ms = s_diag_stage_ms.load(std::memory_order_relaxed);
    r.stage_age_ms = event_ms ? now_ms - event_ms : UINT32_MAX;
    const uint32_t ready_ms = s_diag_ready_ms.load(std::memory_order_relaxed);
    r.ready_age_ms = ready_ms ? now_ms - ready_ms : UINT32_MAX;
    r.failure_kind = s_diag_fail.load(std::memory_order_relaxed);
    r.failure_code = s_diag_fail_code.load(std::memory_order_relaxed);
    const uint32_t failure_ms = s_diag_fail_ms.load(std::memory_order_relaxed);
    r.failure_age_ms = failure_ms ? now_ms - failure_ms : UINT32_MAX;
    r.pairing_state = s_diag_pairing_state.load(std::memory_order_relaxed);
    r.pairing_status = s_diag_pairing_status.load(std::memory_order_relaxed);
    r.pairing_reason = s_diag_pairing_reason.load(std::memory_order_relaxed);
    r.disconnect_reason = s_diag_disconnect_reason.load(std::memory_order_relaxed);
    r.hids_setup_status = s_hids_setup_status.load(std::memory_order_relaxed);
    r.cccd_attempts = s_hids_cccd_attempts.load(std::memory_order_relaxed);
    r.cccd_acks = s_hids_cccd_acks.load(std::memory_order_relaxed);
    r.cccd_ok = s_hids_cccd_ok.load(std::memory_order_relaxed);
    r.cccd_errors = s_hids_cccd_errors.load(std::memory_order_relaxed);
    r.cccd_request_status = s_hids_cccd_request_status.load(std::memory_order_relaxed);
    r.cccd_last_status = s_hids_cccd_last_status.load(std::memory_order_relaxed);
    r.notifications = s_hids_notifications.load(std::memory_order_relaxed);
    r.raw_gatt_notifications = s_raw_gatt_notifications.load(std::memory_order_relaxed);
    r.raw_gatt_value_handle = s_raw_gatt_last_value_handle.load(std::memory_order_relaxed);
    const uint32_t raw_ms = s_raw_gatt_last_ms.load(std::memory_order_relaxed);
    r.raw_gatt_age_ms = raw_ms ? now_ms - raw_ms : UINT32_MAX;
    r.get_results = s_hids_get_results.load(std::memory_order_relaxed);
    r.last_origin = s_hids_last_origin.load(std::memory_order_relaxed);
    r.last_report_id = s_hids_last_report_id.load(std::memory_order_relaxed);
    r.notification_age_ms = s_hids_last_notification_ms.load(std::memory_order_relaxed);
    r.notification_age_ms = r.notification_age_ms ? now_ms - r.notification_age_ms : UINT32_MAX;
    r.get_result_age_ms = s_hids_last_get_ms.load(std::memory_order_relaxed);
    r.get_result_age_ms = r.get_result_age_ms ? now_ms - r.get_result_age_ms : UINT32_MAX;
    r.exit_suspend_calls = s_hids_exit_suspend_calls.load(std::memory_order_relaxed);
    r.exit_suspend_status = s_hids_exit_suspend_status.load(std::memory_order_relaxed);
    r.get_requests = s_hids_get_requests.load(std::memory_order_relaxed);
    r.get_rejected = s_hids_get_rejected.load(std::memory_order_relaxed);
    r.get_request_status = s_hids_get_request_status.load(std::memory_order_relaxed);
    return r;
}

bool is_wii_controller_connected(uint8_t idx)
{
    if (idx >= MAX_GAMEPADS) {
        return false;
    }
    uni_hid_device_t* bp_device = uni_hid_device_get_instance_for_idx(idx);
    return (bp_device != nullptr && bp_device->controller_type == CONTROLLER_TYPE_WiiController);
}

bool any_wii_controller_connected()
{
    for (uint8_t i = 0; i < MAX_GAMEPADS; ++i)
    {
        if (bt_devices_[i].connected && is_wii_controller_connected(i))
        {
            return true;
        }
    }
    return false;
}

/** TV-style Wiimote (not Wii U Pro, Classic, or Balance Board). */
static bool ogxm_is_handheld_wiimote(const uni_hid_device_t* device)
{
    if (device == nullptr || device->controller_type != CONTROLLER_TYPE_WiiController) {
        return false;
    }
    switch (device->controller_subtype) {
        case CONTROLLER_SUBTYPE_WIIUPRO:
        case CONTROLLER_SUBTYPE_WII_CLASSIC:
        case CONTROLLER_SUBTYPE_WII_BALANCE_BOARD:
            return false;
        default:
            return true;
    }
}

//This solves a function pointer/crash issue with bluepad32
void set_rumble(uni_hid_device_t* bp_device, uint16_t length, uint8_t rumble_l, uint8_t rumble_r)
{
    switch (bp_device->controller_type)
    {
        case CONTROLLER_TYPE_XBoxOneController:
            uni_hid_parser_xboxone_play_dual_rumble(bp_device, 0, length + 10, rumble_l, rumble_r);
            break;
        case CONTROLLER_TYPE_AndroidController:
            if (bp_device->vendor_id == UNI_HID_PARSER_STADIA_VID && bp_device->product_id == UNI_HID_PARSER_STADIA_PID) 
            {
                uni_hid_parser_stadia_play_dual_rumble(bp_device, 0, length, rumble_l, rumble_r);
            }
            break;
        case CONTROLLER_TYPE_PSMoveController:
            uni_hid_parser_psmove_play_dual_rumble(bp_device, 0, length, rumble_l, rumble_r);
            break;
        case CONTROLLER_TYPE_PS3Controller:
            uni_hid_parser_ds3_play_dual_rumble(bp_device, 0, length, rumble_l, rumble_r);
            break;
        case CONTROLLER_TYPE_PS4Controller:
            uni_hid_parser_ds4_play_dual_rumble(bp_device, 0, length, rumble_l, rumble_r);
            break;
        case CONTROLLER_TYPE_PS5Controller:
            uni_hid_parser_ds5_play_dual_rumble(bp_device, 0, length, rumble_l, rumble_r);
            break;
        case CONTROLLER_TYPE_WiiController:
            uni_hid_parser_wii_play_dual_rumble(bp_device, 0, length, rumble_l, rumble_r);
            break;
        case CONTROLLER_TYPE_SwitchProController:
        case CONTROLLER_TYPE_SwitchJoyConRight:
        case CONTROLLER_TYPE_SwitchJoyConLeft:
            uni_hid_parser_switch_play_dual_rumble(bp_device, 0, length, rumble_l, rumble_r);
            break;
        case CONTROLLER_TYPE_Switch2ProController:
        case CONTROLLER_TYPE_Switch2JoyConRight:
        case CONTROLLER_TYPE_Switch2JoyConLeft:
            uni_hid_parser_switch2_play_dual_rumble(bp_device, 0, length, rumble_l, rumble_r);
            break;
        case CONTROLLER_TYPE_SteamControllerTriton:
            uni_hid_parser_steam_triton_play_dual_rumble(bp_device, 0, length, rumble_l, rumble_r);
            break;
        default:
            break;
    }
}

static void send_feedback_cb(btstack_timer_source *ts)
{
    uni_hid_device_t* bp_device = nullptr;
    const uint32_t now_ms = to_ms_since_boot(get_absolute_time());

    for (uint8_t i = 0; i < CONFIG_BLUEPAD32_MAX_DEVICES; ++i)
    {
        if (!bt_devices_[i].connected ||
            !(bp_device = uni_hid_device_get_instance_for_idx(i)))
        {
            continue;
        }
        /* BLE Xbox (Series) and Switch 2 BLE may not send reports every poll interval. */
        const bool skip_stall_disconnect =
            (bp_device->controller_type == CONTROLLER_TYPE_XBoxOneController && bp_device->hids_cid != 0) ||
            uni_hid_parser_switch2_is_ble_device(bp_device) ||
            uni_hid_parser_steam_triton_is_device(bp_device);
        /* Virtual slot (e.g. DS4's BT "mouse"): never gets gamepad HID → would always stall-disconnect
         * and drop the real controller. */
        if (uni_hid_device_is_virtual_device(bp_device))
            goto after_stall_check;
        if (uni_hid_parser_switch2_is_joycon_pair_secondary(bp_device))
            goto after_stall_check;
        if (bp32_is_joycon_pair_secondary(bp_device))
            goto after_stall_check;
        if (!skip_stall_disconnect)
        {
        const int stall_idx = bp32_get_gamepad_output_idx(bp_device);
        const unsigned stall_slot =
            (stall_idx >= 0 && stall_idx < static_cast<int>(MAX_GAMEPADS))
                ? static_cast<unsigned>(stall_idx)
                : static_cast<unsigned>(i);
        if (s_last_bt_input_ms[stall_slot] != 0 &&
            (now_ms - s_last_bt_input_ms[stall_slot]) > BT_INPUT_STALL_DISCONNECT_MS)
        {
            printf("[Bluepad32] BT input stalled (%u ms); forcing disconnect (slot %u)\n",
                   static_cast<unsigned>(now_ms - s_last_bt_input_ms[stall_slot]), static_cast<unsigned>(i));
            uni_hid_device_disconnect(bp_device);
            continue;
        }
        }
    after_stall_check:

#if defined(CONFIG_TARGET_PICO_W)
        /* Genuine Microsoft Xbox over LE only. The pinned 2026 parser stops
         * its startup GET poll after the FIRST HID event, even if that event
         * was itself a GET response and no notifications ever arrived.
         *
         * Don't alter the upstream parser, pairing/security, or other pads.
         * Once a real input was observed and it becomes stale, request the
         * current state on our EXISTING feedback timer. The Device diagnostic
         * panel already counts both requests and replies, so failures stay
         * visible (COMMAND_DISALLOWED, no GET replies, etc.).
         *
         * This is a bounded fallback/diagnostic, not a claim of 60 Hz input
         * and not a replacement for repairing missing notifications. */
        if (bp_device->vendor_id == 0x045eu &&
            bp_device->controller_type == CONTROLLER_TYPE_XBoxOneController &&
            bp_device->conn.handle != UNI_BT_CONN_HANDLE_INVALID &&
            gap_get_connection_type(bp_device->conn.handle) == GAP_CONNECTION_LE &&
            bp_device->hids_cid != 0 && bp_device->hids_cid != 0xffff &&
            s_bt_seen_hid_report[i] && s_last_bt_input_ms[i] != 0 &&
            static_cast<uint32_t>(now_ms - s_last_bt_input_ms[i]) >= 400u &&
            (s_xbox_ble_last_recovery_get_ms[i] == 0u ||
             static_cast<uint32_t>(now_ms - s_xbox_ble_last_recovery_get_ms[i]) >= 250u)) {
            s_xbox_ble_last_recovery_get_ms[i] = now_ms;
            /* Called on the radio/BTstack core; never from Core0 or HTTP. */
            const uint8_t result = hids_host_send_get_report(
                bp_device->hids_cid, 0x01, HID_REPORT_TYPE_INPUT);
#if defined(CONFIG_OGXM_DEBUG)
            if (result != ERROR_CODE_SUCCESS) {
                printf("[Xbox BLE] recovery GET refused: 0x%02x\n", result);
            }
#else
            (void)result;
#endif
        }
#endif

        // Bluepad32 may use BT slot 1 even though this build exposes only one
        // Xbox pad. Feedback (including Xbox BLE keepalive) must use the same
        // physical output mapping as the HID input callback, not raw BT slot.
        const int out_i = bp32_get_gamepad_output_idx(bp_device);
        const int gp_idx = resolve_bt_output_pad_idx(out_i >= 0 ? out_i : static_cast<int>(i));
        if (gp_idx < 0 || gp_idx >= static_cast<int>(MAX_GAMEPADS) || bt_devices_[gp_idx].gamepad == nullptr)
            continue;

        Gamepad::PadOut gp_out = bt_devices_[gp_idx].gamepad->get_pad_out();
        if (bp_device->controller_type == CONTROLLER_TYPE_XBoxOneController && bp_device->hids_cid != 0 &&
            gp_out.rumble_l == 0 && gp_out.rumble_r == 0)
        {
            const uint32_t last_ka = s_xbox_ble_ka_last_ms[i];
            if (last_ka == 0u || (now_ms - last_ka) >= XBOX_BLE_KEEPALIVE_MS)
            {
                uni_hid_parser_xboxone_ble_keepalive(bp_device);
                s_xbox_ble_ka_last_ms[i] = now_ms;
            }
        }
        if (uni_hid_parser_switch2_is_ble_device(bp_device) && uni_hid_parser_switch2_is_ready(bp_device) &&
            !uni_hid_parser_switch2_is_joycon_pair_secondary(bp_device) &&
            !uni_hid_parser_switch2_keepalive_timer_active(bp_device) &&
            gp_out.rumble_l == 0 && gp_out.rumble_r == 0)
        {
            const uint32_t last_ka = s_sw2_ble_ka_last_ms[i];
            if (last_ka == 0u || (now_ms - last_ka) >= SW2_BLE_KEEPALIVE_MS)
            {
                uni_hid_parser_switch2_send_keepalive(bp_device);
                s_sw2_ble_ka_last_ms[i] = now_ms;
            }
        }
        if (gp_out.rumble_l > 0 || gp_out.rumble_r > 0)
        {
            if (bp_device->controller_type == CONTROLLER_TYPE_PS4Controller &&
                now_ms < s_ps4_rumble_ok_ms[gp_idx])
                ;
            else if (!bp32_is_joycon_pair_secondary(bp_device))
            {
                set_rumble(bp_device, static_cast<uint16_t>(FEEDBACK_TIME_MS), gp_out.rumble_l, gp_out.rumble_r);
                const int partner = bp32_get_pair_partner_idx(bp_device);
                if (partner >= 0 && partner < CONFIG_BLUEPAD32_MAX_DEVICES) {
                    uni_hid_device_t* pd = uni_hid_device_get_instance_for_idx(partner);
                    if (pd)
                        set_rumble(pd, static_cast<uint16_t>(FEEDBACK_TIME_MS), gp_out.rumble_l, gp_out.rumble_r);
                }
            }
        }
    }
    if (feedback_timer_set_)
	{
        btstack_run_loop_set_timer(ts, FEEDBACK_TIME_MS);
        btstack_run_loop_add_timer(ts);
	}
}

static void check_led_cb(btstack_timer_source *ts)
{
    static bool led_state = false;

    led_state = !led_state;

#if defined(CONFIG_TARGET_PICO_W) && defined(CONFIG_EN_USB_HOST)
    const bool wired_host_pad = board_api::usb::host_any_pad_mounted();
#else
    const bool wired_host_pad = false;
#endif
    /* Solid LED when a BT pad is connected or a wired USB host controller is active (Pico W mux). */
    board_api::set_led((any_connected() || wired_host_pad) ? true : led_state);

    btstack_run_loop_set_timer(ts, LED_CHECK_TIME_MS);
    btstack_run_loop_add_timer(ts);
}

//BT Driver

static void init(int argc, const char** arg_V) {
}

static void init_complete_cb(void) {
    // Keep Bluepad32 defaults (inquiry=3, max=5, min=4 in 1.28s units).
    // Shorter inquiry (2) breaks discovery for 8BitDo SN30 Pro / Pro 2 in D-input /
    // Android modes — see uni_bt_defines.h. (#86)
    uni_bt_set_gap_inquiry_length(UNI_BT_INQUIRY_LENGTH);
    uni_bt_set_gap_max_peridic_length(UNI_BT_MAX_PERIODIC_LENGTH);
    uni_bt_set_gap_min_peridic_length(UNI_BT_MIN_PERIODIC_LENGTH);

    uni_bt_enable_new_connections_unsafe(remote_bt_allowed());
    // uni_bt_del_keys_unsafe();  // use -DOGXM_BT_CLEAR_KEYS_ON_BOOT=1 or ogxm_bt_debug_clear_keys()
    uni_property_dump_all();
    ogxm_classic_pairing_debug_init();
    OGXM_LOG("BT: stack ready — BR/EDR inquiry + BLE scan (8BitDo: use Switch or Android mode)\n");
}

static uni_error_t device_discovered_cb(bd_addr_t addr, const char* name, uint16_t cod, uint8_t rssi) {
    if (!remote_bt_allowed()) return UNI_ERROR_IGNORE_DEVICE;
    uint8_t minor = cod & UNI_BT_COD_MINOR_MASK;

    if (!(minor & (UNI_BT_COD_MINOR_GAMEPAD |
                   UNI_BT_COD_MINOR_JOYSTICK |
                   UNI_BT_COD_MINOR_REMOTE_CONTROL))) {
        return UNI_ERROR_IGNORE_DEVICE;
    }

    /* Cyclone 2 green/"Game Pair Mode" is not a usable direct-BT XInput gamepad. */
    if (gamesir_cyclone2_bt_should_ignore_discovered(name)) {
        gamesir_cyclone2_bt_on_discovered(addr, name, cod, rssi);
        return UNI_ERROR_IGNORE_DEVICE;
    }

#if defined(CONFIG_OGXM_DEBUG)
    printf("\n[BT DEVICE FOUND]\naddr=%s\nname=%s\n", bd_addr_to_str(addr),
           name && name[0] ? name : "(none)");
    log_ogx_slots("BEFORE (discovery; BT idx != OGX slot)");
#endif
    (void)rssi;
    flydigi_apex4_bt_on_discovered(addr, name, cod, rssi);
    gamesir_cyclone2_bt_on_discovered(addr, name, cod, rssi);
    return UNI_ERROR_SUCCESS;
}

static void device_connected_cb(uni_hid_device_t* device) {
    if (device == nullptr) {
        return;
    }
    diag_connected_device(device);
    if (!remote_bt_allowed()) {
        diag_failure(FAIL_MODE, 0);
        diag_stage(DIAG_REJECTED);
        uni_hid_device_disconnect(device);
        return;
    }
    /* Safety net if name arrives only after ACL create. */
    if (gamesir_cyclone2_bt_reject_game_pair_mode_if_needed(device)) {
        return;
    }
    const int bt_idx = uni_hid_device_get_idx_for_instance(device);
#if defined(CONFIG_OGXM_DEBUG)
    printf("\n[BT DEVICE CREATE]\nbt_index=%d\nname=%s\n", bt_idx,
           device->name[0] ? device->name : "(none)");
    log_ogx_slots("BEFORE create/connect");
#endif
    flydigi_apex4_bt_on_connected(device);
    gamesir_cyclone2_bt_on_connected(device);
    if (uni_hid_parser_switch2_is_ble_device(device)) {
        OGXM_LOG("SW2: connected pid=0x%04x — waiting for encryption/GATT\n", device->product_id);
    }
#if defined(CONFIG_OGXM_DEBUG)
    log_ogx_slots("AFTER create/connect (USB slots must be unchanged)");
    if (bt_idx >= 0 && bt_idx < static_cast<int>(MAX_GAMEPADS) &&
        InputSlot::usb_owns(static_cast<uint8_t>(bt_idx))) {
        printf("[BT] NOTE: Bluepad idx=%d overlaps USB-owned OGX slot %d — "
               "will not write PadIn / reset that slot\n",
               bt_idx, bt_idx);
    }
#else
    (void)bt_idx;
#endif
}

/** CYW43: resume OGX BLE advertising when no Classic (BR/EDR) gamepad remains connected. */
static void ogxm_resume_ble_ads_if_no_acl_pad(int disconnected_idx) {
    if (!remote_bt_allowed()) return;
#if defined(CONFIG_TARGET_PICO_W)
    for (uint8_t i = 0; i < CONFIG_BLUEPAD32_MAX_DEVICES; ++i) {
        if (i == static_cast<unsigned>(disconnected_idx))
            continue;
        if (!bt_devices_[i].connected)
            continue;
        uni_hid_device_t* d = uni_hid_device_get_instance_for_idx(static_cast<int>(i));
        if (!d || uni_bt_conn_get_state(&d->conn) != UNI_BT_CONN_STATE_DEVICE_READY)
            continue;
        if (gap_get_connection_type(d->conn.handle) == GAP_CONNECTION_ACL)
            return;
    }
    gap_advertisements_enable(1);
#endif
}

/** CYW43: any active BLE gamepad link (HOGP or Triton Valve GATT) needs scans off. */
static bool device_is_ble_hogp(const uni_hid_device_t* d) {
    if (d == nullptr)
        return false;
    if (d->hids_cid != 0 && d->hids_cid != 0xffff)
        return true;
    /* Triton skips HIDS — still needs quiet radio while LE link is up. */
    if (uni_hid_parser_steam_triton_is_device(d) && d->conn.handle != UNI_BT_CONN_HANDLE_INVALID &&
        gap_get_connection_type(d->conn.handle) == GAP_CONNECTION_LE)
        return true;
    return false;
}

/** CYW43: periodic BR/EDR inquiry while a Classic ACL link is up can drop DS4/PS3 in ~1–2 s. */
static void maybe_restart_bredr_inquiry_after_disconnect(int disconnected_idx) {
    if (!remote_bt_allowed()) return;
#if defined(CONFIG_TARGET_PICO_W)
    if (!uni_bt_enable_new_connections_is_enabled())
        return;
    for (uint8_t i = 0; i < CONFIG_BLUEPAD32_MAX_DEVICES; ++i) {
        if (i == static_cast<unsigned>(disconnected_idx))
            continue;
        if (!bt_devices_[i].connected)
            continue;
        uni_hid_device_t* d = uni_hid_device_get_instance_for_idx(static_cast<int>(i));
        if (!d || uni_bt_conn_get_state(&d->conn) != UNI_BT_CONN_STATE_DEVICE_READY)
            continue;
        if (gap_get_connection_type(d->conn.handle) == GAP_CONNECTION_ACL)
            return;
        /* Any BLE HOGP pad: keep BR inquiry off (same radio contention as Xbox). */
        if (device_is_ble_hogp(d))
            return;
    }
    uni_bt_bredr_scan_start();
#endif
}

/**
 * CYW43: Xbox Series (BLE) connect stops LE scan (and BR inquiry) to avoid supervision timeout,
 * but leaves Bluepad32 "new connections" enabled. On disconnect, enable_new_connections(true) is a
 * no-op, so LE scan never resumes and the pad cannot reconnect without power-cycling the Pico.
 * Restart LE scan unless another pad still requires it off.
 */
static void maybe_restart_ble_scan_after_disconnect(int disconnected_idx) {
    if (!remote_bt_allowed()) return;
#if defined(CONFIG_TARGET_PICO_W)
    if (!uni_bt_enable_new_connections_is_enabled())
        return;
    for (uint8_t i = 0; i < CONFIG_BLUEPAD32_MAX_DEVICES; ++i) {
        if (i == static_cast<unsigned>(disconnected_idx))
            continue;
        if (!bt_devices_[i].connected)
            continue;
        uni_hid_device_t* d = uni_hid_device_get_instance_for_idx(static_cast<int>(i));
        if (!d || uni_bt_conn_get_state(&d->conn) != UNI_BT_CONN_STATE_DEVICE_READY)
            continue;
        if (device_is_ble_hogp(d))
            return;
        /* Solo Switch Joy-Con keeps LE scan off while waiting for partner over Classic BT. */
        if (uni_hid_parser_switch_solo_needs_partner(d))
            return;
    }
    uni_bt_le_scan_start();
#endif
}

/** Set in device_ready — only reboot after a fully working pad disconnects (not failed pair attempts). */
static bool s_bt_slot_was_ready[CONFIG_BLUEPAD32_MAX_DEVICES]{};

/** Full chip reset after last ready BT pad drops — next pair is always a clean first connect. */
static btstack_timer_source_t s_bt_disconnect_reboot_timer;
static bool s_bt_disconnect_reboot_pending = false;

static void bt_disconnect_reboot_cb(btstack_timer_source_t* ts) {
    (void)ts;
    s_bt_disconnect_reboot_pending = false;
    if (!remote_bt_allowed()) return;
    printf("[BP32] Last Xbox BLE controller disconnected — restarting Pico for clean reconnect\n");
    board_api::reboot();
}

/** Xbox Series / One S over BLE (HOGP) — in-place reconnect leaves HIDS/bond state bad. */
static bool device_is_xbox_ble(const uni_hid_device_t* d) {
    return d != nullptr && d->vendor_id == 0x045eu &&
           d->controller_type == CONTROLLER_TYPE_XBoxOneController &&
           d->hids_cid != 0 && d->hids_cid != 0xffff;
}

/**
 * Drop LE links that never reached DEVICE_READY (stuck DIS/HIDS after USB suspend).
 * Classic ACL pads are left alone so 8BitDo Android/Switch can reconnect cleanly.
 */
static void drop_incomplete_ble_slots(void) {
#if defined(CONFIG_TARGET_PICO_W)
    for (uint8_t i = 0; i < CONFIG_BLUEPAD32_MAX_DEVICES; ++i) {
        uni_hid_device_t* d = uni_hid_device_get_instance_for_idx(static_cast<int>(i));
        if (!d || d->conn.handle == UNI_BT_CONN_HANDLE_INVALID)
            continue;
        if (uni_bt_conn_get_state(&d->conn) == UNI_BT_CONN_STATE_DEVICE_READY)
            continue;
        if (gap_get_connection_type(d->conn.handle) != GAP_CONNECTION_LE)
            continue;
        printf("[BP32] Dropping incomplete BLE slot %u (stale after suspend)\n",
               static_cast<unsigned>(i));
        uni_hid_device_disconnect(d);
    }
#endif
}

static void restore_bt_pairing_mode(int disconnected_idx);

#if defined(CONFIG_EN_BLUETOOTH) && defined(CONFIG_TARGET_PICO_W)
/** True while a BLE pad is connected but not yet DEVICE_READY (pairing / DIS / HIDS). */
static bool any_ble_connect_in_progress() {
    for (uint8_t i = 0; i < CONFIG_BLUEPAD32_MAX_DEVICES; ++i) {
        uni_hid_device_t* d = uni_hid_device_get_instance_for_idx(static_cast<int>(i));
        if (!d || d->conn.handle == UNI_BT_CONN_HANDLE_INVALID)
            continue;
        if (uni_bt_conn_get_state(&d->conn) == UNI_BT_CONN_STATE_DEVICE_READY)
            continue;
        if (gap_get_connection_type(d->conn.handle) == GAP_CONNECTION_LE)
            return true;
    }
    return false;
}

static bool is_pairing_idle() {
    if (!remote_bt_allowed()) return false;
#if defined(CONFIG_EN_USB_HOST)
    if (board_api::usb::host_any_pad_mounted())
        return false;
#endif
    /* Mid HOGP discovery still looks "disconnected" to bt_devices_[], but scans
     * must stay off or Triton's HIDS discovery never finishes. */
    if (any_ble_connect_in_progress())
        return false;
    return !any_connected();
}

/** Restart BR/LE scans when idle; no-op if a pad or wired host controller is active. */
static void ensure_idle_pairing_scans(int disconnected_idx) {
    if (!is_pairing_idle())
        return;
    maybe_restart_bredr_inquiry_after_disconnect(disconnected_idx);
    maybe_restart_ble_scan_after_disconnect(disconnected_idx);
    ogxm_resume_ble_ads_if_no_acl_pad(disconnected_idx);
    if (!uni_bt_enable_new_connections_is_enabled())
        uni_bt_enable_new_connections_unsafe(true);
}

static btstack_timer_source_t s_pairing_watchdog_timer;

static void pairing_watchdog_cb(btstack_timer_source_t* ts) {
    ensure_idle_pairing_scans(-1);
    btstack_run_loop_set_timer(ts, PAIRING_WATCHDOG_MS);
    btstack_run_loop_add_timer(ts);
}

static btstack_context_callback_registration_t s_usb_resume_bt_reg;

static void usb_resume_on_bt_main(void* ctx) {
    (void)ctx;
    drop_incomplete_ble_slots();
    if (!is_pairing_idle())
        return;
    printf("[BP32] USB resume — restoring BT pairing scans\n");
    restore_bt_pairing_mode(-1);
}
#endif /* CONFIG_EN_BLUETOOTH && CONFIG_TARGET_PICO_W */

static void restore_bt_pairing_mode(int disconnected_idx) {
    if (!remote_bt_allowed()) return;
    if (led_timer_set_)
        btstack_run_loop_remove_timer(&led_timer_);
    led_timer_set_ = true;
    led_timer_.process = check_led_cb;
    led_timer_.context = nullptr;
    btstack_run_loop_set_timer(&led_timer_, LED_CHECK_TIME_MS);
    btstack_run_loop_add_timer(&led_timer_);
    board_api::set_led(false);

#if defined(CONFIG_EN_BLUETOOTH) && defined(CONFIG_TARGET_PICO_W)
    ensure_idle_pairing_scans(disconnected_idx);
#else
    maybe_restart_bredr_inquiry_after_disconnect(disconnected_idx);
    maybe_restart_ble_scan_after_disconnect(disconnected_idx);
    ogxm_resume_ble_ads_if_no_acl_pad(disconnected_idx);
    uni_bt_enable_new_connections_unsafe(true);
#endif
}

static void device_disconnected_cb(uni_hid_device_t* device) {
    int idx = uni_hid_device_get_idx_for_instance(device);
    if (idx >= CONFIG_BLUEPAD32_MAX_DEVICES || idx < 0) {
        return;
    }

    if (uni_hid_parser_switch2_is_ble_device(device)) {
        OGXM_LOG("SW2: disconnected slot %d\n", idx);
    }
    flydigi_apex4_bt_on_disconnected(device);
    gamesir_cyclone2_bt_on_disconnected(device);

    const bool was_ready = s_bt_slot_was_ready[idx];
    // Do not overwrite a specific pairing/authentication failure with a generic disconnect.
    if (!was_ready && s_diag_fail.load(std::memory_order_relaxed) == FAIL_NONE &&
        s_diag_stage.load(std::memory_order_relaxed) != DIAG_REJECTED)
        diag_failure(FAIL_EARLY_DISCONNECT, 0);
    diag_stage(DIAG_DISCONNECTED);
    if (was_ready) s_diag_disconnect_events.fetch_add(1, std::memory_order_relaxed);
    s_bt_slot_was_ready[idx] = false;
    s_bt_seen_hid_report[idx] = false;

    bt_devices_[idx].connected = false;
    bool any_other_connected = false;
    for (uint8_t i = 0; i < CONFIG_BLUEPAD32_MAX_DEVICES; ++i) {
        if (bt_devices_[i].connected) {
            any_other_connected = true;
            break;
        }
    }
    s_bt_any_connected_cached.store(any_other_connected, std::memory_order_release);
#if defined(CONFIG_OGXM_NETWORK)
    RemoteControl::note_bluetooth_connected(any_other_connected);
#endif
    s_last_bt_input_ms[idx] = 0;
    s_xbox_ble_ka_last_ms[idx] = 0;
    s_sw2_ble_ka_last_ms[idx] = 0;
    s_xbox_ble_last_recovery_get_ms[idx] = 0;
    /* Bluepad32 has two connection slots, but this build has one output pad.
     * Do not index output-pad state using a second Bluetooth slot. */
    if (idx < static_cast<int>(MAX_GAMEPADS)) {
        s_bt_disconnect_combo_grace_until_ms[idx] = 0;
        s_ps4_rumble_ok_ms[idx] = 0;
        prev_touchpad_clicked_[idx] = false;
        pending_adaptive_trigger_send_[idx] = false;
    }
    /* Never clear PadIn for an OGX slot owned by USB (BT idx often equals USB pad 0). */
    const int out_for_reset = bp32_get_gamepad_output_idx(device);
    const int pad_for_reset =
        (out_for_reset >= 0 && out_for_reset < static_cast<int>(MAX_GAMEPADS)) ? out_for_reset
        : (idx < static_cast<int>(MAX_GAMEPADS) ? idx : -1);
    if (pad_for_reset >= 0 && !InputSlot::usb_owns(static_cast<uint8_t>(pad_for_reset))) {
        if (pad_for_reset < CONFIG_BLUEPAD32_MAX_DEVICES &&
            bt_devices_[pad_for_reset].gamepad != nullptr) {
            bt_devices_[pad_for_reset].gamepad->reset_pad_in();
        }
    } else if (pad_for_reset >= 0) {
#if defined(CONFIG_OGXM_DEBUG)
        printf("[BT DISCONNECT] skip reset_pad_in: OGX slot %d owned by USB\n", pad_for_reset);
#endif
    }
    SteamPassthrough::clear();

    if (feedback_timer_set_ && !any_other_connected) {
        feedback_timer_set_ = false;
        btstack_run_loop_remove_timer(&feedback_timer_);
    }

    /* The Ultimate family can expose different HID personalities. Log the
     * actual detected identity and transport on disconnect, even if Wi-Fi
     * currently owns output; this avoids assuming an Xbox BLE classification. */
    const bool xbox_ble = device_is_xbox_ble(device);
    printf("[BP32] BT disconnect slot=%d name=%.24s type=%u vid=%04x pid=%04x hids=%u ready=%u xbox_ble=%u\n",
           idx, device->name, static_cast<unsigned>(device->controller_type),
           static_cast<unsigned>(device->vendor_id), static_cast<unsigned>(device->product_id),
           static_cast<unsigned>(device->hids_cid), static_cast<unsigned>(was_ready),
           static_cast<unsigned>(xbox_ble));

    if (any_other_connected || !remote_bt_allowed())
        return;

    /* Immediately show pairing mode (LED blink + BT scan). */
    restore_bt_pairing_mode(idx);

    /*
     * Xbox BLE needs a full reboot after a ready disconnect — bonded re-encryption /
     * leftover HIDS state otherwise leave the adapter frozen until unplug.
     * Classic BT and non-Xbox BLE (8BitDo Android/Switch, DualShock, Joy-Con, etc.)
     * reconnect in place; rebooting those on OG Xbox looks like a freeze.
     * Failed pair attempts must not reboot or we never stay in pairing mode.
     */
    if (was_ready && xbox_ble && !s_bt_disconnect_reboot_pending) {
        s_bt_disconnect_reboot_pending = true;
        s_bt_disconnect_reboot_timer.process = bt_disconnect_reboot_cb;
        s_bt_disconnect_reboot_timer.context = nullptr;
        btstack_run_loop_set_timer(&s_bt_disconnect_reboot_timer, 500);
        btstack_run_loop_add_timer(&s_bt_disconnect_reboot_timer);
        printf("[BP32] Pairing mode on — Xbox BLE reboot in 500 ms for clean reconnect\n");
    } else if (was_ready) {
        printf("[BP32] Pairing mode on — Classic/non-Xbox-BLE reconnect without reboot\n");
    }
}

static void ogxm_play_connection_rumble(uni_hid_device_t* device)
{
    if (device == nullptr || device->report_parser.play_dual_rumble == nullptr) {
        return;
    }
    const uint16_t start_delay =
        (device->controller_type == CONTROLLER_TYPE_PS4Controller)
            ? CONNECT_RUMBLE_DELAY_PS4_MS
            : CONNECT_RUMBLE_DELAY_DEFAULT_MS;
    device->report_parser.play_dual_rumble(
        device,
        start_delay,
        CONNECT_RUMBLE_DURATION_MS,
        CONNECT_RUMBLE_WEAK,
        CONNECT_RUMBLE_STRONG);
}

static uni_error_t device_ready_cb(uni_hid_device_t* device) {
    if (!remote_bt_allowed()) return UNI_ERROR_INVALID_CONTROLLER;
    /* DS4/DS5 BT create a second "virtual mouse" device on the same ACL. OGX-Mini only uses
     * gamepad input; accepting the virtual slot destabilized the link (disconnect ~2 s). */
    if (uni_hid_device_is_virtual_device(device))
        return UNI_ERROR_INVALID_CONTROLLER;

    int idx = uni_hid_device_get_idx_for_instance(device);
    if (idx < 0 || idx >= CONFIG_BLUEPAD32_MAX_DEVICES) {
        return UNI_ERROR_SUCCESS;
    }

    const int out_idx = bp32_get_gamepad_output_idx(device);
    const int pad_idx = resolve_bt_output_pad_idx(out_idx >= 0 ? out_idx : idx);
    if (pad_idx < 0) {
        diag_failure(FAIL_NO_OUTPUT, 0);
        diag_stage(DIAG_REJECTED);
#if defined(CONFIG_OGXM_DEBUG)
        printf("\n[BT READY REJECTED]\nbt_idx=%d preferred_out=%d — no free OGX slot "
               "(USB occupies all MAX_GAMEPADS=%u)\n",
               idx, out_idx, static_cast<unsigned>(MAX_GAMEPADS));
        log_ogx_slots("REJECT reason: USB ownership");
#endif
        return UNI_ERROR_NO_SLOTS;
    }
#if defined(CONFIG_OGXM_DEBUG)
    if (pad_idx != idx || pad_idx != out_idx) {
        printf("[BT READY] bt_idx=%d out_idx=%d -> OGX pad %d (avoid USB collision)\n",
               idx, out_idx, pad_idx);
    }
#endif

    if (gamesir_cyclone2_bt_is_game_pair_mode(device->name)) {
        gamesir_cyclone2_bt_on_ready(device);
        gamesir_cyclone2_bt_reject_game_pair_mode_if_needed(device);
        return UNI_ERROR_INVALID_CONTROLLER;
    }

    printf("[BP32] READY name=%.24s bt_slot=%d out=%d type=%u vid=%04x pid=%04x hids=%u\n",
           device->name, idx, pad_idx, static_cast<unsigned>(device->controller_type),
           static_cast<unsigned>(device->vendor_id), static_cast<unsigned>(device->product_id),
           static_cast<unsigned>(device->hids_cid));
    bt_devices_[idx].connected = true;
    s_bt_slot_was_ready[idx] = true;
    s_bt_seen_hid_report[idx] = false;
    s_xbox_ble_last_recovery_get_ms[idx] = 0;
    // Capture the *real* BT slot and transport, not the remapped Xbox pad slot.
    s_diag_ready_events.fetch_add(1, std::memory_order_relaxed);
    s_diag_ready_ms.store(diag_now(), std::memory_order_relaxed);
    diag_stage(DIAG_READY);
    s_diag_vid.store(static_cast<uint32_t>(device->vendor_id), std::memory_order_relaxed);
    s_diag_pid.store(static_cast<uint32_t>(device->product_id), std::memory_order_relaxed);
    s_diag_conn_handle.store(device->conn.handle, std::memory_order_relaxed);
    diag_set_name(device->name);
    s_diag_callbacks.store(0, std::memory_order_relaxed);
    s_diag_gamepad_reports.store(0, std::memory_order_relaxed);
    s_diag_last_report_ms.store(0, std::memory_order_relaxed);
    s_diag_bt_slot.store(idx, std::memory_order_relaxed);
    s_diag_output_slot.store(pad_idx, std::memory_order_relaxed);
    s_diag_type.store(static_cast<uint32_t>(device->controller_type), std::memory_order_relaxed);
    s_diag_hids_cid.store(static_cast<uint32_t>(device->hids_cid), std::memory_order_relaxed);
    s_diag_ble.store(gap_get_connection_type(device->conn.handle) == GAP_CONNECTION_LE,
                     std::memory_order_relaxed);
    flydigi_apex4_bt_on_ready(device);
    gamesir_cyclone2_bt_on_ready(device);
#if defined(CONFIG_OGXM_DEBUG)
    if (uni_hid_parser_switch2_is_ble_device(device)) {
        OGXM_LOG("SW2: READY slot %d pid=0x%04x out=%d — input active\n", idx, device->product_id, pad_idx);
    } else if (bp32_is_switch_joycon(device)) {
        OGXM_LOG("SW1: READY slot %d Joy-Con out=%d — input active\n", idx, pad_idx);
    }
#endif
    s_bt_any_connected_cached.store(true, std::memory_order_release);
#if defined(CONFIG_OGXM_NETWORK)
    RemoteControl::note_bluetooth_connected(true);
#endif
#if defined(CONFIG_TARGET_PICO_W)
    if (device_is_ble_hogp(device)) {
        /* CYW43439: BLE scan + BR inquiry during an active LE HOGP link stalls GATT / drops link. */
        uni_bt_le_scan_stop();
        uni_bt_bredr_scan_stop();
        gap_advertisements_enable(0);
    } else if (gap_get_connection_type(device->conn.handle) == GAP_CONNECTION_ACL) {
        gap_advertisements_enable(0);
        if (uni_hid_parser_switch_solo_needs_partner(device)) {
            /* Solo Joy-Con (L or R): keep inquiry + page scan for the partner; pause BLE scan. */
            uni_bt_le_scan_stop();
            gap_connectable_control(1);
            uni_bt_bredr_scan_start();
        } else if (!uni_hid_parser_switch_any_awaiting_partner()) {
            uni_bt_bredr_scan_stop();
        }
    }
#endif
    const uint32_t tnow = to_ms_since_boot(get_absolute_time());
    /* pad_idx already resolved above to avoid USB-owned OGX slots. */
    s_last_bt_input_ms[pad_idx] = tnow;
    s_bt_disconnect_combo_grace_until_ms[pad_idx] = tnow + 3500u;
    if (device->controller_type == CONTROLLER_TYPE_PS4Controller)
        s_ps4_rumble_ok_ms[pad_idx] = tnow + 6000u;
    /* Xbox BLE: 0 = send keepalive on next feedback tick (wakes Series/SW2 pad link immediately). */
    if (device->controller_type == CONTROLLER_TYPE_XBoxOneController && device->hids_cid != 0)
        s_xbox_ble_ka_last_ms[idx] = 0;
    if (uni_hid_parser_switch2_is_ble_device(device))
        s_sw2_ble_ka_last_ms[idx] = 0;

    // Set controller player LED to match slot (e.g. Wii U: LED 1 = player 1, LED 2 = player 2).
    // Joy-Con pairs set LEDs in the parser when merged; skip secondary to avoid player-2 LED.
    if (device->report_parser.set_player_leds != nullptr &&
        !bp32_is_joycon_pair_secondary(device)) {
        device->report_parser.set_player_leds(device, static_cast<uint8_t>(1u << pad_idx));
    }

    /* Wiimote: Bluepad32 defaults to sideways (horizontal) mapping; OGX uses TV-style vertical.
     * set_mode() overwrites controller_subtype — save extension info first. */
    if (ogxm_is_handheld_wiimote(device)) {
        const uint8_t saved_subtype = device->controller_subtype;
        const bool has_nunchuk =
            saved_subtype == CONTROLLER_SUBTYPE_WIIMOTE_NUNCHUK ||
            saved_subtype == CONTROLLER_SUBTYPE_WIIMOTE_NUNCHUK_ACCEL;

        uni_hid_parser_wii_set_mode(device, WII_MODE_VERTICAL);

        /* Motion output: request accel reports. With a nunchuk, DRM_KA (0x31) is ignored /
         * has no extension bytes — use DRM_KAE (0x35) so stick + accel both work.
         * Byte 0x04 = continuous reporting (wiibrew); without it the Wiimote only sends on
         * large changes, so Switch gyro-pointer feels dead except when shaking hard.
         * Keep mode VERTICAL (not WII_MODE_ACCEL) so button layouts stay correct. */
        if (MotionOutputActive::is_enabled()) {
            if (has_nunchuk) {
                device->controller_subtype = CONTROLLER_SUBTYPE_WIIMOTE_NUNCHUK_ACCEL;
                static constexpr uint8_t kWiiReqAccelNunchuk[] = {0xa2, 0x12, 0x04, 0x35};
                uni_hid_device_send_intr_report(device, kWiiReqAccelNunchuk, sizeof(kWiiReqAccelNunchuk));
            } else {
                device->controller_subtype = CONTROLLER_SUBTYPE_WIIMOTE_ACCEL;
                static constexpr uint8_t kWiiReqAccel[] = {0xa2, 0x12, 0x04, 0x31};
                uni_hid_device_send_intr_report(device, kWiiReqAccel, sizeof(kWiiReqAccel));
            }
        } else if (has_nunchuk) {
            device->controller_subtype = CONTROLLER_SUBTYPE_WIIMOTE_NUNCHUK;
        }
    }

    // PS5: start with adaptive triggers off; touchpad click toggles them.
    if (device->controller_type == CONTROLLER_TYPE_PS5Controller) {
        adaptive_trigger_enabled_[pad_idx] = false;
        ds5_adaptive_trigger_effect_t off = ds5_new_adaptive_trigger_effect_off();
        ds5_set_adaptive_trigger_effect(device, UNI_ADAPTIVE_TRIGGER_TYPE_LEFT, &off);
        ds5_set_adaptive_trigger_effect(device, UNI_ADAPTIVE_TRIGGER_TYPE_RIGHT, &off);
    }

    if (led_timer_set_) {
        led_timer_set_ = false;
        btstack_run_loop_remove_timer(&led_timer_);
        board_api::set_led(true);
    }
    if (!feedback_timer_set_) {
        feedback_timer_set_ = true;
        feedback_timer_.process = send_feedback_cb;
        feedback_timer_.context = nullptr;
        btstack_run_loop_set_timer(&feedback_timer_, FEEDBACK_TIME_MS);
        btstack_run_loop_add_timer(&feedback_timer_);
    }

    ogxm_play_connection_rumble(device);

    return UNI_ERROR_SUCCESS;
}

static void oob_event_cb(uni_platform_oob_event_t event, void* data) {
	return;
}

// Set to 1 to print all Bluepad32 controller inputs to UART (only when state changes)
#ifndef BLUEPAD32_UART_LOG_INPUT
#if defined(CONFIG_OGXM_DEBUG)
#define BLUEPAD32_UART_LOG_INPUT 1
#else
#define BLUEPAD32_UART_LOG_INPUT 0
#endif
#endif

static void controller_data_cb(uni_hid_device_t* device, uni_controller_t* controller) {
    s_diag_callbacks.fetch_add(1, std::memory_order_relaxed);
    if (!remote_bt_allowed()) return;
#if defined(CONFIG_OGXM_NETWORK)
    // The radio acknowledges between callbacks. Preserve the last published
    // state while Core0 commits the shared profile; USB service keeps running.
    if (RemoteControl::profile_apply_pending()) return;
#endif
    static uni_gamepad_t prev_uni_gp[MAX_GAMEPADS] = {};

    if (controller->klass != UNI_CONTROLLER_CLASS_GAMEPAD){
        return;
    }
    const uint32_t previous_reports = s_diag_gamepad_reports.fetch_add(1, std::memory_order_relaxed);
    if (previous_reports == 0) diag_stage(DIAG_REPORTS);
    s_diag_last_report_ms.store(to_ms_since_boot(get_absolute_time()), std::memory_order_relaxed);

    uni_gamepad_t *uni_gp = &controller->gamepad;
    const int bt_slot = uni_hid_device_get_idx_for_instance(device);
    if (bt_slot >= 0 && bt_slot < CONFIG_BLUEPAD32_MAX_DEVICES &&
        !s_bt_seen_hid_report[bt_slot]) {
        s_bt_seen_hid_report[bt_slot] = true;
        printf("[BP32] FIRST GAMEPAD REPORT bt_slot=%d buttons=%04x dpad=%02x\n",
               bt_slot, static_cast<unsigned>(controller->gamepad.buttons),
               static_cast<unsigned>(controller->gamepad.dpad));
    }
    int idx = bp32_get_gamepad_output_idx(device);
    if (idx < 0)
        idx = bt_slot;
    idx = resolve_bt_output_pad_idx(idx);
    if (idx < 0)
        return;
    {
        const uint32_t now_cb = to_ms_since_boot(get_absolute_time());
        s_last_bt_input_ms[static_cast<unsigned>(idx)] = now_cb;
        if (bt_slot >= 0 && bt_slot < CONFIG_BLUEPAD32_MAX_DEVICES && bt_slot != idx)
            s_last_bt_input_ms[static_cast<unsigned>(bt_slot)] = now_cb;
    }

    /* USB-owned OGX pad must not receive Bluetooth PadIn (MAX_GAMEPADS=1 collision). */
    if (InputSlot::usb_owns(static_cast<uint8_t>(idx))) {
        return;
    }

#if defined(CONFIG_OGXM_DEBUG)
    if (flydigi_apex4_bt_parser_installed(device)) {
        static uint32_t s_apex_plat_log_ms[MAX_GAMEPADS]{};
        const uint32_t now = to_ms_since_boot(get_absolute_time());
        if (idx >= 0 && idx < static_cast<int>(MAX_GAMEPADS) &&
            (now - s_apex_plat_log_ms[idx] >= 500u ||
             (uni_gp->buttons | uni_gp->misc_buttons | uni_gp->dpad) != 0)) {
            if (now - s_apex_plat_log_ms[idx] >= 250u) {
                s_apex_plat_log_ms[idx] = now;
                OGXM_LOG("[3] BLUEPAD PLATFORM CALLBACK idx=%d buttons=0x%04x misc=0x%02x "
                         "Lx=%ld Ly=%ld Rx=%ld Ry=%ld\n",
                         idx, uni_gp->buttons, uni_gp->misc_buttons,
                         static_cast<long>(uni_gp->axis_x), static_cast<long>(uni_gp->axis_y),
                         static_cast<long>(uni_gp->axis_rx), static_cast<long>(uni_gp->axis_ry));
            }
        }
    }
#endif

#if BLUEPAD32_UART_LOG_INPUT
    {
        bool changed = std::memcmp(uni_gp, &prev_uni_gp[idx], sizeof(uni_gamepad_t)) != 0;
        if (changed && device->controller_type != CONTROLLER_TYPE_Switch2ProController) {
            printf("[BP32 idx=%d] dpad=0x%02x btns=0x%04x misc=0x%02x brake=%u throttle=%u "
                   "Lx=%d Ly=%d Rx=%d Ry=%d\n",
                   idx, (unsigned)uni_gp->dpad, (unsigned)uni_gp->buttons, (unsigned)uni_gp->misc_buttons,
                   (unsigned)uni_gp->brake, (unsigned)uni_gp->throttle,
                   (int)uni_gp->axis_x, (int)uni_gp->axis_y, (int)uni_gp->axis_rx, (int)uni_gp->axis_ry);
        }
    }
#endif

    Gamepad* gamepad = bt_devices_[idx].gamepad;
    Gamepad::PadIn gp_in;

    switch (uni_gp->dpad) 
    {
        case DPAD_UP:
            gp_in.dpad = gamepad->MAP_DPAD_UP;
            break;
        case DPAD_DOWN:
            gp_in.dpad = gamepad->MAP_DPAD_DOWN;
            break;
        case DPAD_LEFT:
            gp_in.dpad = gamepad->MAP_DPAD_LEFT;
            break;
        case DPAD_RIGHT:
            gp_in.dpad = gamepad->MAP_DPAD_RIGHT;
            break;
        case DPAD_UP | DPAD_RIGHT:
            gp_in.dpad = gamepad->MAP_DPAD_UP_RIGHT;
            break;
        case DPAD_DOWN | DPAD_RIGHT:
            gp_in.dpad = gamepad->MAP_DPAD_DOWN_RIGHT;
            break;
        case DPAD_DOWN | DPAD_LEFT:
            gp_in.dpad = gamepad->MAP_DPAD_DOWN_LEFT;
            break;
        case DPAD_UP | DPAD_LEFT:
            gp_in.dpad = gamepad->MAP_DPAD_UP_LEFT;
            break;
        default:
            break;
    }

    if (is_wii_controller_connected(idx)) {
        if (uni_gp->buttons & BUTTON_A) gp_in.buttons |= gamepad->MAP_BUTTON_A;
        if (uni_gp->buttons & BUTTON_B) gp_in.buttons |= gamepad->MAP_BUTTON_B;
        if (uni_gp->buttons & BUTTON_X) gp_in.buttons |= gamepad->MAP_BUTTON_X;
        if (uni_gp->buttons & BUTTON_Y) gp_in.buttons |= gamepad->MAP_BUTTON_Y;
        if (uni_gp->buttons & BUTTON_SHOULDER_L) gp_in.buttons |= gamepad->MAP_BUTTON_LB;
        if (uni_gp->buttons & BUTTON_SHOULDER_R) gp_in.buttons |= gamepad->MAP_BUTTON_RB;
        //if (uni_gp->buttons & BUTTON_THUMB_L)    gp_in.buttons |= gamepad->MAP_BUTTON_L3;  
        //if (uni_gp->buttons & BUTTON_THUMB_R)    gp_in.buttons |= gamepad->MAP_BUTTON_R3;
        if (uni_gp->misc_buttons & MISC_BUTTON_BACK)    gp_in.buttons |= gamepad->MAP_BUTTON_BACK;
        if (uni_gp->misc_buttons & MISC_BUTTON_START)   gp_in.buttons |= gamepad->MAP_BUTTON_START;
        if (uni_gp->misc_buttons & MISC_BUTTON_SYSTEM)  gp_in.buttons |= gamepad->MAP_BUTTON_SYS;
    }
    else {
        if (uni_gp->buttons & BUTTON_A) gp_in.buttons |= gamepad->MAP_BUTTON_A;
        if (uni_gp->buttons & BUTTON_B) gp_in.buttons |= gamepad->MAP_BUTTON_B;
        if (uni_gp->buttons & BUTTON_X) gp_in.buttons |= gamepad->MAP_BUTTON_X;
        if (uni_gp->buttons & BUTTON_Y) gp_in.buttons |= gamepad->MAP_BUTTON_Y;
        if (uni_gp->buttons & BUTTON_SHOULDER_L) gp_in.buttons |= gamepad->MAP_BUTTON_LB;
        if (uni_gp->buttons & BUTTON_SHOULDER_R) gp_in.buttons |= gamepad->MAP_BUTTON_RB;
        if (uni_gp->buttons & BUTTON_THUMB_L)    gp_in.buttons |= gamepad->MAP_BUTTON_L3;  
        if (uni_gp->buttons & BUTTON_THUMB_R)    gp_in.buttons |= gamepad->MAP_BUTTON_R3;
        if (uni_gp->misc_buttons & MISC_BUTTON_BACK)    gp_in.buttons |= gamepad->MAP_BUTTON_BACK;
        if (uni_gp->misc_buttons & MISC_BUTTON_START)   gp_in.buttons |= gamepad->MAP_BUTTON_START;
        if (uni_gp->misc_buttons & MISC_BUTTON_SYSTEM)  gp_in.buttons |= gamepad->MAP_BUTTON_SYS; 
    }

    if (uni_gp->misc_buttons & MISC_BUTTON_CAPTURE) gp_in.buttons |= gamepad->MAP_BUTTON_MISC;

    // Check for disconnect combo: Start+Select for most controllers, L3+R3 for OUYA (no Start/Select)
    static uint32_t disconnect_combo_hold_time[MAX_GAMEPADS] = {0};
    const uint32_t now_cb = to_ms_since_boot(get_absolute_time());
    const bool combo_grace =
        (idx >= 0 && idx < MAX_GAMEPADS && now_cb < s_bt_disconnect_combo_grace_until_ms[idx]);
    bool is_ouya = (device->controller_type == CONTROLLER_TYPE_OUYAController);
    bool combo_pressed = is_ouya
        ? ((uni_gp->buttons & BUTTON_THUMB_L) && (uni_gp->buttons & BUTTON_THUMB_R))
        : ((uni_gp->misc_buttons & MISC_BUTTON_START) && (uni_gp->misc_buttons & MISC_BUTTON_BACK));

#if defined(CONFIG_OGXM_NETWORK)
    if (!RemoteControl::physical_output_enabled()) combo_pressed = false;
#endif
    if (combo_grace) {
        disconnect_combo_hold_time[idx] = 0;
    } else if (combo_pressed) {
        disconnect_combo_hold_time[idx]++;
        // Require combo to be held for ~500ms (assuming ~60Hz callback rate, ~30 frames)
        if (disconnect_combo_hold_time[idx] >= 30) {
            printf("[BP32] Disconnect combo detected, disconnecting controller %d\n", idx);
            bp32_disconnect_controller_and_joycon_partner(device);
            disconnect_combo_hold_time[idx] = 0;
            return; // Don't process further input after disconnect
        }
    } else {
        disconnect_combo_hold_time[idx] = 0;
    }

    // Normalize Bluepad's physical report before ANY user profile transform.
    // Capture these values for calibration; calibrating processed output would
    // bake deadzones/curves back into the controller's measured range.
    const bool wii_nunchuk =
        device->controller_type == CONTROLLER_TYPE_WiiController &&
        (device->controller_subtype == CONTROLLER_SUBTYPE_WIIMOTE_NUNCHUK ||
         device->controller_subtype == CONTROLLER_SUBTYPE_WIIMOTE_NUNCHUK_ACCEL);
    ControllerSample raw;
    raw.axis[0] = Range::scale_from_bits<int16_t, 10>(wii_nunchuk ? uni_gp->axis_rx : uni_gp->axis_x);
    raw.axis[1] = Range::invert(Range::scale_from_bits<int16_t, 10>(wii_nunchuk ? uni_gp->axis_ry : uni_gp->axis_y));
    if (!wii_nunchuk) {
        raw.axis[2] = Range::scale_from_bits<int16_t, 10>(uni_gp->axis_rx);
        raw.axis[3] = Range::invert(Range::scale_from_bits<int16_t, 10>(uni_gp->axis_ry));
    }
    const bool wii_shoulder = is_wii_controller_connected(idx) &&
        ((uni_gp->buttons & BUTTON_SHOULDER_L) || (uni_gp->buttons & BUTTON_SHOULDER_R));
    if (!wii_shoulder) {
        raw.trigger[0] = Range::scale_from_bits<uint8_t, 10>(static_cast<uint16_t>(uni_gp->brake));
        raw.trigger[1] = Range::scale_from_bits<uint8_t, 10>(static_cast<uint16_t>(uni_gp->throttle));
        if (!raw.trigger[0] && (uni_gp->buttons & BUTTON_TRIGGER_L)) raw.trigger[0] = 255;
        if (!raw.trigger[1] && (uni_gp->buttons & BUTTON_TRIGGER_R)) raw.trigger[1] = 255;
    }
    // Raw button identities remain independent of the active remapping.
    if (uni_gp->buttons & BUTTON_A) raw.buttons |= Gamepad::BUTTON_A;
    if (uni_gp->buttons & BUTTON_B) raw.buttons |= Gamepad::BUTTON_B;
    if (uni_gp->buttons & BUTTON_X) raw.buttons |= Gamepad::BUTTON_X;
    if (uni_gp->buttons & BUTTON_Y) raw.buttons |= Gamepad::BUTTON_Y;
    if (uni_gp->buttons & BUTTON_SHOULDER_L) raw.buttons |= Gamepad::BUTTON_LB;
    if (uni_gp->buttons & BUTTON_SHOULDER_R) raw.buttons |= Gamepad::BUTTON_RB;
    if (uni_gp->buttons & BUTTON_THUMB_L) raw.buttons |= Gamepad::BUTTON_L3;
    if (uni_gp->buttons & BUTTON_THUMB_R) raw.buttons |= Gamepad::BUTTON_R3;
    if (uni_gp->misc_buttons & MISC_BUTTON_BACK) raw.buttons |= Gamepad::BUTTON_BACK;
    if (uni_gp->misc_buttons & MISC_BUTTON_START) raw.buttons |= Gamepad::BUTTON_START;
    if (uni_gp->misc_buttons & MISC_BUTTON_SYSTEM) raw.buttons |= Gamepad::BUTTON_SYS;
    if (uni_gp->misc_buttons & MISC_BUTTON_CAPTURE) raw.buttons |= Gamepad::BUTTON_MISC;
    if (uni_gp->dpad & DPAD_UP) raw.dpad |= Gamepad::DPAD_UP;
    if (uni_gp->dpad & DPAD_DOWN) raw.dpad |= Gamepad::DPAD_DOWN;
    if (uni_gp->dpad & DPAD_LEFT) raw.dpad |= Gamepad::DPAD_LEFT;
    if (uni_gp->dpad & DPAD_RIGHT) raw.dpad |= Gamepad::DPAD_RIGHT;
#if defined(CONFIG_OGXM_NETWORK)
    if (idx == 0) RemoteControl::on_bluetooth_input(raw, now_cb);
#endif
    gamepad->calibrate_bluetooth(raw);
    const auto left = gamepad->scale_joystick_l(raw.axis[0], Range::invert(raw.axis[1]));
    const auto right = gamepad->scale_joystick_r(raw.axis[2], Range::invert(raw.axis[3]));
    // PadIn is packed: assigning members avoids misaligned std::tie references.
    gp_in.joystick_lx = left.first;
    gp_in.joystick_ly = left.second;
    gp_in.joystick_rx = right.first;
    gp_in.joystick_ry = right.second;
    gp_in.trigger_l = gamepad->scale_trigger_l(raw.trigger[0]);
    gp_in.trigger_r = gamepad->scale_trigger_r(raw.trigger[1]);
    if ((gp_in.dpad & 3u) == 3u) gp_in.dpad &= ~3u;
    if ((gp_in.dpad & 12u) == 12u) gp_in.dpad &= ~12u;

    gp_in.motion_source = Gamepad::PadIn::MOTION_SRC_NONE;
    switch (device->controller_type) {
        case CONTROLLER_TYPE_PS4Controller:
            gp_in.motion_source = Gamepad::PadIn::MOTION_SRC_DS4;
            break;
        case CONTROLLER_TYPE_PS5Controller:
            gp_in.motion_source = Gamepad::PadIn::MOTION_SRC_DS5;
            break;
        case CONTROLLER_TYPE_SwitchProController:
        case CONTROLLER_TYPE_Switch2ProController:
            gp_in.motion_source = Gamepad::PadIn::MOTION_SRC_SWITCH_PRO;
            break;
        case CONTROLLER_TYPE_WiiController:
            if (device->controller_subtype == CONTROLLER_SUBTYPE_WIIMOTE_ACCEL ||
                device->controller_subtype == CONTROLLER_SUBTYPE_WIIMOTE_NUNCHUK_ACCEL) {
                gp_in.motion_source = Gamepad::PadIn::MOTION_SRC_WII_BT;
            } else {
                const int32_t l1 = (uni_gp->accel[0] >= 0 ? uni_gp->accel[0] : -uni_gp->accel[0]) +
                                   (uni_gp->accel[1] >= 0 ? uni_gp->accel[1] : -uni_gp->accel[1]) +
                                   (uni_gp->accel[2] >= 0 ? uni_gp->accel[2] : -uni_gp->accel[2]);
                if (l1 > 16) {
                    gp_in.motion_source = Gamepad::PadIn::MOTION_SRC_WII_BT;
                }
            }
            break;
        default:
            break;
    }
    if (gp_in.motion_source == Gamepad::PadIn::MOTION_SRC_WII_BT) {
        MotionImu::fill_from_wii_bt(gp_in.accel, gp_in.gyro, uni_gp->accel);
        /* Switch cursor/aim uses gyro; synthesize rate from accel (no MotionPlus parse yet). */
        if (idx >= 0 && idx < static_cast<int>(MAX_GAMEPADS)) {
            static MotionImu::WiiPseudoGyroState s_wii_gyro[MAX_GAMEPADS]{};
            MotionImu::apply_wii_pseudo_gyro(gp_in.accel, gp_in.gyro, s_wii_gyro[idx],
                                             to_ms_since_boot(get_absolute_time()));
        }
    } else if (gp_in.motion_source != Gamepad::PadIn::MOTION_SRC_NONE) {
        for (int i = 0; i < 3; i++) {
            gp_in.accel[i] = uni_gp->accel[i];
            gp_in.gyro[i] = uni_gp->gyro[i];
        }
    }

    if (SteamActive::is_enabled()) {
        SteamPassthrough::input_has_touchpad =
            (device->controller_type == CONTROLLER_TYPE_PS5Controller);
        SteamBtReport::update_from_uni_gamepad(uni_gp);
        if (device->controller_type == CONTROLLER_TYPE_PS5Controller) {
            uint8_t touch_points[8]{};
            bool touchpad_click = false;
            uni_hid_parser_ds5_get_touchpad(device, touch_points, &touchpad_click);
            SteamTouchpad::apply_to_passthrough(touch_points, touchpad_click);
            std::memcpy(gp_in.touch_raw, touch_points, sizeof(gp_in.touch_raw));
            gp_in.touchpad_click = touchpad_click ? 1 : 0;
            gp_in.touchpad_valid = 1;
        }
    }

    gamepad->set_pad_in_from_bluetooth(gp_in);

#if defined(CONFIG_OGXM_DEBUG)
    if (flydigi_apex4_bt_parser_installed(device)) {
        static uint32_t s_apex_ogx_log_ms[MAX_GAMEPADS]{};
        const uint32_t now = to_ms_since_boot(get_absolute_time());
        if (idx >= 0 && idx < static_cast<int>(MAX_GAMEPADS) && now - s_apex_ogx_log_ms[idx] >= 500u) {
            s_apex_ogx_log_ms[idx] = now;
            OGXM_LOG("[4] OGX INPUT idx=%d buttons=0x%04x dpad=0x%02x lx=%d ly=%d rx=%d ry=%d\n",
                     idx, gp_in.buttons, gp_in.dpad,
                     static_cast<int>(gp_in.joystick_lx), static_cast<int>(gp_in.joystick_ly),
                     static_cast<int>(gp_in.joystick_rx), static_cast<int>(gp_in.joystick_ry));
        }
    }
#endif

#if BLUEPAD32_UART_LOG_INPUT
    if (idx >= 0 && idx < static_cast<int>(MAX_GAMEPADS) &&
        device->controller_type != CONTROLLER_TYPE_Switch2ProController) {
        std::memcpy(&prev_uni_gp[idx], uni_gp, sizeof(uni_gamepad_t));
    }
#endif

    // PS5: defer adaptive trigger send to main loop so callback never does l2cap_send (reduces input lag)
    if (device->controller_type == CONTROLLER_TYPE_PS5Controller && idx >= 0 && idx < static_cast<int>(MAX_GAMEPADS)) {
        bool touchpad_clicked = (uni_gp->misc_buttons & MISC_BUTTON_CAPTURE) != 0;
        if (touchpad_clicked && !prev_touchpad_clicked_[idx]) {
            adaptive_trigger_enabled_[idx] = !adaptive_trigger_enabled_[idx];
            pending_adaptive_trigger_send_[idx] = true;
        }
        prev_touchpad_clicked_[idx] = touchpad_clicked;
    }
}

const uni_property_t* get_property_cb(uni_property_idx_t idx) 
{
    return nullptr;
}

void process_pending_adaptive_triggers()
{
    for (uint8_t i = 0; i < MAX_GAMEPADS; ++i) {
        if (!pending_adaptive_trigger_send_[i])
            continue;
        pending_adaptive_trigger_send_[i] = false;
        uni_hid_device_t* device = uni_hid_device_get_instance_for_idx(static_cast<int>(i));
        if (!device || device->controller_type != CONTROLLER_TYPE_PS5Controller)
            continue;
        if (adaptive_trigger_enabled_[i]) {
            ds5_adaptive_trigger_effect_t on = ds5_new_adaptive_trigger_effect_feedback(5, 4);
            ds5_set_adaptive_trigger_effect(device, UNI_ADAPTIVE_TRIGGER_TYPE_LEFT, &on);
            ds5_set_adaptive_trigger_effect(device, UNI_ADAPTIVE_TRIGGER_TYPE_RIGHT, &on);
        } else {
            ds5_adaptive_trigger_effect_t off = ds5_new_adaptive_trigger_effect_off();
            ds5_set_adaptive_trigger_effect(device, UNI_ADAPTIVE_TRIGGER_TYPE_LEFT, &off);
            ds5_set_adaptive_trigger_effect(device, UNI_ADAPTIVE_TRIGGER_TYPE_RIGHT, &off);
        }
    }
}

uni_platform* get_driver() 
{
    static uni_platform driver = 
    {
        .name = "OGXMiniW",
        .init = init,
        .on_init_complete = init_complete_cb,
        .on_device_discovered = device_discovered_cb,
        .on_device_connected = device_connected_cb,
        .on_device_disconnected = device_disconnected_cb,
        .on_device_ready = device_ready_cb,
        .on_controller_data = controller_data_cb,
        .get_property = get_property_cb,
        .on_oob_event = oob_event_cb,
    };
    return &driver;
}

//Public API

void set_gpio_device_process_callback(void (*callback)(void* ctx), void* ctx) {
    gpio_process_cb_ = callback;
    gpio_process_ctx_ = ctx;
}

void set_pico_w_pio_usb_mux_tick(void (*tick_cb)(void)) {
    s_pico_w_pio_usb_mux_tick = tick_cb;
}

void set_remote_wifi_mode(bool active) {
    if (s_remote_wifi_mode == active) return;
    s_remote_wifi_mode = active;
    if (active) {
        // These operations run between HID callbacks, never on USB Core0.
        // Reject incoming bonded reconnects as well as stopping discovery.
        uni_bt_allow_incoming_connections(false);
        uni_bt_enable_new_connections_unsafe(false);
        uni_bt_le_scan_stop();
        uni_bt_bredr_scan_stop();
        gap_connectable_control(0);
        gap_advertisements_enable(0);
        if (s_bt_disconnect_reboot_pending) {
            btstack_run_loop_remove_timer(&s_bt_disconnect_reboot_timer);
            s_bt_disconnect_reboot_pending = false;
        }
        for (unsigned i = 0; i < CONFIG_BLUEPAD32_MAX_DEVICES; ++i) {
            auto* device = uni_hid_device_get_instance_for_idx(static_cast<int>(i));
            if (device && device->conn.handle != UNI_BT_CONN_HANDLE_INVALID) {
                // This deliberate handoff must not schedule the legacy Xbox
                // BLE recovery reboot, even if the user re-enables Bluetooth
                // before the asynchronous disconnection event arrives.
                s_bt_slot_was_ready[i] = false;
                uni_hid_device_disconnect(device);
            }
        }
    } else {
        uni_bt_allow_incoming_connections(true);
        gap_connectable_control(1);
        restore_bt_pairing_mode(-1);
    }
}


void wired_usb_takeover_disconnect_bt() {
#if defined(CONFIG_TARGET_PICO_W) && defined(CONFIG_EN_USB_HOST)
    /* Core0 mux uses this atomic; disconnect callbacks run async on Core1. Clear immediately so
     * we do not treat BT as active and tuh_deinit() wired USB during the disconnect window. */
    s_bt_any_connected_cached.store(false, std::memory_order_release);
#endif
    s_bt_quiet_for_usb_pending.store(true, std::memory_order_release);
    if (!s_btstack_run_loop_ready.load(std::memory_order_acquire)) {
        /* Pad plugged during CYW43/uni_init — defer until run loop exists (avoids btstack_assert). */
        return;
    }
    for (uint8_t i = 0; i < CONFIG_BLUEPAD32_MAX_DEVICES; ++i) {
        uni_bt_disconnect_device_safe(i);
    }
    uni_bt_enable_new_connections_safe(false);
}

void wired_usb_release_enable_bt_pairing() {
    s_bt_quiet_for_usb_pending.store(false, std::memory_order_release);
    if (!s_btstack_run_loop_ready.load(std::memory_order_acquire)) {
        return;
    }
#if defined(CONFIG_EN_BLUETOOTH) && defined(CONFIG_TARGET_PICO_W)
    s_usb_resume_bt_reg.callback = usb_resume_on_bt_main;
    s_usb_resume_bt_reg.context = nullptr;
    btstack_run_loop_execute_on_main_thread(&s_usb_resume_bt_reg);
#else
    uni_bt_enable_new_connections_safe(true);
#endif
}

void on_usb_device_resume() {
    if (!s_btstack_run_loop_ready.load(std::memory_order_acquire)) {
        return;
    }
#if defined(CONFIG_EN_BLUETOOTH) && defined(CONFIG_TARGET_PICO_W)
    s_usb_resume_bt_reg.callback = usb_resume_on_bt_main;
    s_usb_resume_bt_reg.context = nullptr;
    btstack_run_loop_execute_on_main_thread(&s_usb_resume_bt_reg);
#endif
}

void init(Gamepad(&gamepads)[MAX_GAMEPADS])
{
    for (uint8_t i = 0; i < MAX_GAMEPADS; ++i)
    {
        bt_devices_[i].gamepad = &gamepads[i];
    }

    uni_platform_set_custom(get_driver());
    uni_init(0, nullptr);
    /* Observe GATT notifications, including those HIDS cannot map to a report.
     * No writes, no new timers, and no changes to the existing listeners. */
    gatt_client_listen_for_characteristic_value_updates(
        &s_raw_gatt_listener, diag_raw_gatt_event,
        GATT_CLIENT_ANY_CONNECTION, nullptr);
    s_diag_hci_listener.callback = diag_hci_event;
    hci_add_event_handler(&s_diag_hci_listener);
    s_diag_sm_listener.callback = diag_sm_event;
    sm_add_event_handler(&s_diag_sm_listener);

    led_timer_set_ = true;
    led_timer_.process = check_led_cb;
    led_timer_.context = nullptr;
    btstack_run_loop_set_timer(&led_timer_, LED_CHECK_TIME_MS);
    btstack_run_loop_add_timer(&led_timer_);

    if (gpio_process_cb_ != nullptr) {
        gpio_process_timer_.process = gpio_process_timer_cb;
        gpio_process_timer_.context = nullptr;
        btstack_run_loop_set_timer(&gpio_process_timer_, GPIO_PROCESS_INTERVAL_MS);
        btstack_run_loop_add_timer(&gpio_process_timer_);
    }

    if (s_pico_w_pio_usb_mux_tick != nullptr) {
        s_pico_w_usb_mux_timer_.process = pico_w_usb_mux_timer_cb;
        s_pico_w_usb_mux_timer_.context = nullptr;
        btstack_run_loop_set_timer(&s_pico_w_usb_mux_timer_, 1);
        btstack_run_loop_add_timer(&s_pico_w_usb_mux_timer_);
    }

#if defined(CONFIG_EN_BLUETOOTH) && defined(CONFIG_TARGET_PICO_W)
    s_pairing_watchdog_timer.process = pairing_watchdog_cb;
    s_pairing_watchdog_timer.context = nullptr;
    btstack_run_loop_set_timer(&s_pairing_watchdog_timer, PAIRING_WATCHDOG_MS);
    btstack_run_loop_add_timer(&s_pairing_watchdog_timer);
#endif
}

void run_task(Gamepad(&gamepads)[MAX_GAMEPADS])
{
    init(gamepads);
#if defined(CONFIG_OGXM_NETWORK)
    if (NetworkService::ready()) NetworkService::start_radio_poll();
#endif
    /* uni_init has installed the run loop — safe for Core0 USB mux BT calls. */
    s_btstack_run_loop_ready.store(true, std::memory_order_release);
    if (s_bt_quiet_for_usb_pending.load(std::memory_order_acquire)) {
        for (uint8_t i = 0; i < CONFIG_BLUEPAD32_MAX_DEVICES; ++i) {
            uni_bt_disconnect_device_safe(i);
        }
        uni_bt_enable_new_connections_safe(false);
        OGXM_LOG("BT: applied deferred USB quiet (pad was plugged during BT bring-up)\n");
    }
    // Native 2026 BTstack run loop. SDK 2.3.0 already includes the timer fixes
    // used by develop; NetworkService remains on its serialized radio hook.
    btstack_run_loop_execute();
}

} // namespace bluepad32
