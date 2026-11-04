#pragma once
#include <cstddef>
#include <cstdint>

// Optional SSD1306 display preferences. A single atomic uint32_t transfers the
// complete settings snapshot from the radio/WebUI core to the OLED task; no
// USB-core I2C or flash operations are performed when applying settings.
namespace OledSettings {
struct Settings {
    std::uint8_t brightness{224};
    std::uint8_t idle_brightness{32};
    std::uint8_t dim_index{0};
    std::uint8_t off_index{0};
    bool enabled{true};
    bool invert{false};
    bool flip{false};
    bool fade{true};
};
struct Result {
    std::uint16_t status;
    std::size_t length;
    bool save_response;
};
// Stable selectable timeouts, in seconds; index 0 disables the corresponding timeout.
constexpr std::uint16_t TIMEOUTS_S[] = {0,30,60,120,300,600,900,1800,3600};
Settings defaults();
void initialize(); // Core0, before the radio starts; reads a separate NVS key.
Settings get();    // Core1 OLED service; snapshot is atomic on RP2040.
std::uint32_t packed();
Result public_json(char* output, std::size_t capacity);
Result apply_json(const char* body, std::size_t length, char* output, std::size_t capacity);
Result stage_json(const char* body, std::size_t length, char* output, std::size_t capacity);
bool pending();
void cancel();
bool commit(); // Core0 after Core1 stop, only after the save response completes.
} // namespace OledSettings
