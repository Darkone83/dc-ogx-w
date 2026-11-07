#pragma once

#include <cstdint>

namespace NetworkService {
enum class SaveAction : std::uint8_t { None = 0, Profile = 1, Network = 2, Firmware = 3, Led = 4, Oled = 5 };
struct Completion {
    SaveAction action = SaveAction::None;
    bool peer_acknowledged = false;
};
struct Health {
    bool ready{};
    bool station_configured{};
    bool station_connected{};
    bool error{};
    bool station_failed{};
    bool station_joining{};
    bool station_retry_pending{};
};
// Start on the radio/core1 side. Pico W must initialize its existing CYW43
// radio first; the ESP transport initializes its UART asynchronously here.
bool start();
// Clone runtime calls poll() on core1. Pico W calls it from its existing
// serialized radio context through the timer installed by start_radio_poll().
void poll();
// Called after Bluepad32 has installed its BTstack run loop; no-op for ESP.
void start_radio_poll();
// Both boards execute bounded OLED I2C work on Core0, outside radio callbacks.
// The radio core only publishes a compact atomic network snapshot.
void service_display_from_core0(std::uint32_t now_ms);
// Core0 only: publish physical connection snapshots.
void note_runtime(bool usb_configured, bool bluetooth_connected, bool wired_connected,
                  bool remote_owned = false);
// Core0 consumes exactly one save completion. On Pico W this requires the
// entire response's peer TCP acknowledgement. ESP only proves SEND OK (modem
// acceptance); its runtime must allow a settling interval before restarting.
bool take_save_acknowledged();
bool take_save_ready();
// Core0 consumes the deferred operation after its entire HTTP reply is sent.
bool take_completion(Completion& completion);
// A recoverable install failure cancels the delayed commit on Core0 before
// the network core allows a new upload. The acknowledgement follows discard.
bool install_cancel_requested();
void acknowledge_install_cancel();
// Atomic snapshot for Core0's status LED; never reads mutable radio state.
Health health();
bool ready();
const char *ssid();
} // namespace NetworkService
