#pragma once

#include <cstdint>

// A separate, versioned NVS record preserves the existing 190-byte UserProfile
// and all older settings. Values use normalized Bluetooth input BEFORE profile
// curves, deadzones and mappings: X right, Y up, triggers 0..255.
struct ControllerSample {
    int16_t axis[4]{}; // lx, ly, rx, ry
    uint8_t trigger[2]{}; // lt, rt
    uint16_t buttons{};
    uint8_t dpad{};
};

#pragma pack(push, 1)
struct ControllerCalibration {
    struct Axis { int16_t minimum{-32768}, center{}, maximum{32767}; };
    struct Trigger { uint8_t minimum{}, maximum{255}; };
    uint8_t version{1};
    uint8_t mask{}; // lx, ly, rx, ry, lt, rt; absent controls are identity.
    Axis axis[4]{};
    Trigger trigger[2]{};

    bool valid() const {
        if (version != 1 || mask > 63) return false;
        for (unsigned i = 0; i < 4; ++i) {
            if (!(mask & (1u << i))) continue;
            if (int32_t(axis[i].center) - axis[i].minimum < 4096 ||
                int32_t(axis[i].maximum) - axis[i].center < 4096) return false;
        }
        for (unsigned i = 0; i < 2; ++i)
            if ((mask & (16u << i)) && int(trigger[i].maximum) - trigger[i].minimum < 32)
                return false;
        return true;
    }

    void apply(ControllerSample& sample) const {
        for (unsigned i = 0; i < 4; ++i) {
            if (!(mask & (1u << i))) continue;
            const auto& a = axis[i];
            const int32_t delta = int32_t(sample.axis[i]) - a.center;
            // Bounds and denominators are checked before this record is used.
            const int32_t span = delta < 0 ? int32_t(a.center) - a.minimum
                                          : int32_t(a.maximum) - a.center;
            int32_t value = span > 0 ? delta * (delta < 0 ? 32768 : 32767) / span : 0;
            if (value < -32768) value = -32768;
            if (value > 32767) value = 32767;
            sample.axis[i] = static_cast<int16_t>(value);
        }
        for (unsigned i = 0; i < 2; ++i) {
            if (!(mask & (16u << i))) continue;
            const auto& t = trigger[i];
            const int span = int(t.maximum) - t.minimum;
            int value = span > 0 ? (int(sample.trigger[i]) - t.minimum) * 255 / span : 0;
            if (value < 0) value = 0;
            if (value > 255) value = 255;
            sample.trigger[i] = static_cast<uint8_t>(value);
        }
    }
};
#pragma pack(pop)
static_assert(sizeof(ControllerCalibration) == 30, "Calibration record layout changed");
