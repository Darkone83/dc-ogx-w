#include "RemoteControl/Profile.h"

#include <cmath>
#include <cstddef>
#include <cstring>

namespace RemoteControl {
namespace {

enum class Kind : uint8_t { Fixed, Byte, Word, Boolean };
struct Field {
    const char* name;
    size_t offset;
    Kind kind;
    double minimum;
    double maximum;
};
#define F(type, name, lo, hi) {#name, offsetof(type, name), Kind::Fixed, lo, hi}
#define B(type, name, lo, hi) {#name, offsetof(type, name), Kind::Byte, lo, hi}
#define W(type, name, lo, hi) {#name, offsetof(type, name), Kind::Word, lo, hi}
#define FLAG(type, name) {#name, offsetof(type, name), Kind::Boolean, 0, 1}

const Field stick_fields[] = {
    F(JoystickSettingsRaw, dz_inner, 0, .95),
    F(JoystickSettingsRaw, dz_outer, .01, 1),
    F(JoystickSettingsRaw, anti_dz_circle, 0, .95),
    F(JoystickSettingsRaw, anti_dz_circle_y_scale, 0, 1),
    F(JoystickSettingsRaw, anti_dz_square, 0, .95),
    F(JoystickSettingsRaw, anti_dz_square_y_scale, 0, .95),
    F(JoystickSettingsRaw, anti_dz_angular, 0, .89),
    F(JoystickSettingsRaw, anti_dz_outer, .01, 1),
    // These three retain the upstream packed-profile units. Gamepad multiplies
    // restriction values by 100; axis_restrict must stay below 1 afterwards.
    F(JoystickSettingsRaw, axis_restrict, 0, .0095),
    F(JoystickSettingsRaw, angle_restrict, 0, .89),
    F(JoystickSettingsRaw, diag_scale_min, 0, 2),
    F(JoystickSettingsRaw, diag_scale_max, 0, 2),
    F(JoystickSettingsRaw, curve, .05, 4),
    FLAG(JoystickSettingsRaw, uncap_radius),
    FLAG(JoystickSettingsRaw, invert_y),
    FLAG(JoystickSettingsRaw, invert_x),
};
const Field trigger_fields[] = {
    F(TriggerSettingsRaw, dz_inner, 0, .95),
    F(TriggerSettingsRaw, dz_outer, .01, 1),
    F(TriggerSettingsRaw, anti_dz_inner, 0, .95),
    F(TriggerSettingsRaw, anti_dz_outer, .01, 1),
    F(TriggerSettingsRaw, curve, .05, 4),
};
const Field profile_fields[] = {
    B(UserProfile, id, 1, 8),
    B(UserProfile, dpad_up, 0, 15), B(UserProfile, dpad_down, 0, 15),
    B(UserProfile, dpad_left, 0, 15), B(UserProfile, dpad_right, 0, 15),
    W(UserProfile, button_a, 0, 4095), W(UserProfile, button_b, 0, 4095),
    W(UserProfile, button_x, 0, 4095), W(UserProfile, button_y, 0, 4095),
    W(UserProfile, button_l3, 0, 4095), W(UserProfile, button_r3, 0, 4095),
    W(UserProfile, button_back, 0, 4095), W(UserProfile, button_start, 0, 4095),
    W(UserProfile, button_lb, 0, 4095), W(UserProfile, button_rb, 0, 4095),
    W(UserProfile, button_sys, 0, 4095), W(UserProfile, button_misc, 0, 4095),
    FLAG(UserProfile, analog_enabled),
    B(UserProfile, analog_off_up, 0, 9), B(UserProfile, analog_off_down, 0, 9),
    B(UserProfile, analog_off_left, 0, 9), B(UserProfile, analog_off_right, 0, 9),
    B(UserProfile, analog_off_a, 0, 9), B(UserProfile, analog_off_b, 0, 9),
    B(UserProfile, analog_off_x, 0, 9), B(UserProfile, analog_off_y, 0, 9),
    B(UserProfile, analog_off_lb, 0, 9), B(UserProfile, analog_off_rb, 0, 9),
};
#undef F
#undef B
#undef W
#undef FLAG

double read_field(const void* data, const Field& field) {
    const auto* at = static_cast<const uint8_t*>(data) + field.offset;
    if (field.kind == Kind::Fixed) {
        int32_t fixed;
        std::memcpy(&fixed, at, sizeof(fixed));
        return static_cast<double>(fixed) / 65536.0;
    }
    if (field.kind == Kind::Word) {
        uint16_t word;
        std::memcpy(&word, at, sizeof(word));
        return word;
    }
    return *at;
}

bool write_field(const Json& json, int token, void* data, const Field& field) {
    auto* at = static_cast<uint8_t*>(data) + field.offset;
    if (field.kind == Kind::Boolean) {
        bool value;
        if (!json.boolean(token, value)) return false;
        *at = value ? 1 : 0;
        return true;
    }
    double value;
    const double rounding = field.kind == Kind::Fixed ? .5 / 65536.0 : 0;
    if (!json.number(token, value) || value < field.minimum - rounding || value > field.maximum + rounding) return false;
    if (field.kind == Kind::Fixed) {
        const int32_t fixed = static_cast<int32_t>(std::llround(value * 65536.0));
        std::memcpy(at, &fixed, sizeof(fixed));
        return true;
    }
    if (std::floor(value) != value) return false;
    if (field.kind == Kind::Word) {
        const uint16_t word = static_cast<uint16_t>(value);
        std::memcpy(at, &word, sizeof(word));
    } else *at = static_cast<uint8_t>(value);
    return true;
}

template <size_t N>
bool apply_fields(const Json& json, int object, void* data, const Field (&fields)[N], const char*& error) {
    if (!json.valid(object) || json.at(object).type != Json::Type::Object) {
        error = "profile_group_must_be_object";
        return false;
    }
    for (unsigned key = object + 1; key < json.at(object).next; key = json.at(key + 1).next) {
        bool found = false;
        for (const Field& field : fields) {
            if (!json.equals(key, field.name)) continue;
            found = true;
            if (!write_field(json, static_cast<int>(key + 1), data, field)) {
                error = field.name;
                return false;
            }
            break;
        }
        if (!found) { error = "unknown_profile_field"; return false; }
    }
    return true;
}

template <size_t N>
bool check_fields(const void* data, const Field (&fields)[N], const char*& error) {
    for (const Field& field : fields) {
        const double value = read_field(data, field);
        // Fixed-point values are quantized to 1/65536; accept rounded bounds.
        const double rounding = field.kind == Kind::Fixed ? .5 / 65536.0 : 0;
        if (value < field.minimum - rounding || value > field.maximum + rounding) {
            error = field.name;
            return false;
        }
    }
    return true;
}

template <size_t N>
void emit_fields(JsonWriter& writer, const void* data, const Field (&fields)[N], bool leading_comma = false) {
    bool comma = leading_comma;
    for (const Field& field : fields) {
        writer.append("%s\"%s\":", comma ? "," : "", field.name);
        const double value = read_field(data, field);
        if (field.kind == Kind::Boolean) writer.append("%s", value != 0 ? "true" : "false");
        else if (field.kind == Kind::Fixed) writer.append("%.8g", value);
        else writer.append("%u", static_cast<unsigned>(value));
        comma = true;
    }
}

bool check_stick(const JoystickSettingsRaw& stick, const char*& error) {
    if (!check_fields(&stick, stick_fields, error)) return false;
    if (stick.dz_inner >= stick.dz_outer || stick.dz_inner >= stick.anti_dz_outer) {
        error = "stick_inner_must_be_below_outer";
        return false;
    }
    if (stick.anti_dz_circle >= stick.dz_outer || stick.anti_dz_square >= stick.dz_outer ||
        stick.anti_dz_square_y_scale >= stick.dz_outer) {
        error = "stick_antideadzone_must_be_below_outer";
        return false;
    }
    if (stick.anti_dz_circle > 0) {
        const double outer = static_cast<double>(stick.dz_outer) / 65536.0;
        const double square = static_cast<double>(stick.anti_dz_square) / 65536.0;
        double circle = static_cast<double>(stick.anti_dz_circle) / 65536.0;
        const double y_scale = static_cast<double>(stick.anti_dz_circle_y_scale) / 65536.0;
        if (y_scale > circle) circle = y_scale;
        // The legacy curve compensates circle anti-DZ for square shrinkage.
        // Validate the compensated maximum before its later division.
        if (circle * (1.0 - square) >= outer - square) {
            error = "combined_antideadzone_exceeds_outer";
            return false;
        }
    }
    return true;
}

bool check_trigger(const TriggerSettingsRaw& trigger, const char*& error) {
    if (!check_fields(&trigger, trigger_fields, error)) return false;
    if (trigger.dz_inner >= trigger.dz_outer || trigger.dz_inner >= trigger.anti_dz_outer) {
        error = "trigger_inner_must_be_below_outer";
        return false;
    }
    return true;
}

} // namespace

bool validate_profile(const UserProfile& profile, const char*& error) {
    if (!check_fields(&profile, profile_fields, error) ||
        !check_stick(profile.joystick_settings_l, error) ||
        !check_stick(profile.joystick_settings_r, error) ||
        !check_trigger(profile.trigger_settings_l, error) ||
        !check_trigger(profile.trigger_settings_r, error)) return false;
    const uint8_t dpad[] = {profile.dpad_up, profile.dpad_down, profile.dpad_left, profile.dpad_right};
    for (uint8_t value : dpad) {
        if ((value & 3) == 3 || (value & 12) == 12) {
            error = "opposing_dpad_mapping";
            return false;
        }
    }
    return true;
}

bool patch_profile(const Json& json, const UserProfile& current,
                   UserProfile& candidate, const char*& error) {
    if (!json.count() || json.at(0).type != Json::Type::Object) { error = "body_must_be_object"; return false; }
    const int profile = json.find(0, "profile");
    if (profile < 0 || json.at(profile).type != Json::Type::Object) { error = "missing_profile_object"; return false; }
    for (unsigned key = 1; key < json.at(0).next; key = json.at(key + 1).next) {
        if (!json.equals(key, "profile") && !json.equals(key, "revision")) {
            error = "unknown_request_field";
            return false;
        }
    }
    candidate = current;
    for (unsigned key = profile + 1; key < json.at(profile).next; key = json.at(key + 1).next) {
        const int value = static_cast<int>(key + 1);
        if (json.equals(key, "joystick_settings_l")) {
            if (!apply_fields(json, value, &candidate.joystick_settings_l, stick_fields, error)) return false;
        } else if (json.equals(key, "joystick_settings_r")) {
            if (!apply_fields(json, value, &candidate.joystick_settings_r, stick_fields, error)) return false;
        } else if (json.equals(key, "trigger_settings_l")) {
            if (!apply_fields(json, value, &candidate.trigger_settings_l, trigger_fields, error)) return false;
        } else if (json.equals(key, "trigger_settings_r")) {
            if (!apply_fields(json, value, &candidate.trigger_settings_r, trigger_fields, error)) return false;
        } else {
            bool found = false;
            for (const Field& field : profile_fields) {
                if (!json.equals(key, field.name)) continue;
                found = true;
                if (!write_field(json, value, &candidate, field)) { error = field.name; return false; }
                break;
            }
            if (!found) { error = "unknown_profile_field"; return false; }
        }
    }
    return validate_profile(candidate, error);
}

void write_profile(JsonWriter& writer, const UserProfile& profile) {
    writer.append("{");
    emit_fields(writer, &profile, profile_fields);
    writer.append(",\"joystick_settings_l\":{");
    emit_fields(writer, &profile.joystick_settings_l, stick_fields);
    writer.append("},\"joystick_settings_r\":{");
    emit_fields(writer, &profile.joystick_settings_r, stick_fields);
    writer.append("},\"trigger_settings_l\":{");
    emit_fields(writer, &profile.trigger_settings_l, trigger_fields);
    writer.append("},\"trigger_settings_r\":{");
    emit_fields(writer, &profile.trigger_settings_r, trigger_fields);
    writer.append("}}");
}

} // namespace RemoteControl
