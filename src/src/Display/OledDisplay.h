#pragma once
#include <cstdint>
#include "NetworkService/Transport.h"

// Optional SSD1306 display on RP2040 I2C1: SDA GP14, SCL GP15.
// Hardware and frame buffer are owned exclusively by the network core.
// Core0 only publishes one atomic controller snapshot; it never drives I2C.
namespace OledDisplay {
// Short status messages from successfully accepted configuration operations.
// Events are a one-word atomic mailbox; callers never drive I2C.
enum class Activity : std::uint8_t {
    None = 0,
    ProfileLoad,
    NetworkLoad,
    LedLoad,
    ProfileApply,
    ProfileSave,
    NetworkSave,
    LedApply,
    LedSave,
    OledApply,
    OledSave,
    FirmwareCancel,
};
void notify(Activity event);
void publish_controller(bool usb_connected, bool remote_owned, bool input_fresh,
                        std::uint8_t profile_id, std::uint8_t output_driver);
// Type is a short display category, not a new controller parser or connection owner.
// Core0 publishes it atomically; the OLED has no direct BTstack/USB-host access.
enum class InputKind : std::uint8_t {
    Generic, Xbox, Xbox360, Ps3, Ps4, Ps5, SwitchPro, Wii, EightBitDo, Steam, GameSir,
};
void publish_input_devices(bool bluetooth_connected, InputKind bluetooth_type, bool bluetooth_le,
                           bool wired_connected, InputKind wired_type);
void service(std::uint32_t now_ms, const NetworkService::Transport::Status& network);
bool detected();
} // namespace OledDisplay
