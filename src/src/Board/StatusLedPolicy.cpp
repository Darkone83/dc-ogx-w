#include "Board/StatusLedPolicy.h"

namespace StatusLed {
namespace {
using LedSettings::Color;
using LedSettings::Diagnostic;
using LedSettings::Mode;

Color scale(Color color, unsigned level) {
    return {static_cast<std::uint8_t>(static_cast<unsigned>(color.r) * level / 255),
            static_cast<std::uint8_t>(static_cast<unsigned>(color.g) * level / 255),
            static_cast<std::uint8_t>(static_cast<unsigned>(color.b) * level / 255)};
}

// The first twelve diagnostic phases intentionally have the same order as the
// persisted diagnostic colour array. Off precedes them in the public phase enum.
Diagnostic diagnostic(Phase phase) {
    return static_cast<Diagnostic>(static_cast<unsigned>(phase) - 1);
}

unsigned pattern_level(Phase phase, std::uint32_t now_ms, std::uint32_t changed_ms) {
    switch (phase) {
    case Phase::Off: return 0;
    case Phase::Starting: return now_ms % 1200 < 400 ? 255 : 0;
    case Phase::JoiningWifi: return now_ms % 500 < 250 ? 255 : 0;
    case Phase::ReconnectingWifi: {
        const auto time = now_ms % 1200;
        return (time < 120 || (time >= 240 && time < 360)) ? 255 : 0;
    }
    case Phase::WifiFailed: {
        const auto time = now_ms % 1600;
        return time < 750 && time % 300 < 150 ? 255 : 0;
    }
    case Phase::UsbDisconnected: return now_ms % 2000 < 120 ? 255 : 0;
    case Phase::InputPaused: return now_ms % 1000 < 500 ? 255 : 0;
    case Phase::Updating: {
        const auto time = now_ms % 1000;
        return 64 + (time < 500 ? time : 1000 - time) * 191 / 500;
    }
    case Phase::Error: return now_ms % 200 < 100 ? 255 : 0;
    case Phase::ProfileChanged:
        return static_cast<std::uint32_t>(now_ms - changed_ms) % 200 < 100 ? 255 : 0;
    default: return 255;
    }
}
} // namespace

Output Policy::render(std::uint32_t now_ms, const State& state,
                      const LedSettings::Settings& settings, const LedSettings::Preview& preview) {
    Output out;
    out.profile_id = state.profile_id >= 1 && state.profile_id <= LedSettings::PROFILE_COUNT ? state.profile_id : 1;
    out.profile_color = settings.profile_colors[out.profile_id - 1];
    if (!initialized_) {
        initialized_ = true;
        boot_ms_ = now_ms;
        profile_id_ = out.profile_id;
    }
    if (profile_id_ != out.profile_id) {
        profile_id_ = out.profile_id;
        profile_changed_ms_ = now_ms;
        profile_changed_ = true;
    }
    if (profile_changed_ && static_cast<std::uint32_t>(now_ms - profile_changed_ms_) >= 600)
        profile_changed_ = false;
    if (!state.station_configured) station_connected_before_ = false;
    else if (state.station_connected) station_connected_before_ = true;

    const bool critical = state.updating || state.update_error;
    if (preview.visible(now_ms) && !critical) out.phase = Phase::Test;
    else if (!settings.enabled) out.phase = Phase::Off;
    else if (state.update_error) out.phase = Phase::Error;
    else if (state.updating) out.phase = Phase::Updating;
    else if (settings.mode == Mode::Profiles)
        out.phase = profile_changed_ ? Phase::ProfileChanged : Phase::Profile;
    else if (static_cast<std::uint32_t>(now_ms - boot_ms_) < 800) out.phase = Phase::Starting;
    // A transport may retain a past per-client send error while its AP/server
    // is healthy. That historical diagnostic must not permanently hide the
    // profile. A failed station join has its own explicit, live failure flag.
    else if (state.station_failed || (state.network_error && !state.network_ready)) out.phase = Phase::WifiFailed;
    else if (state.station_configured && !state.station_connected) {
        out.phase = station_connected_before_ || state.station_retry_pending ?
                    Phase::ReconnectingWifi : Phase::JoiningWifi;
    } else if (state.station_joining) out.phase = Phase::JoiningWifi;
    else if (!state.network_ready) out.phase = Phase::Starting;
    else if (!state.usb_configured) out.phase = Phase::UsbDisconnected;
    else if (state.control_owned && !state.input_fresh) out.phase = Phase::InputPaused;
    else if (profile_changed_) out.phase = Phase::ProfileChanged;
    else if (settings.mode == Mode::Combined) out.phase = Phase::Profile;
    else if ((state.control_owned && state.input_fresh) || state.bluetooth_connected) out.phase = Phase::ControlActive;
    else if (state.station_connected) out.phase = Phase::WifiConnected;
    else out.phase = Phase::AccessPoint;

    Color source{};
    if (out.phase == Phase::Test) source = preview.color;
    else if (out.phase == Phase::Profile ||
             (out.phase == Phase::ProfileChanged && settings.mode == Mode::Profiles)) source = out.profile_color;
    else if (out.phase != Phase::Off)
        source = settings.diagnostic_colors[static_cast<unsigned>(diagnostic(out.phase))];
    const auto pattern = pattern_level(out.phase, now_ms, profile_changed_ms_);
    // Calculate a single combined level to avoid a second rounding loss on each
    // RGB channel when brightness is deliberately low for a nearby status LED.
    const auto level = static_cast<unsigned>(settings.brightness) * pattern / 255;
    out.color = scale(source, level);
    return out;
}

const char* state_name(Phase phase) {
    if (phase == Phase::Profile) return "profile";
    if (phase == Phase::Test) return "test";
    if (phase == Phase::Off) return "off";
    return LedSettings::diagnostic_name(diagnostic(phase));
}

} // namespace StatusLed
