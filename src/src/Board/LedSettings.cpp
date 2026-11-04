#include "Board/LedSettings.h"
#include "RemoteControl/Json.h"
#include "UserSettings/NVSTool.h"

#include <atomic>
#include <cstring>

#ifndef OGXM_STATUS_LED_BRIGHTNESS
#define OGXM_STATUS_LED_BRIGHTNESS 24
#endif

namespace LedSettings {
namespace {
using RemoteControl::Json;
using RemoteControl::JsonWriter;
constexpr std::uint32_t MAGIC = 0x444c4344; // DCLD
constexpr std::uint32_t VERSION = 1;
constexpr char KEY[] = "led-v1";
constexpr const char* DIAGNOSTIC_NAMES[] = {
    "starting", "access_point", "joining_wifi", "reconnecting_wifi",
    "wifi_failed", "usb_disconnected", "input_paused", "updating", "error",
    "profile_changed", "wifi_connected", "control_active"
};
static_assert(sizeof(Settings) == 64, "Keep the persisted LED layout explicit");
struct Record {
    std::uint32_t magic;
    std::uint32_t version;
    Settings settings;
    std::uint32_t checksum;
};
static_assert(sizeof(Record) <= NVSTool::VALUE_LEN_MAX, "LED record must fit one NVS entry");

// The settings and preview consumed by the renderer belong only to Core0.
Settings active;
Preview active_preview;
std::uint32_t active_revision = 1;

// All shared non-atomic fields below are covered by this short try-lock. Use a
// boolean exchange: RP2040 SDK supplies this atomic helper for Cortex-M0+.
std::atomic<bool> mailbox{false};
std::atomic<bool> save_pending{false};
Settings requested;
Settings persisted;
Settings staged;
std::uint32_t requested_revision = 1;
std::uint32_t published_revision = 1;
Color requested_test_color;
std::uint32_t requested_test_duration{};
bool test_requested{};
bool published_test_active{};

bool try_lock() { return !mailbox.exchange(true, std::memory_order_acquire); }
void unlock() { mailbox.store(false, std::memory_order_release); }

std::uint32_t crc32(const void* bytes, std::size_t length) {
    auto* p = static_cast<const std::uint8_t*>(bytes);
    std::uint32_t crc = 0xffffffffu;
    while (length--) {
        crc ^= *p++;
        for (unsigned i = 0; i != 8; ++i)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

bool valid(const Settings& settings) {
    return settings.enabled <= 1 && settings.reserved == 0 &&
        static_cast<unsigned>(settings.mode) <= static_cast<unsigned>(Mode::Diagnostics);
}

Result error(char* output, std::size_t capacity, std::uint16_t status, const char* message) {
    JsonWriter writer(output, capacity);
    writer.append("{\"ok\":false,\"error\":\"%s\"}", message);
    return {status, writer.length(), false};
}

int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool read_color(const Json& json, int token, const char* body, Color& out) {
    if (!json.valid(token) || json.at(token).type != Json::Type::String) return false;
    const auto& t = json.at(token);
    if (t.end - t.begin != 7 || body[t.begin] != '#') return false;
    unsigned rgb = 0;
    for (unsigned i = t.begin + 1; i < t.end; ++i) {
        const int value = hex(body[i]);
        if (value < 0) return false;
        rgb = (rgb << 4) | static_cast<unsigned>(value);
    }
    out = {static_cast<std::uint8_t>(rgb >> 16), static_cast<std::uint8_t>(rgb >> 8),
           static_cast<std::uint8_t>(rgb)};
    return true;
}

const char* parse_settings(const Json& json, const char* body, Settings& candidate) {
    for (unsigned key = 1; key < json.at(0).next; key = json.at(key + 1).next) {
        const int value = static_cast<int>(key + 1);
        if (json.equals(static_cast<int>(key), "enabled")) {
            bool enabled;
            if (json.at(value).type != Json::Type::Boolean || !json.boolean(value, enabled)) return "invalid_enabled";
            candidate.enabled = enabled ? 1 : 0;
        } else if (json.equals(static_cast<int>(key), "brightness")) {
            std::int64_t brightness;
            if (!json.integer(value, 0, 255, brightness)) return "invalid_brightness";
            candidate.brightness = static_cast<std::uint8_t>(brightness);
        } else if (json.equals(static_cast<int>(key), "mode")) {
            if (json.at(value).type != Json::Type::String) return "invalid_mode";
            if (json.equals(value, "combined")) candidate.mode = Mode::Combined;
            else if (json.equals(value, "profiles")) candidate.mode = Mode::Profiles;
            else if (json.equals(value, "diagnostics")) candidate.mode = Mode::Diagnostics;
            else return "invalid_mode";
        } else if (json.equals(static_cast<int>(key), "profile_colors")) {
            if (json.at(value).type != Json::Type::Array) return "invalid_profile_colors";
            unsigned count = 0;
            for (unsigned item = value + 1; item < json.at(value).next; item = json.at(item).next) {
                if (count == PROFILE_COUNT || !read_color(json, static_cast<int>(item), body, candidate.profile_colors[count]))
                    return "profile_colors_requires_8_hex_colors";
                ++count;
            }
            if (count != PROFILE_COUNT) return "profile_colors_requires_8_hex_colors";
        } else if (json.equals(static_cast<int>(key), "diagnostic_colors")) {
            if (json.at(value).type != Json::Type::Object) return "invalid_diagnostic_colors";
            for (unsigned name = value + 1; name < json.at(value).next; name = json.at(name + 1).next) {
                unsigned diagnostic = 0;
                for (; diagnostic < DIAGNOSTIC_COUNT; ++diagnostic)
                    if (json.equals(static_cast<int>(name), DIAGNOSTIC_NAMES[diagnostic])) break;
                if (diagnostic == DIAGNOSTIC_COUNT) return "unknown_diagnostic";
                if (!read_color(json, static_cast<int>(name + 1), body, candidate.diagnostic_colors[diagnostic]))
                    return "invalid_diagnostic_color";
            }
        } else return "unknown_field";
    }
    return nullptr;
}

Result change_json(const char* body, std::size_t length, char* output, std::size_t capacity, bool save) {
    Json json;
    if (!json.parse(body, length) || json.at(0).type != Json::Type::Object)
        return error(output, capacity, 400, "invalid_json");
    if (!try_lock()) return error(output, capacity, 503, "led_busy");
    if (save_pending.load(std::memory_order_acquire)) {
        unlock();
        return error(output, capacity, 409, "led_save_pending");
    }
    Settings candidate = requested;
    const auto before_revision = requested_revision;
    unlock();

    const char* issue = parse_settings(json, body, candidate);
    if (issue) return error(output, capacity, 400, issue);
    const auto next_revision = before_revision + 1;
    JsonWriter writer(output, capacity);
    if (save) writer.append("{\"ok\":true,\"revision\":%lu,\"restart_required\":true,\"rebooting\":true}",
                            static_cast<unsigned long>(next_revision));
    else writer.append("{\"ok\":true,\"revision\":%lu,\"applied\":false}", static_cast<unsigned long>(next_revision));
    if (!writer.ok()) return error(output, capacity, 500, "response_too_large");

    if (!try_lock()) return error(output, capacity, 503, "led_busy");
    if (save_pending.load(std::memory_order_acquire) || requested_revision != before_revision) {
        unlock();
        return error(output, capacity, 409, "led_settings_changed_retry");
    }
    requested = candidate;
    requested_revision = next_revision;
    if (save) {
        staged = candidate;
        save_pending.store(true, std::memory_order_release);
    }
    unlock();
    return {202, writer.length(), save};
}
} // namespace

Settings defaults() {
    Settings settings;
    settings.brightness = OGXM_STATUS_LED_BRIGHTNESS;
    settings.profile_colors = {{{0, 128, 255}, {0, 255, 96}, {180, 0, 255}, {255, 110, 0},
                                {0, 220, 255}, {255, 0, 100}, {255, 220, 0}, {255, 255, 255}}};
    settings.diagnostic_colors = {{{180, 180, 180}, {0, 50, 255}, {255, 110, 0}, {255, 160, 0},
                                   {255, 40, 0}, {255, 220, 0}, {255, 140, 0}, {180, 0, 255},
                                   {255, 0, 0}, {255, 255, 255}, {0, 210, 255}, {0, 255, 0}}};
    return settings;
}

const char* mode_name(Mode mode) {
    switch (mode) {
    case Mode::Profiles: return "profiles";
    case Mode::Diagnostics: return "diagnostics";
    default: return "combined";
    }
}

const char* diagnostic_name(Diagnostic diagnostic) {
    const auto index = static_cast<unsigned>(diagnostic);
    return index < DIAGNOSTIC_COUNT ? DIAGNOSTIC_NAMES[index] : "off";
}

std::uint32_t rgb24(Color color) {
    return (static_cast<std::uint32_t>(color.r) << 16) |
           (static_cast<std::uint32_t>(color.g) << 8) | color.b;
}

void initialize() {
    active = defaults();
    Record record{};
    if (NVSTool::get_instance().read(KEY, &record, sizeof(record)) &&
        record.magic == MAGIC && record.version == VERSION && valid(record.settings) &&
        record.checksum == crc32(&record, offsetof(Record, checksum))) active = record.settings;
    requested = persisted = staged = active;
    active_preview = {};
    test_requested = published_test_active = false;
    requested_test_color = {};
    requested_test_duration = 0;
    active_revision = requested_revision = published_revision = 1;
    save_pending.store(false, std::memory_order_relaxed);
    mailbox.store(false, std::memory_order_release);
}

void service(std::uint32_t now_ms) {
    if (active_preview.active && !active_preview.visible(now_ms)) active_preview.active = false;
    if (!try_lock()) return; // Never hold up the USB task for a browser callback.
    if (requested_revision != active_revision) {
        active = requested;
        active_revision = requested_revision;
    }
    if (test_requested) {
        active_preview = {requested_test_color, now_ms, requested_test_duration, true};
        test_requested = false;
    }
    published_revision = active_revision;
    published_test_active = active_preview.active;
    unlock();
}

const Settings& get() { return active; }
const Preview& preview() { return active_preview; }
std::uint32_t applied_revision() { return active_revision; }

Result public_json(char* output, std::size_t capacity) {
    if (!try_lock()) return error(output, capacity, 503, "led_busy");
    const Settings copy = requested;
    const auto revision = requested_revision;
    const auto applied = published_revision;
    const bool dirty = std::memcmp(&requested, &persisted, sizeof(Settings)) != 0;
    const bool saving = save_pending.load(std::memory_order_acquire);
    const bool testing = published_test_active || test_requested;
    unlock();
    JsonWriter writer(output, capacity);
    writer.append("{\"enabled\":%s,\"brightness\":%u,\"mode\":\"%s\",\"profile_colors\":[",
                  copy.enabled ? "true" : "false", static_cast<unsigned>(copy.brightness), mode_name(copy.mode));
    for (unsigned i = 0; i < PROFILE_COUNT; ++i)
        writer.append("%s\"#%06lX\"", i ? "," : "", static_cast<unsigned long>(rgb24(copy.profile_colors[i])));
    writer.append("],\"diagnostic_colors\":{");
    for (unsigned i = 0; i < DIAGNOSTIC_COUNT; ++i)
        writer.append("%s\"%s\":\"#%06lX\"", i ? "," : "", DIAGNOSTIC_NAMES[i],
                      static_cast<unsigned long>(rgb24(copy.diagnostic_colors[i])));
    writer.append("},\"revision\":%lu,\"applied_revision\":%lu,\"dirty\":%s,\"save_pending\":%s,\"test_active\":%s}",
                  static_cast<unsigned long>(revision), static_cast<unsigned long>(applied),
                  dirty ? "true" : "false", saving ? "true" : "false", testing ? "true" : "false");
    if (!writer.ok()) return error(output, capacity, 500, "response_too_large");
    return {200, writer.length(), false};
}

Result apply_json(const char* body, std::size_t length, char* output, std::size_t capacity) {
    return change_json(body, length, output, capacity, false);
}

Result stage_json(const char* body, std::size_t length, char* output, std::size_t capacity) {
    return change_json(body, length, output, capacity, true);
}

Result test_json(const char* body, std::size_t length, char* output, std::size_t capacity) {
    Json json;
    if (!json.parse(body, length) || json.at(0).type != Json::Type::Object)
        return error(output, capacity, 400, "invalid_json");
    for (unsigned key = 1; key < json.at(0).next; key = json.at(key + 1).next)
        if (!json.equals(static_cast<int>(key), "color") && !json.equals(static_cast<int>(key), "duration_ms"))
            return error(output, capacity, 400, "unknown_field");
    Color color;
    if (!read_color(json, json.find(0, "color"), body, color))
        return error(output, capacity, 400, "invalid_color");
    std::int64_t duration = 1500;
    const int duration_token = json.find(0, "duration_ms");
    if (duration_token >= 0 && !json.integer(duration_token, 1, 5000, duration))
        return error(output, capacity, 400, "duration_ms_requires_1_to_5000");
    JsonWriter writer(output, capacity);
    writer.append("{\"ok\":true,\"duration_ms\":%lu}", static_cast<unsigned long>(duration));
    if (!writer.ok()) return error(output, capacity, 500, "response_too_large");
    if (!try_lock()) return error(output, capacity, 503, "led_busy");
    if (save_pending.load(std::memory_order_acquire)) {
        unlock();
        return error(output, capacity, 409, "led_save_pending");
    }
    requested_test_color = color;
    requested_test_duration = static_cast<std::uint32_t>(duration);
    test_requested = true;
    unlock();
    return {202, writer.length(), false};
}

bool pending() { return save_pending.load(std::memory_order_acquire); }
void cancel() { save_pending.store(false, std::memory_order_release); }

bool commit() {
    if (!pending()) return false;
    // The release/acquire save flag makes staged immutable and visible here.
    // Core1 is already stopped by the caller, so do not take a lock that could
    // have belonged to it when it stopped. Reboot follows this operation.
    Record record{};
    record.magic = MAGIC;
    record.version = VERSION;
    record.settings = staged;
    record.checksum = crc32(&record, offsetof(Record, checksum));
    const bool saved = NVSTool::get_instance().write(KEY, &record, sizeof(record));
    if (saved) persisted = staged;
    save_pending.store(false, std::memory_order_release);
    return saved;
}

} // namespace LedSettings
