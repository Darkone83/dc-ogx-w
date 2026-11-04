#pragma once

#include "Board/StatusLed.h"

namespace StatusLed {

struct Output {
    Phase phase{Phase::Off};
    std::uint8_t profile_id{1};
    LedSettings::Color profile_color{};
    LedSettings::Color color{}; // Already scaled for brightness and pattern.
};

// Portable rendering policy, shared by the real Core0 renderer and native
// regression tests. It owns no GPIO, timer, interrupt, dynamic memory or flash.
class Policy {
public:
    Output render(std::uint32_t now_ms, const State& state,
                  const LedSettings::Settings& settings, const LedSettings::Preview& preview);
private:
    bool initialized_{};
    bool station_connected_before_{};
    bool profile_changed_{};
    std::uint8_t profile_id_{1};
    std::uint32_t boot_ms_{};
    std::uint32_t profile_changed_ms_{};
};

} // namespace StatusLed
