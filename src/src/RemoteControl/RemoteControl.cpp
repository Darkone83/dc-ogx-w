#include "RemoteControl/RemoteControl.h"
#include "RemoteControl/Protocol.h"
#include "RemoteControl/Profile.h"

#include <atomic>
#include <cstring>
#include <pico/rand.h>

#include "Gamepad/Gamepad.h"
#include "UserSettings/UserSettings.h"
#if defined(CONFIG_EN_BLUETOOTH)
#include "Bluepad32/Bluepad32.h"
#endif

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "development"
#endif
#ifndef OGXM_REMOTE_VERSION
#define OGXM_REMOTE_VERSION "0.1.0"
#endif

namespace RemoteControl {
namespace {

// Core0 never waits for a network callback. A network request gets a bounded
// opportunity to acquire the short mailbox before returning a retryable 503.
// Formatting and all Gamepad/flash operations happen outside this lock.
// Pico SDK supplies byte exchange, but some ARM toolchains lower atomic_flag
// to an unavailable __atomic_test_and_set helper. A boolean exchange preserves
// the same bounded try-lock semantics with the supported SDK primitive.
std::atomic<bool> mailbox{false};
std::atomic<bool> network_waiting{false};
bool try_lock() { return !mailbox.exchange(true, std::memory_order_acquire); }
void unlock() { mailbox.store(false, std::memory_order_release); }

bool try_lock_core() {
    return !network_waiting.load(std::memory_order_acquire) && try_lock();
}

bool try_lock_network() {
    // Without this short priority window, the tight USB loop can reacquire
    // the mailbox several times before one status/input request gets a turn.
    // Never leave the hint set across callbacks or HTTP retries.
    network_waiting.store(true, std::memory_order_release);
    for (unsigned attempt = 0; attempt < 256; ++attempt) {
        if (!mailbox.load(std::memory_order_relaxed) && try_lock()) {
            network_waiting.store(false, std::memory_order_release);
            return true;
        }
    }
    network_waiting.store(false, std::memory_order_release);
    return false;
}

enum class Source : uint8_t { Local, Remote, Neutral };
enum class Mode : unsigned { Physical, Wifi, Configure };
#if defined(CONFIG_EN_BLUETOOTH)
constexpr bool BLUETOOTH_SUPPORTED = true;
#else
constexpr bool BLUETOOTH_SUPPORTED = false;
#endif
#if defined(CONFIG_EN_USB_HOST) || defined(CONFIG_EN_BLUETOOTH)
constexpr bool PHYSICAL_SUPPORTED = true;
#else
constexpr bool PHYSICAL_SUPPORTED = false;
#endif
// USB and Bluetooth are physical input sources on the same controller runtime.
constexpr Mode BOOT_MODE = PHYSICAL_SUPPORTED ? Mode::Physical : Mode::Wifi;
std::atomic<Mode> requested_mode{BOOT_MODE}, applied_mode{BOOT_MODE}, radio_mode{BOOT_MODE};
std::atomic<bool> bluetooth_connected{false};
std::atomic<uint32_t> bluetooth_generation{0};
Mode core_mode_target = BOOT_MODE;
bool mode_handoff{};
uint32_t mode_handoff_started{};

const char* mode_name(Mode mode) {
    return mode == Mode::Wifi ? "wifi" : mode == Mode::Configure ? "configure" : "physical";
}

enum class CapturePhase : uint8_t { Idle, Center, Range };
struct Capture {
    ControllerCalibration draft;
    CapturePhase phase{CapturePhase::Idle};
    uint32_t samples{};
    uint32_t generation{};
    int32_t sum[6]{};
    int16_t minimum[6]{}, maximum[6]{};
};
struct BluetoothSnapshot {
    ControllerSample raw;
    uint32_t received_at{};
    uint32_t generation{};
    bool have_sample{};
};
struct Snapshot {
    Gamepad::PadIn processed;
    Gamepad::PadIn physical;
    InputState raw;
    uint32_t applied_seq{};
    bool has_applied_seq{};
    Source source{Source::Local};
    uint8_t rumble_l{}, rumble_r{};
};
struct Shared {
    Lease lease;
    UserProfile requested;
    UserProfile applied;
    UserProfile saved;
    ControllerCalibration calibration, applied_calibration, saved_calibration;
    Capture capture;
    BluetoothSnapshot bluetooth;
    Snapshot snapshot;
    uint32_t revision{1};
    uint32_t applied_revision{1};
    uint32_t previous_token{};
    uint32_t save_revision{};
    bool save_pending{};
    bool profile_recovered{};
};
Shared shared;
// Updated under the mailbox whenever ownership changes or expires. Reading
// this single word never mistakes Core0 mailbox contention for a real owner.
std::atomic<uint32_t> network_owner_until{0};

void publish_owner(uint32_t now) {
    const uint32_t remaining = shared.lease.owner_remaining(now);
    // Use the actual receive timestamp: Core0 may have sampled now one tick
    // before a newer radio heartbeat won the mailbox. Do not shorten its lease.
    const uint32_t deadline = shared.lease.owner_deadline();
    // Zero denotes no owner. At millisecond rollover reserve one extra ms,
    // conservatively, rather than exposing an active owner as unowned.
    network_owner_until.store(remaining ? (deadline ? deadline : 1u) : 0u,
                              std::memory_order_release);
}
// Read once on Core0 before the network starts. Network profile selection must
// never enter NVS/flash from a CYW43 callback or the ESP UART processing loop.
UserProfile stored_profiles[UserSettings::MAX_PROFILES];
ControllerCalibration stored_calibrations[UserSettings::MAX_PROFILES];
bool stored_profile_recovered[UserSettings::MAX_PROFILES]{};
Gamepad* physical_pad{};
Gamepad remote_pad;
DeviceDriverType output_driver{DeviceDriverType::NONE};
bool initialized{};
uint8_t core_applied_profile_id{1};

// Only Bluetooth report production pauses during a profile handoff. The radio
// acknowledges between its serialized callbacks; Core0 then commits both
// Gamepad configurations between its normal USB service visits.
enum ApplyStage : unsigned { Idle = 0, Ready, Applying, RadioDone, CoreDone };
std::atomic<unsigned> apply_stage{Idle};
UserProfile profile_to_apply;
ControllerCalibration calibration_to_apply;
uint32_t profile_to_apply_revision{};
std::atomic<bool> commit_requested{false};
std::atomic<bool> cancel_requested{false};
std::atomic<bool> maintenance{false};
std::atomic<bool> clear_owner_requested{false};
std::atomic<uint32_t> pending_release_token{0};

// Radio service consumes requests outside transport callbacks. The caller
// keeps borrowed buffers stable until completion. Keep the slot state atomic but
// all other fields plain: they are published/read through release/acquire.
struct CoreRequest {
    std::atomic<unsigned> state{0}; // 0 free, 1 queued, 2 executing, 3 done
    std::atomic<bool> abandoned{false};
    bool websocket{};
    char method[8]{};
    char path[80]{};
    char ws_body[1025]{}; // masked WebSocket frames cannot be borrowed
    const char* body{};
    size_t length{};
    char* output{};
    size_t capacity{};
    uint32_t received_at{};
    HttpResult result{};
};
CoreRequest core_requests[CORE_REQUEST_SLOTS];
unsigned core_request_rr{};

bool remote_selected{};
bool owner_selected{};
uint32_t owner_observed_at{};
uint32_t owner_observed_remaining{};
bool handoff_active{};
uint32_t handoff_started{};
uint32_t selected_generation{};
uint32_t lease_observed_at{};
uint32_t lease_observed_remaining{};
InputState last_remote_raw;
bool have_remote_raw{};
Gamepad::PadIn latest_physical;
Snapshot core_snapshot;
// Reissue neutral across several USB service visits so the release is not
// reduced to a single report that could arrive while an endpoint is busy.
constexpr uint32_t NEUTRAL_HANDOFF_MS = 8;

bool profile_dirty() {
    return std::memcmp(&shared.requested, &shared.saved, sizeof(UserProfile)) != 0 ||
           std::memcmp(&shared.calibration, &shared.saved_calibration, sizeof(ControllerCalibration)) != 0;
}

void bump_revision() { if (!++shared.revision) ++shared.revision; }

bool mode_pending() {
    const Mode requested = requested_mode.load(std::memory_order_acquire);
    return requested != applied_mode.load(std::memory_order_acquire) ||
           requested != radio_mode.load(std::memory_order_acquire);
}

void write_mode(JsonWriter& writer) {
    const Mode requested = requested_mode.load(std::memory_order_acquire);
    writer.append("\"mode\":\"%s\",\"requested_mode\":\"%s\",\"mode_pending\":%s,"
                  "\"physical_supported\":%s,\"bluetooth_supported\":%s,\"bluetooth_connected\":%s,\"bluetooth_allowed\":%s,"
                  "\"usb_suppressed\":%s",
                  mode_name(applied_mode.load(std::memory_order_acquire)), mode_name(requested),
                  mode_pending() ? "true" : "false", PHYSICAL_SUPPORTED ? "true" : "false",
                  BLUETOOTH_SUPPORTED ? "true" : "false",
                  bluetooth_connected.load(std::memory_order_acquire) ? "true" : "false",
                  BLUETOOTH_SUPPORTED && requested != Mode::Wifi ? "true" : "false",
                  requested == Mode::Configure || mode_pending() || profile_apply_pending() || maintenance.load() ? "true" : "false");
}


const char* driver_name(DeviceDriverType driver) {
    switch (driver) {
        case DeviceDriverType::XBOXOG: return "XBOXOG";
        case DeviceDriverType::XBOXOG_SB: return "XBOXOG_SB";
        case DeviceDriverType::XBOXOG_XR: return "XBOXOG_XR";
        case DeviceDriverType::XINPUT: return "XINPUT";
        case DeviceDriverType::PS3: return "PS3";
        case DeviceDriverType::DINPUT: return "DINPUT";
        case DeviceDriverType::PSCLASSIC: return "PSCLASSIC";
        case DeviceDriverType::SWITCH: return "SWITCH";
        case DeviceDriverType::WIIU: return "WIIU";
        case DeviceDriverType::WII: return "WII";
        case DeviceDriverType::PS1PS2: return "PS1PS2";
        case DeviceDriverType::GAMECUBE: return "GAMECUBE";
        case DeviceDriverType::DREAMCAST: return "DREAMCAST";
        case DeviceDriverType::N64: return "N64";
        case DeviceDriverType::PS4: return "PS4";
        case DeviceDriverType::STEAM: return "STEAM";
        case DeviceDriverType::WEBAPP: return "WEBAPP";
        default: return "UNKNOWN";
    }
}

const char* board_name() {
#if defined(CONFIG_OGXM_BOARD_PI_PICOW_2023)
    return "RP2040-Pico W-2023";
#elif defined(CONFIG_OGXM_BOARD_PI_PICO2W)
    return "Pico 2 W";
#else
    return "Pico W";
#endif
}

HttpResult error_response(char* output, size_t capacity, uint16_t status, const char* error) {
    JsonWriter writer(output, capacity);
    writer.append("{\"ok\":false,\"error\":\"%s\"}", error);
    return {status, writer.ok() ? writer.length() : 0, false};
}

HttpResult finish(JsonWriter& writer, char* output, size_t capacity,
                  uint16_t status = 200, bool save = false) {
    if (!writer.ok()) return error_response(output, capacity, 500, "response_too_large");
    return {status, writer.length(), save};
}

HttpResult get_mode(char* output, size_t capacity, uint16_t status = 200) {
    JsonWriter writer(output, capacity);
    writer.append("{\"ok\":true,");
    write_mode(writer);
    writer.append("}");
    return finish(writer, output, capacity, status);
}

void write_sample(JsonWriter& writer, const ControllerSample& sample) {
    writer.append("{\"buttons\":%u,\"dpad\":%u,\"lx\":%d,\"ly\":%d,"
                  "\"rx\":%d,\"ry\":%d,\"lt\":%u,\"rt\":%u}",
                  unsigned(sample.buttons), unsigned(sample.dpad), sample.axis[0], sample.axis[1],
                  sample.axis[2], sample.axis[3], unsigned(sample.trigger[0]), unsigned(sample.trigger[1]));
}

void write_calibration(JsonWriter& writer, const ControllerCalibration& calibration) {
    static constexpr const char* axes[] = {"lx", "ly", "rx", "ry"};
    writer.append("{\"mask\":%u,\"axes\":{", unsigned(calibration.mask));
    for (unsigned i = 0; i < 4; ++i)
        writer.append("%s\"%s\":{\"min\":%d,\"center\":%d,\"max\":%d}", i ? "," : "", axes[i],
                      calibration.axis[i].minimum, calibration.axis[i].center, calibration.axis[i].maximum);
    writer.append("},\"triggers\":{\"lt\":{\"min\":%u,\"max\":%u},\"rt\":{\"min\":%u,\"max\":%u}}}",
                  unsigned(calibration.trigger[0].minimum), unsigned(calibration.trigger[0].maximum),
                  unsigned(calibration.trigger[1].minimum), unsigned(calibration.trigger[1].maximum));
}

// Capture and current settings fit in a bounded snapshot; JSON formatting does
// not occupy the cross-core mailbox or block real-time USB reports.
HttpResult get_calibration(char* output, size_t capacity, uint32_t now, bool locked = false) {
    if (!locked && !try_lock_network()) return error_response(output, capacity, 503, "service_busy");
    const auto calibration = shared.calibration;
    const auto capture = shared.capture;
    const auto sample = shared.bluetooth;
    const unsigned id = shared.requested.id;
    const uint32_t revision = shared.revision, applied = shared.applied_revision;
    const bool dirty = profile_dirty();
    unlock();
    const bool connected = bluetooth_connected.load(std::memory_order_acquire);
    const bool same_controller = sample.generation == bluetooth_generation.load(std::memory_order_acquire);
    const uint32_t age = !sample.have_sample ? UINT32_MAX :
        int32_t(now - sample.received_at) < 0 ? 0 : now - sample.received_at;
    JsonWriter writer(output, capacity);
    writer.append("{\"ok\":true,\"supported\":%s,\"mode\":\"%s\",\"profile_id\":%u,"
                  "\"revision\":%lu,\"applied_revision\":%lu,\"dirty\":%s,\"phase\":\"%s\","
                  "\"samples\":%lu,\"connected\":%s,\"fresh\":%s,\"sample_age_ms\":%lu,\"raw\":",
                  BLUETOOTH_SUPPORTED ? "true" : "false", mode_name(applied_mode.load()), id,
                  static_cast<unsigned long>(revision), static_cast<unsigned long>(applied), dirty ? "true" : "false",
                  capture.phase == CapturePhase::Center ? "center" : capture.phase == CapturePhase::Range ? "range" : "idle",
                  static_cast<unsigned long>(capture.samples), connected ? "true" : "false",
                  connected && same_controller && sample.have_sample && age < 1000 ? "true" : "false", static_cast<unsigned long>(age));
    write_sample(writer, sample.raw);
    writer.append(",\"calibration\":");
    write_calibration(writer, calibration);
    writer.append(",\"draft\":");
    write_calibration(writer, capture.draft);
    writer.append("}");
    return finish(writer, output, capacity);
}

HttpResult calibration_request(const Json& json, char* output, size_t capacity, uint32_t now) {
    if (!BLUETOOTH_SUPPORTED) return error_response(output, capacity, 409, "bluetooth_not_supported");
    const int action = json.find(0, "action");
    const bool begin_capture = json.equals(action, "begin");
    const bool capture_center = json.equals(action, "center");
    const bool apply_capture = json.equals(action, "apply");
    const bool cancel_capture = json.equals(action, "cancel");
    const bool reset = json.equals(action, "reset");
    if (!begin_capture && !capture_center && !apply_capture && !cancel_capture && !reset)
        return error_response(output, capacity, 400, "invalid_calibration_action");
    int64_t mask = 63;
    if (begin_capture && json.find(0, "mask") >= 0 && !json.integer(json.find(0, "mask"), 1, 63, mask))
        return error_response(output, capacity, 400, "invalid_calibration_mask");
    if (!try_lock_network()) return error_response(output, capacity, 503, "service_busy");
    const char* error = nullptr;
    // Live calibration must not drop controller output. Wi-Fi ownership still
    // excludes Bluetooth input; saving continues to require Configuration mode.
    if (requested_mode.load() == Mode::Wifi || mode_pending()) error = "physical_mode_required";
    else if (shared.save_pending) error = "save_pending_restart";
    else if (!cancel_capture && !reset && (!bluetooth_connected.load() || !shared.bluetooth.have_sample ||
             shared.bluetooth.generation != bluetooth_generation.load() ||
             int32_t(now - shared.bluetooth.received_at) >= 1000)) error = "bluetooth_sample_stale";
    auto& capture = shared.capture;
    if (!error && capture_center) {
        if (capture.phase != CapturePhase::Center || capture.samples < 32 ||
            capture.generation != bluetooth_generation.load()) error = "calibration_needs_samples";
        else for (unsigned i = 0; i < 6; ++i) {
            if (!(capture.draft.mask & (1u << i))) continue;
            if (int(capture.maximum[i]) - capture.minimum[i] > (i < 4 ? 2048 : 8))
                error = "calibration_center_unstable";
        }
    }
    if (!error && apply_capture && (capture.phase != CapturePhase::Range || capture.samples < 32 ||
        capture.generation != bluetooth_generation.load()))
        error = "calibration_needs_samples";
    if (!error && apply_capture && !capture.draft.valid()) error = "calibration_range_incomplete";
    if (error) { unlock(); return error_response(output, capacity, 409, error); }

    if (begin_capture) {
        capture = Capture{};
        capture.draft.mask = static_cast<uint8_t>(mask);
        capture.generation = bluetooth_generation.load();
        capture.phase = CapturePhase::Center;
    } else if (capture_center) {
        for (unsigned i = 0; i < 4; ++i) {
            if (!(capture.draft.mask & (1u << i))) continue;
            const int16_t center = static_cast<int16_t>(capture.sum[i] / int32_t(capture.samples));
            capture.draft.axis[i].minimum = capture.draft.axis[i].maximum = capture.draft.axis[i].center = center;
        }
        for (unsigned i = 0; i < 2; ++i) {
            if (!(capture.draft.mask & (16u << i))) continue;
            capture.draft.trigger[i].minimum = capture.draft.trigger[i].maximum =
                static_cast<uint8_t>(capture.sum[4 + i] / int32_t(capture.samples));
        }
        capture.samples = 0;
        capture.phase = CapturePhase::Range;
    } else if (apply_capture || reset) {
        const auto candidate = reset ? ControllerCalibration{} : capture.draft;
        if (std::memcmp(&candidate, &shared.calibration, sizeof(candidate))) {
            shared.calibration = candidate;
            bump_revision();
        }
        capture = Capture{};
    } else capture = Capture{};
    return get_calibration(output, capacity, now, true);
}

// Drop the bounded parser workspace before entering handle_request(), which
// has its own parser. Inlining these scopes would stack two 1.5 KiB token arrays
// in the network callback on a core with an 8 KiB stack.
[[gnu::noinline]] bool is_heartbeat_message(const char* body, size_t length) {
    Json json;
    return json.parse(body, length) && json.count() &&
           json.at(0).type == Json::Type::Object &&
           json.equals(json.find(0, "type"), "heartbeat");
}

bool request_token(const Json& json, uint32_t& token, bool optional = false) {
    const int field = json.find(0, "token");
    if (field < 0 && optional) { token = 0; return true; }
    int64_t value;
    if (!json.integer(field, 1, UINT32_MAX, value)) return false;
    token = static_cast<uint32_t>(value);
    return true;
}

void write_pad(JsonWriter& writer, const Gamepad::PadIn& pad) {
    // Public telemetry uses the same +Y-up coordinates as remote input.
    writer.append("{\"buttons\":%u,\"dpad\":%u,\"lx\":%d,\"ly\":%d,"
                  "\"rx\":%d,\"ry\":%d,\"lt\":%u,\"rt\":%u}",
                  static_cast<unsigned>(pad.buttons), static_cast<unsigned>(pad.dpad),
                  static_cast<int>(pad.joystick_lx), static_cast<int>(Range::invert(pad.joystick_ly)),
                  static_cast<int>(pad.joystick_rx), static_cast<int>(Range::invert(pad.joystick_ry)),
                  static_cast<unsigned>(pad.trigger_l), static_cast<unsigned>(pad.trigger_r));
}

Gamepad::PadIn map_remote(const InputState& state) {
    Gamepad::PadIn pad;
    if (state.dpad & Gamepad::DPAD_UP) pad.dpad |= remote_pad.MAP_DPAD_UP;
    if (state.dpad & Gamepad::DPAD_DOWN) pad.dpad |= remote_pad.MAP_DPAD_DOWN;
    if (state.dpad & Gamepad::DPAD_LEFT) pad.dpad |= remote_pad.MAP_DPAD_LEFT;
    if (state.dpad & Gamepad::DPAD_RIGHT) pad.dpad |= remote_pad.MAP_DPAD_RIGHT;
    if ((pad.dpad & 3) == 3) pad.dpad &= static_cast<uint8_t>(~3u);
    if ((pad.dpad & 12) == 12) pad.dpad &= static_cast<uint8_t>(~12u);
    const uint16_t maps[] = {
        remote_pad.MAP_BUTTON_A, remote_pad.MAP_BUTTON_B, remote_pad.MAP_BUTTON_X,
        remote_pad.MAP_BUTTON_Y, remote_pad.MAP_BUTTON_L3, remote_pad.MAP_BUTTON_R3,
        remote_pad.MAP_BUTTON_BACK, remote_pad.MAP_BUTTON_START,
        remote_pad.MAP_BUTTON_LB, remote_pad.MAP_BUTTON_RB,
        remote_pad.MAP_BUTTON_SYS, remote_pad.MAP_BUTTON_MISC,
    };
    for (unsigned i = 0; i < 12; ++i)
        if (state.buttons & (1u << i)) pad.buttons |= maps[i];
    // PadIn is packed; binding std::tie references to its int16 members would
    // violate alignment on RP2040. Assign packed members directly instead.
    const auto left = remote_pad.scale_joystick_l(state.lx, state.ly, true);
    const auto right = remote_pad.scale_joystick_r(state.rx, state.ry, true);
    pad.joystick_lx = left.first;
    pad.joystick_ly = left.second;
    pad.joystick_rx = right.first;
    pad.joystick_ry = right.second;
    pad.trigger_l = remote_pad.scale_trigger_l(state.lt);
    pad.trigger_r = remote_pad.scale_trigger_r(state.rt);
    return pad;
}

void drain_physical_to_latest() {
    // Physical producers continue while remote owns output. Discard their old
    // history, retain the latest state, and never replay a burst on handoff.
    for (unsigned i = 0; i < Gamepad::PAD_IN_QUEUE_SIZE + 1; ++i) {
        if (!physical_pad->new_pad_in()) break;
        latest_physical = physical_pad->get_pad_in();
    }
}

HttpResult get_profile(char* output, size_t capacity, bool already_locked = false) {
    if (!already_locked && !try_lock_network())
        return error_response(output, capacity, 503, "service_busy");
    const UserProfile profile = shared.requested;
    const uint32_t revision = shared.revision;
    const uint32_t applied = shared.applied_revision;
    const unsigned applied_id = shared.applied.id;
    const bool dirty = profile_dirty();
    const bool saving = shared.save_pending;
    const bool recovered = shared.profile_recovered;
    unlock();
    JsonWriter writer(output, capacity);
    writer.append("{\"ok\":true,\"revision\":%lu,\"applied_revision\":%lu,\"applied_profile_id\":%u,\"dirty\":%s,"
                  "\"save_pending\":%s,\"recovered_invalid_profile\":%s,\"profile\":",
                  static_cast<unsigned long>(revision), static_cast<unsigned long>(applied),
                  applied_id,
                  dirty ? "true" : "false", saving ? "true" : "false", recovered ? "true" : "false");
    write_profile(writer, profile);
    const bool xinput = output_driver == DeviceDriverType::XINPUT;
    writer.append(",\"effective\":{\"xinput_stock_l\":%s,\"xinput_stock_r\":%s,"
                  "\"stock_left_inner\":0.08,\"stock_right_inner\":0.09,\"stock_curve\":0.35}}",
                  xinput && !JoystickSettings::raw_has_customization(profile.joystick_settings_l) ? "true" : "false",
                  xinput && !JoystickSettings::raw_has_customization(profile.joystick_settings_r) ? "true" : "false");
    return finish(writer, output, capacity);
}

HttpResult get_profiles(char* output, size_t capacity) {
    if (!try_lock_network()) return error_response(output, capacity, 503, "service_busy");
    const unsigned requested_id = shared.requested.id;
    const unsigned applied_id = shared.applied.id;
    unlock();
    JsonWriter writer(output, capacity);
    writer.append("{\"ok\":true,\"profile_id\":%u,\"applied_profile_id\":%u,\"profiles\":[",
                  requested_id, applied_id);
    for (unsigned i = 0; i < UserSettings::MAX_PROFILES; ++i)
        writer.append("%s{\"id\":%u,\"name\":\"Profile %u\",\"recovered\":%s}",
                      i ? "," : "", i + 1, i + 1,
                      stored_profile_recovered[i] ? "true" : "false");
    writer.append("]}");
    return finish(writer, output, capacity);
}

HttpResult get_status(char* output, size_t capacity, uint32_t now) {
    if (!try_lock_network()) return error_response(output, capacity, 503, "service_busy");
    const Snapshot snapshot = shared.snapshot;
    const bool active = shared.lease.active() && shared.lease.owner_remaining(now) > 0;
    const bool input_active = shared.lease.input_active(now);
    const uint32_t remaining = shared.lease.owner_remaining(now);
    const uint32_t input_remaining = shared.lease.remaining(now);
    const uint32_t received = shared.lease.received();
    const uint32_t rejected = shared.lease.rejected();
    const uint32_t expired = shared.lease.expired();
    const uint32_t owner_expired = shared.lease.owner_expired();
    const uint32_t dropped = shared.lease.dropped();
    const uint32_t revision = shared.revision;
    const uint32_t applied = shared.applied_revision;
    const bool dirty = profile_dirty();
    const bool saving = shared.save_pending;
    const unsigned profile_id = shared.requested.id;
    const unsigned applied_profile = shared.applied.id;
    unlock();
    JsonWriter writer(output, capacity);
    writer.append("{\"ok\":true,\"board\":\"%s\",\"firmware\":\"%s\",\"remote_version\":\"%s\",\"remote_api\":2,"
                  "\"output_mode\":\"%s\",\"uptime_ms\":%lu,\"profile_id\":%u,\"applied_profile_id\":%u,"
                  "\"profile_revision\":%lu,\"applied_revision\":%lu,\"dirty\":%s,\"save_pending\":%s,"
                  "\"control\":{\"active\":%s,\"lease_ms\":%lu,\"timeout_ms\":250,"
                  "\"input_active\":%s,\"input_remaining_ms\":%lu,\"owner_timeout_ms\":5000,"
                  "\"maintenance\":%s,\"owner_expired\":%lu,"
                  "\"received\":%lu,\"expired\":%lu,\"rejected\":%lu,\"dropped\":%lu,"
                  "\"applied_seq\":%lu,\"has_applied_seq\":%s,\"ping_supported\":true,",
                  board_name(), FIRMWARE_VERSION, OGXM_REMOTE_VERSION, driver_name(output_driver), static_cast<unsigned long>(now),
                  profile_id, applied_profile, static_cast<unsigned long>(revision), static_cast<unsigned long>(applied),
                  dirty ? "true" : "false", saving ? "true" : "false", active ? "true" : "false",
                  static_cast<unsigned long>(remaining), input_active ? "true" : "false",
                  static_cast<unsigned long>(input_remaining), maintenance.load() ? "true" : "false",
                  static_cast<unsigned long>(owner_expired), static_cast<unsigned long>(received),
                  static_cast<unsigned long>(expired), static_cast<unsigned long>(rejected),
                  static_cast<unsigned long>(dropped), static_cast<unsigned long>(snapshot.applied_seq),
                  snapshot.has_applied_seq ? "true" : "false");
    write_mode(writer);
    writer.append("},\"output_source\":\"%s\",\"processed\":",
                  snapshot.source == Source::Remote ? "remote" : snapshot.source == Source::Neutral ? "neutral" : "local");
    write_pad(writer, snapshot.processed);
    writer.append(",\"physical\":");
    write_pad(writer, snapshot.physical);
    writer.append(",\"rumble\":{\"left\":%u,\"right\":%u}}",
                  static_cast<unsigned>(snapshot.rumble_l), static_cast<unsigned>(snapshot.rumble_r));
    return finish(writer, output, capacity);
}

HttpResult input_request(const Json& json, char* output, size_t capacity, uint32_t now) {
    InputState input;
    if (!parse_input(json, input)) return error_response(output, capacity, 400, "invalid_complete_input");
    if (!try_lock_network()) return error_response(output, capacity, 503, "service_busy");
    const ControlResult result = shared.lease.submit(input, now);
    const uint32_t applied_seq = shared.snapshot.applied_seq;
    publish_owner(now);
    unlock();
    if (result == ControlResult::NotOwner) return error_response(output, capacity, 409, "not_control_owner");
    if (result == ControlResult::Stale) return error_response(output, capacity, 409, "stale_sequence");
    if (result != ControlResult::Ok) return error_response(output, capacity, 400, "invalid_input");
    JsonWriter writer(output, capacity);
    writer.append("{\"ok\":true,\"token\":%lu,\"accepted_seq\":%lu,\"applied_seq\":%lu}",
                  static_cast<unsigned long>(input.token), static_cast<unsigned long>(input.seq),
                  static_cast<unsigned long>(applied_seq));
    return finish(writer, output, capacity);
}

} // namespace

void begin(Gamepad& local, DeviceDriverType driver) {
    physical_pad = &local;
    output_driver = driver;
    shared = Shared{};
    shared.saved = UserSettings::get_instance().get_profile_by_index(0);
    shared.requested = shared.saved;
    const char* error = nullptr;
    if (!validate_profile(shared.requested, error)) {
        shared.requested = UserProfile{};
        shared.applied_revision = 0;
        shared.profile_recovered = true;
    }
    shared.applied = shared.requested;
    shared.calibration = UserSettings::get_instance().get_calibration_by_id(shared.requested.id);
    shared.applied_calibration = shared.saved_calibration = shared.calibration;
    local.set_bluetooth_calibration(shared.calibration);
    core_applied_profile_id = shared.applied.id;
    for (unsigned i = 0; i < UserSettings::MAX_PROFILES; ++i) {
        stored_calibrations[i] = UserSettings::get_instance().get_calibration_by_id(static_cast<uint8_t>(i + 1));
        auto& profile = stored_profiles[i];
        profile = UserSettings::get_instance().get_profile_by_id(static_cast<uint8_t>(i + 1));
        stored_profile_recovered[i] = !validate_profile(profile, error) || profile.id != i + 1;
        if (stored_profile_recovered[i]) {
            profile = UserProfile{};
            profile.id = static_cast<uint8_t>(i + 1);
        }
    }
    remote_pad.set_profile(shared.applied, driver);
    remote_pad.set_analog_device(driver == DeviceDriverType::XBOXOG || driver == DeviceDriverType::PS3);
    remote_pad.set_analog_host(false); // The v1 wire protocol has digital face buttons.
    latest_physical = local.peek_latest_pad_in();
    core_snapshot = Snapshot{};
    remote_selected = handoff_active = have_remote_raw = false;
    selected_generation = 0;
    lease_observed_at = lease_observed_remaining = 0;
    owner_selected = false;
    owner_observed_at = owner_observed_remaining = 0;
    maintenance.store(false);
    requested_mode.store(BOOT_MODE);
    applied_mode.store(BOOT_MODE);
    radio_mode.store(BOOT_MODE);
    bluetooth_connected.store(false);
    bluetooth_generation.store(0);
    core_mode_target = BOOT_MODE;
    mode_handoff = false;
    network_waiting.store(false);
    for (auto& cmd : core_requests) {
        cmd.abandoned.store(false, std::memory_order_relaxed);
        cmd.state.store(0, std::memory_order_release);
    }
    core_request_rr = 0;
    network_owner_until.store(0);
    clear_owner_requested.store(false);
    pending_release_token.store(0);
    apply_stage.store(Idle, std::memory_order_release);
    commit_requested.store(false);
    cancel_requested.store(false);
    initialized = true;
}

bool profile_apply_pending() {
    return apply_stage.load(std::memory_order_acquire) != Idle;
}

void apply_profile_on_radio_core() {
    const Mode wanted = requested_mode.load(std::memory_order_acquire);
    if (wanted != radio_mode.load(std::memory_order_relaxed)) {
#if defined(CONFIG_EN_BLUETOOTH)
        bluepad32::set_remote_wifi_mode(wanted == Mode::Wifi);
#endif
        radio_mode.store(wanted, std::memory_order_release);
    }
    unsigned expected = Ready;
    if (!apply_stage.compare_exchange_strong(expected, Applying, std::memory_order_acq_rel)) return;
    // No Gamepad mutation on the radio core. Earlier Bluetooth callbacks have
    // completed, and new report callbacks remain gated until Core0 commits.
    apply_stage.store(RadioDone, std::memory_order_release);
}

// One borrowed request per radio service visit, outside the shared mailbox
// lock and outside transport receive callbacks. JSON work never runs in the
// Core0 USB service loop.
[[gnu::noinline]] void consume_core_request() {
    for (unsigned attempt = 0; attempt < CORE_REQUEST_SLOTS; ++attempt) {
        const unsigned id = (core_request_rr + attempt) % CORE_REQUEST_SLOTS;
        auto& cmd = core_requests[id];
        unsigned expected = 1;
        if (!cmd.state.compare_exchange_strong(expected, 2, std::memory_order_acq_rel))
            continue;
        core_request_rr = (id + 1) % CORE_REQUEST_SLOTS;
        if (!cmd.abandoned.load(std::memory_order_acquire)) {
            cmd.result = cmd.websocket
                ? handle_ws(cmd.body, cmd.length, cmd.output, cmd.capacity, cmd.received_at)
                : handle_request(cmd.method, cmd.path, cmd.body, cmd.length,
                                 cmd.output, cmd.capacity, cmd.received_at);
        } else cmd.result = {0, 0, false};
        // The legacy mailbox may occasionally be owned by the other core.
        // The radio service retries this request on its next poll,
        // rather than emitting service_busy over a long-running connection.
        // Only the exact, pre-side-effect busy result may be retried.
        constexpr char busy[] = "{\"ok\":false,\"error\":\"service_busy\"}";
        if (!cmd.abandoned.load(std::memory_order_acquire) &&
            cmd.result.status == 503 && cmd.result.length == sizeof(busy) - 1 &&
            std::memcmp(cmd.output, busy, sizeof(busy) - 1) == 0) {
            cmd.state.store(1, std::memory_order_release);
            break;
        }
        cmd.state.store(3, std::memory_order_release);
        break;
    }
}

void service_network_requests() {
    if (!initialized) return;
    consume_core_request();
}

void tick(uint32_t now_ms) {
    if (!initialized) return;

    if (apply_stage.load(std::memory_order_acquire) == RadioDone) {
        physical_pad->set_profile(profile_to_apply, output_driver);
        physical_pad->set_bluetooth_calibration(calibration_to_apply);
        remote_pad.set_profile(profile_to_apply, output_driver);
        // USB service continued while the radio acknowledged. Discard reports
        // mapped with the previous profile before resuming Bluetooth reports.
        physical_pad->reset_pad_in();
        remote_pad.reset_pad_in();
        latest_physical = Gamepad::PadIn{};
        core_applied_profile_id = profile_to_apply.id;
        if (remote_selected && have_remote_raw)
            remote_pad.set_pad_in_latest(map_remote(last_remote_raw));
        apply_stage.store(CoreDone, std::memory_order_release);
    }

    const Mode desired_mode = requested_mode.load(std::memory_order_acquire);
    if (desired_mode != core_mode_target) {
        core_mode_target = desired_mode;
        mode_handoff = true;
        mode_handoff_started = now_ms;
        have_remote_raw = false;
        remote_selected = owner_selected = false;
        handoff_active = false;
        remote_pad.reset_pad_in();
        physical_pad->set_pad_out(Gamepad::PadOut{});
        core_snapshot.has_applied_seq = false;
    }
    if (mode_handoff && radio_mode.load(std::memory_order_acquire) == desired_mode &&
        uint32_t(now_ms - mode_handoff_started) >= NEUTRAL_HANDOFF_MS) {
        mode_handoff = false;
        applied_mode.store(desired_mode, std::memory_order_release);
        // Discard captured configuration/old-owner buttons. A new physical
        // report is required after the explicit transition back to physical.
        drain_physical_to_latest();
        physical_pad->set_pad_in_latest(Gamepad::PadIn{});
        latest_physical = Gamepad::PadIn{};
    }

    InputState inputs[Lease::QUEUE_SIZE];
    unsigned input_count = 0;
    bool active = remote_selected;
    const bool was_owned = owner_selected;
    uint32_t generation = selected_generation;
    bool must_commit = false;
    UserProfile commit_profile;
    ControllerCalibration commit_calibration;
    if (try_lock_core()) {
        shared.lease.expire(now_ms);
        const uint32_t released = pending_release_token.exchange(0);
        if (released && shared.lease.token() == released) shared.lease.clear();
        const bool clear_owner = clear_owner_requested.exchange(false, std::memory_order_acq_rel);
        if ((clear_owner || maintenance.load(std::memory_order_acquire)) && shared.lease.active())
            shared.lease.clear();
        active = shared.lease.input_active(now_ms);
        owner_selected = shared.lease.active();
        owner_observed_at = now_ms;
        owner_observed_remaining = shared.lease.owner_remaining(now_ms);
        generation = shared.lease.generation();
        lease_observed_at = now_ms;
        lease_observed_remaining = shared.lease.remaining(now_ms);
        while (input_count < Lease::QUEUE_SIZE && shared.lease.pop(inputs[input_count])) ++input_count;

        if (apply_stage.load(std::memory_order_acquire) == CoreDone) {
            shared.applied = profile_to_apply;
            shared.applied_calibration = calibration_to_apply;
            shared.applied_revision = profile_to_apply_revision;
            apply_stage.store(Idle, std::memory_order_release);
        }
        if (apply_stage.load(std::memory_order_relaxed) == Idle && shared.revision != shared.applied_revision) {
            profile_to_apply = shared.requested;
            calibration_to_apply = shared.calibration;
            profile_to_apply_revision = shared.revision;
            apply_stage.store(Ready, std::memory_order_release);
        }

        if (cancel_requested.exchange(false) && !commit_requested.load()) shared.save_pending = false;
        if (commit_requested.load() && shared.save_pending &&
            shared.save_revision == shared.revision && shared.revision == shared.applied_revision &&
            apply_stage.load(std::memory_order_acquire) == Idle) {
            commit_profile = shared.applied;
            commit_calibration = shared.applied_calibration;
            must_commit = true;
            shared.save_pending = false;
            commit_requested.store(false);
            shared.lease.clear();
            active = owner_selected = false;
            generation = shared.lease.generation();
        }
        publish_owner(now_ms);
        unlock();
    } else if (active && static_cast<uint32_t>(now_ms - lease_observed_at) >= lease_observed_remaining) {
        // A network callback must never hold a controller beyond its last
        // observed lease simply by keeping the mailbox busy.
        active = false;
    }
    if (owner_selected && uint32_t(now_ms - owner_observed_at) >= owner_observed_remaining)
        owner_selected = false;
    if (maintenance.load(std::memory_order_acquire) || desired_mode != Mode::Wifi || mode_handoff) {
        active = false;
        owner_selected = false;
    }

    if (active && (!remote_selected || generation != selected_generation)) {
        remote_pad.reset_pad_in();
        physical_pad->set_pad_out(Gamepad::PadOut{});
        have_remote_raw = false;
        handoff_active = false;
        core_snapshot.has_applied_seq = false;
    }
    if ((!active && remote_selected) || (!owner_selected && was_owned)) {
        handoff_active = true;
        handoff_started = now_ms;
        have_remote_raw = false;
        remote_pad.reset_pad_in();
    }
    selected_generation = generation;
    remote_selected = active;

    if (desired_mode != Mode::Physical || mode_handoff || profile_apply_pending() ||
        maintenance.load(std::memory_order_acquire) ||
        owner_selected || remote_selected || handoff_active)
        drain_physical_to_latest();
    else latest_physical = physical_pad->peek_latest_pad_in();

    if (maintenance.load(std::memory_order_acquire) || desired_mode == Mode::Configure ||
        mode_handoff || profile_apply_pending()) {
        remote_pad.set_pad_in_latest(Gamepad::PadIn{});
        core_snapshot.source = Source::Neutral;
    } else if (remote_selected) {
        for (unsigned i = 0; i < input_count; ++i) {
            remote_pad.set_pad_in(map_remote(inputs[i]));
            last_remote_raw = inputs[i];
            have_remote_raw = true;
            core_snapshot.raw = inputs[i];
            core_snapshot.applied_seq = inputs[i].seq;
            core_snapshot.has_applied_seq = true;
        }
        core_snapshot.source = Source::Remote;
    } else if (owner_selected || desired_mode == Mode::Wifi) {
        // Wi-Fi is an explicit source mode, even without a live lease. It
        // cannot silently hand USB output back to a Bluetooth controller.
        // A heartbeat reserves ownership without extending held inputs. Keep
        // USB neutral throughout that pause; a Bluetooth pad must not silently
        // take over until this remote session is explicitly released/expired.
        remote_pad.set_pad_in_latest(Gamepad::PadIn{});
        core_snapshot.source = Source::Neutral;
    } else if (handoff_active && static_cast<uint32_t>(now_ms - handoff_started) < NEUTRAL_HANDOFF_MS) {
        remote_pad.set_pad_in_latest(Gamepad::PadIn{});
        core_snapshot.source = Source::Neutral;
    } else {
        if (handoff_active) {
            physical_pad->set_pad_in_latest(latest_physical);
            handoff_active = false;
        }
        core_snapshot.source = Source::Local;
    }
    core_snapshot.physical = latest_physical;
    if (try_lock_core()) {
        shared.snapshot = core_snapshot;
        unlock();
    }

    if (must_commit) {
        remote_pad.set_pad_in_latest(Gamepad::PadIn{});
        // Existing store lifecycle halts Core1, disconnects USB, writes NVS and
        // reboots. This is intentionally called only after transport ack.
        UserSettings::get_instance().store_profile(0, commit_profile, &commit_calibration);
    }
}

Gamepad& output_gamepad(Gamepad& local) {
    return initialized && (requested_mode.load(std::memory_order_acquire) != Mode::Physical || mode_handoff ||
                           profile_apply_pending() ||
                           owner_selected || remote_selected || handoff_active ||
                           maintenance.load(std::memory_order_acquire))
               ? remote_pad : local;
}

void on_output_processed(Gamepad& output) {
    if (!initialized) return;
    core_snapshot.processed = output.peek_consumed_pad_in();
    if (&output == &remote_pad && output.new_pad_out()) {
        const Gamepad::PadOut rumble = output.get_pad_out();
        core_snapshot.rumble_l = rumble.rumble_l;
        core_snapshot.rumble_r = rumble.rumble_r;
    }
    if (try_lock_core()) {
        shared.snapshot = core_snapshot;
        unlock();
    }
}

void note_bluetooth_connected(bool connected) {
    if (bluetooth_connected.exchange(connected, std::memory_order_acq_rel) != connected)
        bluetooth_generation.fetch_add(1, std::memory_order_acq_rel);
}

bool bluetooth_input_allowed() {
    return !initialized || requested_mode.load(std::memory_order_acquire) != Mode::Wifi;
}

bool physical_output_enabled() {
    return !initialized || (requested_mode.load(std::memory_order_acquire) == Mode::Physical &&
                           applied_mode.load(std::memory_order_acquire) == Mode::Physical &&
                           !profile_apply_pending() && !maintenance.load());
}

bool configuration_allowed() {
    return !PHYSICAL_SUPPORTED || (requested_mode.load(std::memory_order_acquire) == Mode::Configure && !mode_pending());
}

void on_bluetooth_input(const ControllerSample& sample, uint32_t now_ms) {
    if (!initialized || !bluetooth_input_allowed() || !try_lock()) return;
    const uint32_t generation = bluetooth_generation.load(std::memory_order_acquire);
    shared.bluetooth = {sample, now_ms, generation, true};
    auto& capture = shared.capture;
    if (capture.phase != CapturePhase::Idle && capture.generation != generation) capture = Capture{};
    if (requested_mode.load() != Mode::Wifi && !mode_pending() &&
        capture.phase != CapturePhase::Idle) {
        if (capture.phase == CapturePhase::Center && capture.samples < 256) {
            for (unsigned i = 0; i < 6; ++i) {
                const int16_t value = i < 4 ? sample.axis[i] : sample.trigger[i - 4];
                capture.sum[i] += value;
                if (!capture.samples || value < capture.minimum[i]) capture.minimum[i] = value;
                if (!capture.samples || value > capture.maximum[i]) capture.maximum[i] = value;
            }
            ++capture.samples;
        } else if (capture.phase == CapturePhase::Range) {
            for (unsigned i = 0; i < 4; ++i) {
                if (!(capture.draft.mask & (1u << i))) continue;
                auto& axis = capture.draft.axis[i];
                if (sample.axis[i] < axis.minimum) axis.minimum = sample.axis[i];
                if (sample.axis[i] > axis.maximum) axis.maximum = sample.axis[i];
            }
            for (unsigned i = 0; i < 2; ++i) {
                if (!(capture.draft.mask & (16u << i))) continue;
                if (sample.trigger[i] > capture.draft.trigger[i].maximum)
                    capture.draft.trigger[i].maximum = sample.trigger[i];
            }
            if (capture.samples < 65535) ++capture.samples;
        }
    }
    unlock();
}

HttpResult handle_request(const char* method, const char* path,
                          const char* body, size_t body_length,
                          char* output, size_t capacity, uint32_t now) {
    if (!initialized) return error_response(output, capacity, 503, "control_not_ready");
    if (!method || !path) return error_response(output, capacity, 400, "invalid_request");
    if (!std::strcmp(method, "GET")) {
        if (!std::strcmp(path, "/api/status")) return get_status(output, capacity, now);
        if (!std::strcmp(path, "/api/profile")) return get_profile(output, capacity);
        if (!std::strcmp(path, "/api/profiles")) return get_profiles(output, capacity);
        if (!std::strcmp(path, "/api/control/mode")) return get_mode(output, capacity);
        if (!std::strcmp(path, "/api/calibration")) return get_calibration(output, capacity, now);
        return error_response(output, capacity, 404, "not_found");
    }
    if (std::strcmp(method, "POST")) return error_response(output, capacity, 405, "method_not_allowed");
    Json json;
    if (!body_length) { body = "{}"; body_length = 2; }
    if (!json.parse(body, body_length) || json.at(0).type != Json::Type::Object)
        return error_response(output, capacity, 400, "invalid_json_object");

    if (maintenance.load(std::memory_order_acquire) &&
        (!std::strcmp(path, "/api/control/input") || !std::strcmp(path, "/api/control/acquire") ||
         !std::strcmp(path, "/api/control/heartbeat") ||
         !std::strcmp(path, "/api/control/mode") || !std::strcmp(path, "/api/calibration") ||
         !std::strncmp(path, "/api/profile", 12)))
        return error_response(output, capacity, 423, "maintenance_active");

    if (!std::strcmp(path, "/api/control/mode")) {
        const int field = json.find(0, "mode");
        Mode wanted;
        if (json.equals(field, "wifi")) wanted = Mode::Wifi;
        else if (json.equals(field, "physical")) wanted = Mode::Physical;
        else if (json.equals(field, "configure")) wanted = Mode::Configure;
        else return error_response(output, capacity, 400, "invalid_control_mode");
        if (!PHYSICAL_SUPPORTED && wanted != Mode::Wifi)
            return error_response(output, capacity, 409, "physical_not_supported");
        if (!try_lock_network()) return error_response(output, capacity, 503, "service_busy");
        if (shared.save_pending) { unlock(); return error_response(output, capacity, 409, "save_pending_restart"); }
        if (wanted != requested_mode.load(std::memory_order_relaxed)) {
            shared.lease.clear();
            publish_owner(now);
            shared.capture = Capture{};
            requested_mode.store(wanted, std::memory_order_release);
        }
        unlock();
        return get_mode(output, capacity, mode_pending() ? 202 : 200);
    }
    if (!std::strcmp(path, "/api/calibration")) return calibration_request(json, output, capacity, now);
    if ((!std::strcmp(path, "/api/control/input") || !std::strcmp(path, "/api/control/acquire") ||
         !std::strcmp(path, "/api/control/heartbeat")) &&
        (requested_mode.load(std::memory_order_acquire) != Mode::Wifi || mode_pending()))
        return error_response(output, capacity, 409, "wifi_mode_required");
    if (PHYSICAL_SUPPORTED && !std::strncmp(path, "/api/profile", 12) &&
        (requested_mode.load(std::memory_order_acquire) != Mode::Configure || mode_pending()))
        return error_response(output, capacity, 409, "configuration_mode_required");

    if (!std::strcmp(path, "/api/control/input")) return input_request(json, output, capacity, now);

    if (!std::strcmp(path, "/api/control/heartbeat")) {
        uint32_t token;
        if (!request_token(json, token)) return error_response(output, capacity, 400, "invalid_token");
        if (!try_lock_network()) return error_response(output, capacity, 503, "service_busy");
        const ControlResult result = shared.lease.heartbeat(token, now);
        const uint32_t remaining = shared.lease.owner_remaining(now);
        const bool fresh = shared.lease.input_active(now);
        publish_owner(now);
        unlock();
        if (result != ControlResult::Ok) return error_response(output, capacity, 409, "not_control_owner");
        JsonWriter writer(output, capacity);
        writer.append("{\"ok\":true,\"heartbeat\":true,\"token\":%lu,\"lease_ms\":%lu,\"input_active\":%s}",
                      static_cast<unsigned long>(token), static_cast<unsigned long>(remaining),
                      fresh ? "true" : "false");
        return finish(writer, output, capacity);
    }

    if (!std::strcmp(path, "/api/control/acquire")) {
        uint32_t requested;
        if (!request_token(json, requested, true)) return error_response(output, capacity, 400, "invalid_token");
        uint32_t generated = get_rand_32();
        if (!generated) generated = 1;
        if (!try_lock_network()) return error_response(output, capacity, 503, "service_busy");
        if (generated == shared.previous_token) generated = generated == UINT32_MAX ? 1 : generated + 1;
        const ControlResult result = shared.lease.acquire(requested, generated, now);
        const uint32_t token = shared.lease.token();
        if (result == ControlResult::Ok) shared.previous_token = token;
        publish_owner(now);
        unlock();
        if (result != ControlResult::Ok) return error_response(output, capacity, 409, "control_busy");
        JsonWriter writer(output, capacity);
        writer.append("{\"ok\":true,\"token\":%lu,\"timeout_ms\":250,\"owner_timeout_ms\":5000,\"ping_supported\":true}",
                      static_cast<unsigned long>(token));
        return finish(writer, output, capacity);
    }

    if (!std::strcmp(path, "/api/control/release")) {
        uint32_t token;
        if (!request_token(json, token)) return error_response(output, capacity, 400, "invalid_token");
        if (!try_lock_network()) return error_response(output, capacity, 503, "service_busy");
        const ControlResult result = shared.lease.release(token);
        publish_owner(now);
        unlock();
        if (result != ControlResult::Ok) return error_response(output, capacity, 409, "not_control_owner");
        JsonWriter writer(output, capacity);
        writer.append("{\"ok\":true,\"released\":true}");
        return finish(writer, output, capacity);
    }

    if (!std::strcmp(path, "/api/profile/select")) {
        int64_t id;
        if (!json.integer(json.find(0, "id"), 1, UserSettings::MAX_PROFILES, id))
            return error_response(output, capacity, 400, "invalid_profile_id");
        if (!try_lock_network()) return error_response(output, capacity, 503, "service_busy");
        if (shared.save_pending) {
            unlock();
            return error_response(output, capacity, 409, "save_pending_restart");
        }
        const UserProfile& selected = stored_profiles[id - 1];
        const auto& selected_calibration = stored_calibrations[id - 1];
        if (std::memcmp(&shared.requested, &selected, sizeof(selected)) ||
            std::memcmp(&shared.calibration, &selected_calibration, sizeof(selected_calibration))) {
            shared.requested = selected;
            shared.calibration = selected_calibration;
            shared.capture = Capture{};
            bump_revision();
        }
        shared.profile_recovered = stored_profile_recovered[id - 1];
        // Snapshot the reply under this same lock. A second acquisition could
        // return service_busy AFTER selecting the profile, making a safe retry
        // indistinguishable from a mutation that had already succeeded.
        return get_profile(output, capacity, true);
    }

    if (!std::strcmp(path, "/api/profile")) {
        UserProfile base;
        uint32_t base_revision;
        if (!try_lock_network()) return error_response(output, capacity, 503, "service_busy");
        const bool saving = shared.save_pending;
        base = shared.requested;
        base_revision = shared.revision;
        unlock();
        if (saving) return error_response(output, capacity, 409, "save_pending_restart");
        const int revision_field = json.find(0, "revision");
        if (revision_field >= 0) {
            int64_t revision;
            if (!json.integer(revision_field, 1, UINT32_MAX, revision))
                return error_response(output, capacity, 400, "invalid_revision");
            if (static_cast<uint32_t>(revision) != base_revision)
                return error_response(output, capacity, 409, "profile_revision_changed");
        }
        UserProfile candidate;
        const char* error = nullptr;
        if (!patch_profile(json, base, candidate, error)) return error_response(output, capacity, 400, error);
        if (!try_lock_network()) return error_response(output, capacity, 503, "service_busy");
        if (shared.save_pending || shared.revision != base_revision) {
            unlock();
            return error_response(output, capacity, 409, "profile_revision_changed");
        }
        const bool changed = std::memcmp(&shared.requested, &candidate, sizeof(candidate)) != 0;
        if (changed) {
            shared.requested = candidate;
            ++shared.revision;
            if (!shared.revision) ++shared.revision;
        }
        const uint32_t revision = shared.revision;
        const uint32_t applied = shared.applied_revision;
        unlock();
        JsonWriter writer(output, capacity);
        writer.append("{\"ok\":true,\"revision\":%lu,\"applied_revision\":%lu,\"pending\":%s}",
                      static_cast<unsigned long>(revision), static_cast<unsigned long>(applied),
                      revision != applied ? "true" : "false");
        return finish(writer, output, capacity, revision == applied ? 200 : 202);
    }

    if (!std::strcmp(path, "/api/profile/save")) {
        int64_t revision;
        if (!json.integer(json.find(0, "revision"), 1, UINT32_MAX, revision))
            return error_response(output, capacity, 400, "missing_revision");
        if (!try_lock_network()) return error_response(output, capacity, 503, "service_busy");
        if (shared.save_pending) {
            unlock();
            return error_response(output, capacity, 409, "save_pending_restart");
        }
        if (static_cast<uint32_t>(revision) != shared.revision || shared.revision != shared.applied_revision ||
            apply_stage.load(std::memory_order_acquire) != Idle) {
            unlock();
            return error_response(output, capacity, 409, "profile_not_applied");
        }
        shared.save_pending = true;
        shared.save_revision = shared.revision;
        unlock();
        JsonWriter writer(output, capacity);
        writer.append("{\"ok\":true,\"revision\":%lu,\"restart_required\":true,\"saving\":true}",
                      static_cast<unsigned long>(revision));
        return finish(writer, output, capacity, 202, true);
    }
    return error_response(output, capacity, 404, "not_found");
}

HttpResult handle_ws(const char* body, size_t length,
                     char* output, size_t capacity, uint32_t now) {
    const bool heartbeat = is_heartbeat_message(body, length);
    return handle_request("POST", heartbeat ? "/api/control/heartbeat" : "/api/control/input",
                          body, length, output, capacity, now);
}

bool submit_core_request(unsigned slot, bool websocket, const char* method,
                         const char* path, const char* body, size_t length,
                         char* response, size_t capacity, uint32_t received_at) {
    if (slot >= CORE_REQUEST_SLOTS || !response || !capacity || !body ||
        (!websocket && (!method || !path)) ||
        (websocket && length > 1024)) return false;
    auto& cmd = core_requests[slot];
    if (cmd.state.load(std::memory_order_acquire) != 0) return false;
    cmd.websocket = websocket;
    if (!websocket) {
        const size_t m = std::strlen(method), p = std::strlen(path);
        if (m >= sizeof(cmd.method) || p >= sizeof(cmd.path)) return false;
        std::memcpy(cmd.method, method, m + 1);
        std::memcpy(cmd.path, path, p + 1);
        cmd.body = body;
    } else {
        std::memcpy(cmd.ws_body, body, length);
        cmd.ws_body[length] = '\0';
        cmd.body = cmd.ws_body;
    }
    cmd.length = length;
    cmd.output = response;
    cmd.capacity = capacity;
    cmd.received_at = received_at;
    cmd.abandoned.store(false, std::memory_order_release);
    cmd.state.store(1, std::memory_order_release);
    return true;
}

bool poll_core_result(unsigned slot, HttpResult& result) {
    if (slot >= CORE_REQUEST_SLOTS) return false;
    auto& cmd = core_requests[slot];
    if (cmd.state.load(std::memory_order_acquire) != 3) return false;
    result = cmd.result;
    cmd.state.store(0, std::memory_order_release);
    return true;
}

bool core_request_busy(unsigned slot) {
    return slot < CORE_REQUEST_SLOTS &&
        core_requests[slot].state.load(std::memory_order_acquire) != 0;
}

void abandon_core_request(unsigned slot) {
    if (slot < CORE_REQUEST_SLOTS)
        core_requests[slot].abandoned.store(true, std::memory_order_release);
}

void release_token(uint32_t token, uint32_t now) {
    if (!initialized || !token) return;
    if (!try_lock_network()) {
        // A TCP close notification must not be lost just because Core0 holds
        // the bounded mailbox. Token comparison on Core0 protects a new owner.
        pending_release_token.store(token, std::memory_order_release);
        return;
    }
    shared.lease.expire(now);
    if (shared.lease.token() == token) shared.lease.clear();
    publish_owner(now);
    unlock();
}

void set_maintenance(bool active) {
    // Core0 only. An update always neutralizes USB output, including physical
    // Bluetooth input, and returning to normal requires an explicit acquire.
    const bool previous = maintenance.exchange(active, std::memory_order_acq_rel);
    if (!initialized || previous == active) return;
    if (active) {
        network_owner_until.store(0, std::memory_order_release);
        clear_owner_requested.store(true, std::memory_order_release);
        remote_selected = owner_selected = false;
        have_remote_raw = false;
        handoff_active = false;
        remote_pad.reset_pad_in();
        remote_pad.set_pad_in_latest(Gamepad::PadIn{});
        core_snapshot.source = Source::Neutral;
        core_snapshot.has_applied_seq = false;
        if (try_lock_core()) {
            shared.lease.clear();
            clear_owner_requested.store(false, std::memory_order_release);
            shared.snapshot = core_snapshot;
            unlock();
        }
    }
}

bool maintenance_active() { return maintenance.load(std::memory_order_acquire); }
bool session_active() { return initialized && owner_selected && !maintenance_active(); }
bool input_fresh() { return initialized && remote_selected && !maintenance_active(); }
uint8_t applied_profile_id() { return core_applied_profile_id; }

bool network_control_active(uint32_t now_ms) {
    if (!initialized || maintenance.load(std::memory_order_acquire)) return false;
    const uint32_t deadline = network_owner_until.load(std::memory_order_acquire);
    return deadline && static_cast<int32_t>(deadline - now_ms) > 0;
}

void commit_after_response() {
    commit_requested.store(true, std::memory_order_release);
}

void cancel_save() {
    cancel_requested.store(true, std::memory_order_release);
}

} // namespace RemoteControl
