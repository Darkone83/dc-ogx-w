#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// LED settings have their own small NVS record. Live changes and tests cross a
// nonblocking mailbox; the radio core never touches PIO or flash.
namespace LedSettings {

constexpr std::size_t PROFILE_COUNT = 8;
enum class Mode : std::uint8_t { Combined, Profiles, Diagnostics };
enum class Diagnostic : std::uint8_t {
    Starting, AccessPoint, JoiningWifi, ReconnectingWifi, WifiFailed,
    UsbDisconnected, InputPaused, Updating, Error, ProfileChanged,
    WifiConnected, ControlActive, Count
};
constexpr std::size_t DIAGNOSTIC_COUNT = static_cast<std::size_t>(Diagnostic::Count);

struct Color {
    std::uint8_t r{}, g{}, b{};
};

struct Settings {
    std::uint8_t enabled{1};
    std::uint8_t brightness{24};
    Mode mode{Mode::Combined};
    std::uint8_t reserved{};
    std::array<Color, PROFILE_COUNT> profile_colors{};
    std::array<Color, DIAGNOSTIC_COUNT> diagnostic_colors{};
};

struct Preview {
    Color color{};
    std::uint32_t started_ms{};
    std::uint32_t duration_ms{};
    bool active{};
    bool visible(std::uint32_t now_ms) const {
        return active && static_cast<std::uint32_t>(now_ms - started_ms) < duration_ms;
    }
};

struct Result {
    std::uint16_t status;
    std::size_t length;
    bool save_response;
};

Settings defaults();
const char* mode_name(Mode mode);
const char* diagnostic_name(Diagnostic diagnostic);
std::uint32_t rgb24(Color color);

// Core0, before launching Core1. Loading missing/corrupt settings never writes
// flash. StatusLed::initialize() calls this once during device initialization.
void initialize();
// Core0 only: consume queued settings/tests and expire a bounded test. The
// returned references are owned by Core0 and must never be read by Core1.
void service(std::uint32_t now_ms);
const Settings& get();
const Preview& preview();
std::uint32_t applied_revision();

// Radio-core HTTP handlers. Serialization copies a snapshot under try-lock,
// then formats it after releasing the mailbox. No handler calls PIO or NVS.
Result public_json(char* output, std::size_t capacity);
Result apply_json(const char* body, std::size_t length, char* output, std::size_t capacity);
Result stage_json(const char* body, std::size_t length, char* output, std::size_t capacity);
Result test_json(const char* body, std::size_t length, char* output, std::size_t capacity);
bool pending();
void cancel();
// Core0 ONLY after Core1 has stopped and USB has disconnected. The caller must
// reboot after commit(), including failure, as with the other saved settings.
bool commit();

} // namespace LedSettings
