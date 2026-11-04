#include "Display/OledSettings.h"
#include "Display/OledDisplay.h"
#include "RemoteControl/Json.h"
#include "UserSettings/NVSTool.h"
#include <atomic>
#include <cstddef>
#include <cstring>

#ifndef DC_OGX_OLED_ENABLED
#define DC_OGX_OLED_ENABLED 1
#endif
#ifndef DC_OGX_OLED_CONTRAST
#define DC_OGX_OLED_CONTRAST 224
#endif
#ifndef DC_OGX_OLED_IDLE_CONTRAST
#define DC_OGX_OLED_IDLE_CONTRAST 32
#endif
#ifndef DC_OGX_OLED_IDLE_DIM_MS
#define DC_OGX_OLED_IDLE_DIM_MS 0
#endif
#ifndef DC_OGX_OLED_FADE_ENABLED
#define DC_OGX_OLED_FADE_ENABLED 1
#endif

namespace OledSettings {
namespace {
using RemoteControl::Json;
using RemoteControl::JsonWriter;
constexpr std::uint32_t MAGIC = 0x44434f44; // DCOD
constexpr std::uint32_t VERSION = 1;
constexpr char KEY[] = "oled-v1";
struct Record {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t packed;
    std::uint32_t checksum;
};
static_assert(sizeof(Record) <= NVSTool::VALUE_LEN_MAX, "OLED NVS record too large");
std::atomic<std::uint32_t> requested{0};
std::atomic<std::uint32_t> persisted{0};
std::atomic<bool> save_pending{false};
// Stage is immutable after save_pending goes true. Core1 is stopped before commit.
std::uint32_t staged{};

std::uint32_t encode(const Settings& s) {
    return (static_cast<std::uint32_t>(s.brightness)) |
           (static_cast<std::uint32_t>(s.idle_brightness) << 8) |
           (static_cast<std::uint32_t>(s.dim_index & 15) << 16) |
           (static_cast<std::uint32_t>(s.off_index & 15) << 20) |
           (s.enabled ? (1u << 24) : 0u) |
           (s.invert ? (1u << 25) : 0u) |
           (s.flip ? (1u << 26) : 0u) |
           (s.fade ? (1u << 27) : 0u);
}
Settings decode(std::uint32_t packed) {
    Settings s{};
    s.brightness = static_cast<std::uint8_t>(packed);
    s.idle_brightness = static_cast<std::uint8_t>(packed >> 8);
    s.dim_index = static_cast<std::uint8_t>((packed >> 16) & 15u);
    s.off_index = static_cast<std::uint8_t>((packed >> 20) & 15u);
    s.enabled = (packed & (1u << 24)) != 0u;
    s.invert = (packed & (1u << 25)) != 0u;
    s.flip = (packed & (1u << 26)) != 0u;
    s.fade = (packed & (1u << 27)) != 0u;
    return s;
}
bool valid(std::uint32_t value) {
    const auto s = decode(value);
    return (value & 0xf0000000u) == 0u &&
           s.dim_index < sizeof(TIMEOUTS_S)/sizeof(TIMEOUTS_S[0]) &&
           s.off_index < sizeof(TIMEOUTS_S)/sizeof(TIMEOUTS_S[0]);
}
std::uint32_t checksum(const void* memory, std::size_t length) {
    const auto* data = static_cast<const std::uint8_t*>(memory);
    std::uint32_t crc = 0xffffffffu;
    while (length--) {
        crc ^= *data++;
        for (unsigned bit=0;bit<8;++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
Result error(char* output, std::size_t capacity, unsigned status, const char* reason) {
    JsonWriter w(output, capacity);
    w.append("{\"ok\":false,\"error\":\"%s\"}", reason);
    return {static_cast<std::uint16_t>(status),w.length(),false};
}
const char* parse_settings(const Json& json, Settings& s) {
    for (unsigned key=1;key<json.at(0).next;key=json.at(key+1).next) {
        const int val = static_cast<int>(key+1);
        if (json.equals(static_cast<int>(key),"brightness") ||
            json.equals(static_cast<int>(key),"idle_brightness")) {
            std::int64_t n;
            if (!json.integer(val,0,255,n)) return "invalid_brightness";
            if (json.equals(static_cast<int>(key),"brightness")) s.brightness=static_cast<std::uint8_t>(n);
            else s.idle_brightness=static_cast<std::uint8_t>(n);
        } else if (json.equals(static_cast<int>(key),"dim_after_s") ||
                   json.equals(static_cast<int>(key),"off_after_s")) {
            std::int64_t n;
            if (!json.integer(val,0,3600,n)) return "invalid_oled_timeout";
            std::uint8_t i=0;
            for (;i < sizeof(TIMEOUTS_S)/sizeof(TIMEOUTS_S[0]);++i)
                if (TIMEOUTS_S[i]==n) break;
            if (i==sizeof(TIMEOUTS_S)/sizeof(TIMEOUTS_S[0])) return "unsupported_oled_timeout";
            if (json.equals(static_cast<int>(key),"dim_after_s")) s.dim_index=i;
            else s.off_index=i;
        } else if (json.equals(static_cast<int>(key),"enabled") ||
                   json.equals(static_cast<int>(key),"invert") ||
                   json.equals(static_cast<int>(key),"flip") ||
                   json.equals(static_cast<int>(key),"fade")) {
            bool on;
            if (!json.boolean(val,on)) return "invalid_oled_flag";
            if (json.equals(static_cast<int>(key),"enabled")) s.enabled=on;
            else if (json.equals(static_cast<int>(key),"invert")) s.invert=on;
            else if (json.equals(static_cast<int>(key),"flip")) s.flip=on;
            else s.fade=on;
        } else return "unknown_oled_field";
    }
    return nullptr;
}
Result change(const char* body, std::size_t length, char* output, std::size_t capacity, bool save) {
#if !DC_OGX_OLED_ENABLED
    (void)body;(void)length;(void)save;
    return error(output,capacity,409,"oled_not_compiled");
#else
    Json json;
    if (!json.parse(body,length) || json.at(0).type != Json::Type::Object)
        return error(output,capacity,400,"invalid_json");
    if (save_pending.load(std::memory_order_acquire))
        return error(output,capacity,409,"oled_save_pending");
    Settings s=decode(requested.load(std::memory_order_acquire));
    if (const char* issue=parse_settings(json,s)) return error(output,capacity,400,issue);
    const auto bits=encode(s);
    JsonWriter writer(output,capacity);
    if (save)
        writer.append("{\"ok\":true,\"restart_required\":true,\"rebooting\":true}");
    else
        writer.append("{\"ok\":true,\"applied\":true,\"restart_required\":false}");
    if (!writer.ok()) return error(output,capacity,500,"response_too_large");
    if (save) {
        staged=bits;
        save_pending.store(true,std::memory_order_release);
    } else requested.store(bits,std::memory_order_release);
    return {200,writer.length(),save};
#endif
}
} // namespace

Settings defaults() {
    Settings s;
    s.brightness=static_cast<std::uint8_t>(DC_OGX_OLED_CONTRAST);
    s.idle_brightness=static_cast<std::uint8_t>(DC_OGX_OLED_IDLE_CONTRAST);
    s.fade=DC_OGX_OLED_FADE_ENABLED != 0;
    // Existing build-time behavior is honored on first boot before any NVS save.
    // Non-preset legacy timeouts round up to the next selectable duration.
    if (DC_OGX_OLED_IDLE_DIM_MS != 0) {
        const auto seconds=static_cast<unsigned>((DC_OGX_OLED_IDLE_DIM_MS+999u)/1000u);
        for (unsigned i=1;i<sizeof(TIMEOUTS_S)/sizeof(TIMEOUTS_S[0]);++i)
            if (seconds<=TIMEOUTS_S[i]) { s.dim_index=static_cast<std::uint8_t>(i); break; }
        if (!s.dim_index) s.dim_index=static_cast<std::uint8_t>(sizeof(TIMEOUTS_S)/sizeof(TIMEOUTS_S[0])-1);
    }
    return s;
}
void initialize() {
    std::uint32_t bits=encode(defaults());
    Record record{};
    if (NVSTool::get_instance().read(KEY,&record,sizeof(record)) &&
        record.magic==MAGIC && record.version==VERSION && valid(record.packed) &&
        record.checksum==checksum(&record,offsetof(Record,checksum)))
        bits=record.packed;
    requested.store(bits,std::memory_order_release);
    persisted.store(bits,std::memory_order_release);
    staged=bits;
    save_pending.store(false,std::memory_order_release);
}
Settings get() { return decode(requested.load(std::memory_order_acquire)); }
std::uint32_t packed() { return requested.load(std::memory_order_acquire); }
Result public_json(char* output, std::size_t capacity) {
    const auto bits=requested.load(std::memory_order_acquire);
    const Settings s=decode(bits);
    const Settings preset=defaults();
    JsonWriter writer(output,capacity);
    writer.append("{\"ok\":true,\"available\":%s,\"detected\":%s,\"enabled\":%s,\"brightness\":%u,"
                  "\"idle_brightness\":%u,\"dim_after_s\":%u,\"off_after_s\":%u,"
                  "\"invert\":%s,\"flip\":%s,\"fade\":%s,\"dirty\":%s,\"save_pending\":%s,"
                  "\"defaults\":{\"enabled\":true,\"brightness\":%u,\"idle_brightness\":%u,"
                  "\"dim_after_s\":%u,\"off_after_s\":0,\"invert\":false,\"flip\":false,\"fade\":%s}}",
                  DC_OGX_OLED_ENABLED ? "true" : "false", OledDisplay::detected()?"true":"false", s.enabled?"true":"false",
                  static_cast<unsigned>(s.brightness),static_cast<unsigned>(s.idle_brightness),
                  static_cast<unsigned>(TIMEOUTS_S[s.dim_index]),
                  static_cast<unsigned>(TIMEOUTS_S[s.off_index]),
                  s.invert?"true":"false",s.flip?"true":"false",s.fade?"true":"false",
                  bits!=persisted.load(std::memory_order_acquire)?"true":"false",
                  save_pending.load(std::memory_order_acquire)?"true":"false",
                  static_cast<unsigned>(preset.brightness), static_cast<unsigned>(preset.idle_brightness),
                  static_cast<unsigned>(TIMEOUTS_S[preset.dim_index]), preset.fade?"true":"false");
    return writer.ok() ? Result{200,writer.length(),false} : error(output,capacity,500,"response_too_large");
}
Result apply_json(const char* body,std::size_t length,char* output,std::size_t capacity) {
    return change(body,length,output,capacity,false);
}
Result stage_json(const char* body,std::size_t length,char* output,std::size_t capacity) {
    return change(body,length,output,capacity,true);
}
bool pending() { return save_pending.load(std::memory_order_acquire); }
void cancel() { save_pending.store(false,std::memory_order_release); }
bool commit() {
    if (!pending()) return false;
    Record record{MAGIC,VERSION,staged,0};
    record.checksum=checksum(&record,offsetof(Record,checksum));
    const bool ok=NVSTool::get_instance().write(KEY,&record,sizeof(record));
    if (ok) persisted.store(staged,std::memory_order_release);
    save_pending.store(false,std::memory_order_release);
    return ok;
}
} // namespace OledSettings
