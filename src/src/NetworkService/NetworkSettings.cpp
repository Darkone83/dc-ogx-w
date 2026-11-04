#include "NetworkService/NetworkSettings.h"
#include "RemoteControl/Json.h"
#include "UserSettings/NVSTool.h"

#include <atomic>
#include <cstring>

namespace NetworkSettings {
namespace {
using RemoteControl::Json;
using RemoteControl::JsonWriter;
constexpr std::uint32_t MAGIC = 0x574e4344; // DCNW
constexpr std::uint32_t VERSION = 1;
constexpr char KEY[] = "network-v1";
struct Record {
    std::uint32_t magic;
    std::uint32_t version;
    Settings settings;
    std::uint32_t checksum;
};
static_assert(sizeof(Record) <= NVSTool::VALUE_LEN_MAX);
Settings active;
Settings staged;
std::atomic<bool> save_pending{false};

std::uint32_t crc32(const void* bytes, std::size_t length) {
    auto* p = static_cast<const std::uint8_t*>(bytes);
    std::uint32_t crc = 0xffffffff;
    while (length--) {
        crc ^= *p++;
        for (unsigned i = 0; i != 8; ++i)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

bool bounded_text(const char* text, std::size_t capacity, bool ascii_only) {
    for (std::size_t i = 0; i != capacity; ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (!c) return true;
        if (c < 32 || c == 127 || (ascii_only && c > 126)) return false;
    }
    return false;
}

bool valid_utf8(const char* text) {
    const auto* p = reinterpret_cast<const unsigned char*>(text);
    while (*p) {
        const unsigned first = *p++;
        if (first < 0x80) continue;
        unsigned remaining, code, minimum;
        if (first >= 0xc2 && first <= 0xdf) { remaining = 1; code = first & 31; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { remaining = 2; code = first & 15; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { remaining = 3; code = first & 7; minimum = 0x10000; }
        else return false;
        while (remaining--) {
            if ((*p & 0xc0) != 0x80) return false;
            code = (code << 6) | (*p++ & 63);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
    }
    return true;
}

bool valid_hostname(const char* host) {
    if (!bounded_text(host, 33, true)) return false;
    const auto length = std::strlen(host);
    if (!length || host[0] == '-' || host[length - 1] == '-') return false;
    for (std::size_t i = 0; i != length; ++i) {
        const char c = host[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    }
    return true;
}

bool valid_password(const char* password) {
    if (!bounded_text(password, 65, true)) return false;
    const auto length = std::strlen(password);
    if (!length || (length >= 8 && length <= 63)) return true;
    if (length != 64) return false;
    for (std::size_t i = 0; i != length; ++i) {
        const char c = password[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

bool valid(const Settings& settings) {
    return bounded_text(settings.station_ssid, sizeof(settings.station_ssid), false) &&
        valid_utf8(settings.station_ssid) &&
        (!settings.station_enabled || settings.station_ssid[0]) &&
        valid_password(settings.station_password) && valid_hostname(settings.hostname);
}

int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10; // Json parser has already checked the escape digits.
}

// Decode JSON escapes before validating byte limits/AT quoting. Reject control
// characters, NUL and malformed UTF-16; allow UTF-8 SSIDs within their 32 bytes.
bool read_string(const Json& json, int field, const char* body, char* output, std::size_t capacity) {
    if (!json.valid(field) || json.at(field).type != Json::Type::String) return false;
    auto i = json.at(field).begin;
    const auto end = json.at(field).end;
    std::size_t used = 0;
    auto append = [&](unsigned c) {
        if (used + 1 >= capacity || c < 32 || c == 127) return false;
        output[used++] = static_cast<char>(c);
        return true;
    };
    while (i < end) {
        unsigned c = static_cast<unsigned char>(body[i++]);
        if (c == '\\') {
            c = static_cast<unsigned char>(body[i++]);
            if (c == 'u') {
                unsigned code = 0;
                for (unsigned n = 0; n != 4; ++n) code = code * 16 + hex(body[i++]);
                if (code >= 0xd800 && code <= 0xdbff) {
                    if (end - i < 6 || body[i] != '\\' || body[i + 1] != 'u') return false;
                    i += 2;
                    unsigned low = 0;
                    for (unsigned n = 0; n != 4; ++n) low = low * 16 + hex(body[i++]);
                    if (low < 0xdc00 || low > 0xdfff) return false;
                    code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
                } else if (code >= 0xdc00 && code <= 0xdfff) return false;
                if (code < 0x80) { if (!append(code)) return false; }
                else if (code < 0x800) {
                    if (!append(0xc0 | (code >> 6)) || !append(0x80 | (code & 63))) return false;
                } else if (code < 0x10000) {
                    if (!append(0xe0 | (code >> 12)) || !append(0x80 | ((code >> 6) & 63)) ||
                        !append(0x80 | (code & 63))) return false;
                } else if (!append(0xf0 | (code >> 18)) || !append(0x80 | ((code >> 12) & 63)) ||
                           !append(0x80 | ((code >> 6) & 63)) || !append(0x80 | (code & 63))) return false;
                continue;
            }
            if (c != '"' && c != '\\' && c != '/') return false;
        }
        if (!append(c)) return false;
    }
    output[used] = '\0';
    return true;
}

void quoted(JsonWriter& writer, const char* value) {
    writer.append("\"");
    for (const auto* p = value; *p; ++p) {
        if (*p == '"' || *p == '\\') writer.append("\\%c", *p);
        else writer.append("%c", *p);
    }
    writer.append("\"");
}

Result error(char* output, std::size_t capacity, std::uint16_t status, const char* code) {
    JsonWriter writer(output, capacity);
    writer.append("{\"ok\":false,\"error\":\"%s\"}", code);
    return {status, writer.ok() ? writer.length() : 0, false};
}
} // namespace

void initialize() {
    active = Settings{};
    save_pending.store(false, std::memory_order_relaxed);
    Record record{};
    if (NVSTool::get_instance().read(KEY, &record, sizeof(record)) &&
        record.magic == MAGIC && record.version == VERSION &&
        record.checksum == crc32(&record, offsetof(Record, checksum)) && valid(record.settings))
        active = record.settings;
}

const Settings& get() { return active; }
bool pending() { return save_pending.load(std::memory_order_acquire); }
void cancel() { save_pending.store(false, std::memory_order_release); }

Result public_json(char* output, std::size_t capacity) {
    JsonWriter writer(output, capacity);
    writer.append("{\"ok\":true,\"station_enabled\":%s,\"station_ssid\":", active.station_enabled ? "true" : "false");
    quoted(writer, active.station_ssid);
    writer.append(",\"hostname\":");
    quoted(writer, active.hostname);
    writer.append(",\"password_saved\":%s,\"pending\":%s}",
        active.station_password[0] ? "true" : "false", pending() ? "true" : "false");
    if (!writer.ok()) return error(output, capacity, 500, "response_too_large");
    return {200, writer.length(), false};
}

Result stage_json(const char* body, std::size_t length, char* output, std::size_t capacity) {
    if (pending()) return error(output, capacity, 409, "network_save_pending");
    Json json;
    if (!json.parse(body, length) || json.at(0).type != Json::Type::Object)
        return error(output, capacity, 400, "invalid_json_object");
    for (unsigned i = 1; i < json.at(0).next; i = json.at(i + 1).next) {
        if (!json.equals(i, "station_enabled") && !json.equals(i, "station_ssid") &&
            !json.equals(i, "station_password") && !json.equals(i, "hostname") && !json.equals(i, "forget"))
            return error(output, capacity, 400, "unknown_network_setting");
    }
    Settings candidate = active;
    const int enabled = json.find(0, "station_enabled");
    if (enabled >= 0 && !json.boolean(enabled, candidate.station_enabled))
        return error(output, capacity, 400, "invalid_station_enabled");
    const int ssid = json.find(0, "station_ssid");
    if (ssid >= 0 && !read_string(json, ssid, body, candidate.station_ssid, sizeof(candidate.station_ssid)))
        return error(output, capacity, 400, "invalid_station_ssid");
    const int password = json.find(0, "station_password");
    if (password >= 0 && !read_string(json, password, body, candidate.station_password, sizeof(candidate.station_password)))
        return error(output, capacity, 400, "invalid_station_password");
    const int hostname = json.find(0, "hostname");
    if (hostname >= 0 && !read_string(json, hostname, body, candidate.hostname, sizeof(candidate.hostname)))
        return error(output, capacity, 400, "invalid_hostname");
    for (char* p = candidate.hostname; *p; ++p)
        if (*p >= 'A' && *p <= 'Z') *p = static_cast<char>(*p - 'A' + 'a');
    bool forget = false;
    const int forget_field = json.find(0, "forget");
    if (forget_field >= 0 && !json.boolean(forget_field, forget))
        return error(output, capacity, 400, "invalid_forget");
    if (forget) {
        candidate.station_enabled = false;
        std::memset(candidate.station_ssid, 0, sizeof(candidate.station_ssid));
        std::memset(candidate.station_password, 0, sizeof(candidate.station_password));
    }
    if (!valid_hostname(candidate.hostname)) return error(output, capacity, 400, "invalid_hostname");
    if (candidate.station_enabled && !candidate.station_ssid[0])
        return error(output, capacity, 400, "station_ssid_required");
    if (!valid_utf8(candidate.station_ssid)) return error(output, capacity, 400, "invalid_station_ssid");
    if (!valid_password(candidate.station_password))
        return error(output, capacity, 400, "password_requires_8_to_63_ascii_or_64_hex_or_empty");
    JsonWriter writer(output, capacity);
    writer.append("{\"ok\":true,\"rebooting\":true,\"hostname\":\"%s.local\",\"recovery_ip\":\"192.168.4.1\"}", candidate.hostname);
    if (!writer.ok()) return error(output, capacity, 500, "response_too_large");
    staged = candidate;
    save_pending.store(true, std::memory_order_release);
    return {200, writer.length(), true};
}

bool commit() {
    if (!pending()) return false;
    Record record{};
    record.magic = MAGIC;
    record.version = VERSION;
    record.settings = staged;
    record.checksum = crc32(&record, offsetof(Record, checksum));
    const bool saved = NVSTool::get_instance().write(KEY, &record, sizeof(record));
    save_pending.store(false, std::memory_order_release);
    return saved;
}

} // namespace NetworkSettings
