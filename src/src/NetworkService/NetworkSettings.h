#pragma once

#include <cstddef>
#include <cstdint>

namespace NetworkSettings {

struct Settings {
    char station_ssid[33]{};
    char station_password[65]{};
    char hostname[33] = "dcogxw";
    bool station_enabled = false;
};

struct Result {
    std::uint16_t status;
    std::size_t length;
    bool save_response;
};

// Core0, after UserSettings::initialize_flash() and before launching Core1.
// Missing/corrupt settings use AP-only defaults; no boot-time flash write.
void initialize();
const Settings& get();
// Radio-core JSON API. A successful stage leaves active settings unchanged and
// requests the same response-before-reboot lifecycle used by profile saving.
Result public_json(char* output, std::size_t capacity);
Result stage_json(const char* body, std::size_t length, char* output, std::size_t capacity);
bool pending();
void cancel();
// Core0 ONLY, after Core1 is stopped and USB has been disconnected. Returns
// whether NVS accepted the record; caller handles reboot/status indication.
bool commit();

} // namespace NetworkSettings
