#ifndef OGXM_REMOTE_CONTROL_H
#define OGXM_REMOTE_CONTROL_H

#include <cstddef>
#include <cstdint>
#include "USBDevice/DeviceDriver/DeviceDriverTypes.h"
#include "UserSettings/ControllerCalibration.h"

class Gamepad;

namespace RemoteControl {

struct HttpResult {
    uint16_t status;
    size_t length;
    bool save_response;
};

// Core0 lifecycle, after DeviceManager has configured the physical Gamepad.
void begin(Gamepad& local, DeviceDriverType driver);
void tick(uint32_t now_ms);
Gamepad& output_gamepad(Gamepad& local);
void on_output_processed(Gamepad& output);

// USB service continues throughout this handshake. Bluetooth report callbacks
// skip profile reads while it is pending. The serialized radio apply hook only
// acknowledges quiescence; Core0 tick commits the configurations between USB
// visits. The clone uses the same acknowledgement without a Bluetooth producer.
bool profile_apply_pending();
void apply_profile_on_radio_core();

// Bluetooth callbacks publish input before calibration, remapping and curves.
// They never block on Core0. Configure mode retains radio input for calibration
// while USB output is neutral; Wi-Fi mode disables the physical radio source.
void on_bluetooth_input(const ControllerSample& sample, uint32_t now_ms);
void note_bluetooth_connected(bool connected);
bool bluetooth_input_allowed();
bool physical_output_enabled();
// General settings/updater routes use this same board-aware configuration gate.
bool configuration_allowed();

// Network callbacks use a bounded nonblocking mailbox; they never touch a
// Gamepad or flash. Body length is explicit (no required terminating NUL).
HttpResult handle_request(const char* method, const char* path,
                          const char* json, size_t json_len,
                          char* output, size_t capacity, uint32_t now_ms);
HttpResult handle_ws(const char* json, size_t length,
                     char* output, size_t capacity, uint32_t now_ms);
void release_token(uint32_t token, uint32_t now_ms);

// Both radio backends use these three request slots. Transport callbacks publish
// only borrowed, stable request/response buffers. The normal radio service poll
// consumes at most ONE request per visit, outside receive callbacks; Core0 tick
// never parses or formats requests. The owner must keep buffers
// intact until poll_core_result() reports completion. No heap or spin waits.
constexpr unsigned CORE_REQUEST_SLOTS = 3;
void service_network_requests();
bool submit_core_request(unsigned slot, bool websocket, const char* method,
                         const char* path, const char* body, size_t length,
                         char* response, size_t capacity, uint32_t received_at);
bool poll_core_result(unsigned slot, HttpResult& result);
bool core_request_busy(unsigned slot);
void abandon_core_request(unsigned slot);

// Core0 only: maintenance forces both remote and physical output to neutral.
// Network acquire/input/heartbeat requests return 423 while it is active.
// Clearing maintenance never restores the old owner or a held input.
void set_maintenance(bool active);
bool maintenance_active();
// Core0 snapshots, safe for the status LED without taking the network mailbox.
bool session_active();
bool input_fresh();
uint8_t applied_profile_id();
// Radio-side ownership check for deferring a potentially blocking Wi-Fi join.
// Reads the published owner deadline without acquiring the mailbox. Ordinary
// Core0 activity must not falsely block scans or joins when nobody owns input.
bool network_control_active(uint32_t now_ms);

// Call only on Core0 after the transport completes the WHOLE save response.
// CYW43 waits for peer TCP ACK; ESP SEND OK only proves module acceptance.
// Existing save lifecycle deliberately disconnects USB and reboots.
void commit_after_response();
void cancel_save();

} // namespace RemoteControl
#endif
