#pragma once

#include <cstdint>
#include <array>

#include "Gamepad/Gamepad.h"
#include "Board/Config.h"

/*  NOTE: Everything bluepad32/uni needs to be wrapped
    and kept away from tinyusb due to naming conflicts */

namespace bluepad32 {
    /** Controller reception diagnostics; atomics are sampled without blocking the radio. */
    struct HidFlowStats {
        uint32_t ready_events, disconnect_events, callbacks, gamepad_reports;
        uint32_t last_gamepad_age_ms; // UINT32_MAX = no report since READY
        int32_t bt_slot, output_slot;
        uint32_t controller_type, hids_cid;
        bool bluetooth_le;
        // Last observed connection, read-only across Core0/Core1.
        char name[25];
        uint32_t vendor_id, product_id;
        uint32_t stage, stage_age_ms, ready_age_ms;
        uint32_t failure_kind, failure_code, failure_age_ms;
        uint32_t pairing_state, pairing_status, pairing_reason, disconnect_reason;
        uint32_t hids_setup_status, cccd_attempts, cccd_acks, cccd_ok, cccd_errors;
        uint32_t cccd_request_status, cccd_last_status;
        uint32_t notifications, get_results, last_origin, last_report_id;
        uint32_t raw_gatt_notifications, raw_gatt_value_handle, raw_gatt_age_ms;
        uint32_t notification_age_ms, get_result_age_ms;
        uint32_t exit_suspend_calls, exit_suspend_status;
        uint32_t get_requests, get_rejected, get_request_status;
    };
    HidFlowStats hid_flow_stats(uint32_t now_ms);
    /** True if any Bluetooth gamepad is connected (Classic or LE). */
    bool any_connected();
    /** Serialized Core1 radio context only. Wi-Fi source disables scans,
     * incoming connections, reconnects and all physical input callbacks. */
    void set_remote_wifi_mode(bool active);
    void run_task(Gamepad(&gamepads)[MAX_GAMEPADS]);
    void init(Gamepad(&gamepads)[MAX_GAMEPADS]);
    /**
     * Pico W / Pico 2 W only: register a 1 ms callback from the Bluetooth run loop for
     * PIO USB host + BT mutual exclusion. Call once before run_task(); pass nullptr to disable.
     */
    void set_pico_w_pio_usb_mux_tick(void (*tick_cb)(void));
    /** Disconnect all BT gamepads and block new connections (PIO wired USB took over). */
    void wired_usb_takeover_disconnect_bt();
    /** Re-enable BT pairing after wired USB device unplugged. */
    void wired_usb_release_enable_bt_pairing();
    /** Pico W / Pico 2 W / RP2354 BT: restore pairing scans after USB host resume (e.g. Xbox 360 standby wake). */
    void on_usb_device_resume();
    /** Call from main loop to send deferred PS5 adaptive-trigger updates (keeps BT callback fast). */
    void process_pending_adaptive_triggers();
    /** Optional: when set, a timer in the BT run loop calls this every ~4 ms (for GPIO device modes: PS1/PS2, GameCube). Call before run_task(). */
    void set_gpio_device_process_callback(void (*callback)(void* ctx), void* ctx);
} 
