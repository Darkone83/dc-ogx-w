#pragma once

#include <cstdint>
#include "Board/LedSettings.h"

// One external WS2812, default GPIO8. Owned by Core0; no timer, DMA or IRQ is used.
namespace StatusLed {

struct State {
    bool network_ready{};
    bool station_configured{};
    bool station_connected{};
    bool station_failed{};
    bool station_joining{};
    bool station_retry_pending{};
    bool network_error{};
    bool usb_configured{};
    bool control_owned{};
    bool input_fresh{};
    bool bluetooth_connected{};
    bool updating{};
    bool update_error{};
    // The profile actually applied to the gamepad, not a browser editor draft.
    std::uint8_t profile_id{1};
};

enum class Phase : std::uint8_t {
    Off, Starting, AccessPoint, JoiningWifi, ReconnectingWifi, WifiFailed,
    UsbDisconnected, InputPaused, Updating, Error, ProfileChanged,
    WifiConnected, ControlActive, Profile, Test
};

struct Snapshot {
    Phase phase{Phase::Off};
    std::uint8_t profile_id{1};
    std::uint32_t profile_color{}; // Unscaled 0xRRGGBB from the applied settings.
    std::uint32_t output_color{};  // Actual scaled 0xRRGGBB sent to the WS2812.
    std::uint8_t brightness{};
    bool enabled{};
    LedSettings::Mode mode{LedSettings::Mode::Combined};
    std::uint32_t applied_revision{};
};

void initialize();
void tick(std::uint32_t now_ms, const State& state);
// Read-only status can be queried from the network core.
bool available();
const char* state_name();
const char* state_name(Phase phase);
// Read-only atomic pairs keep profile/id and output/phase mutually consistent.
Snapshot snapshot();
int pin();

} // namespace StatusLed
