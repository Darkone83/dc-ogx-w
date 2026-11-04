#pragma once
#include <cstdint>

namespace DeviceServices {
// Core0, after persistent user settings have been initialized.
void initialize();
// Core0 service visit before sending the next USB controller report.
void tick(std::uint32_t now_ms, bool usb_configured, bool bluetooth_connected);
} // namespace DeviceServices
