#include "Board/StatusLed.h"
#include "Board/StatusLedPolicy.h"

#include <atomic>
#include <initializer_list>
#include "hardware/pio.h"
#include "WS2812.pio.h"

#ifndef OGXM_STATUS_LED_PIN
#define OGXM_STATUS_LED_PIN 8
#endif
#ifndef OGXM_STATUS_LED_ENABLED
#define OGXM_STATUS_LED_ENABLED 1
#endif

namespace StatusLed {
namespace {
std::atomic<bool> initialized{false};
// Each related pair is published with one atomic write, so an HTTP read cannot
// associate the new profile id with the previous profile's configured colour.
std::atomic<std::uint32_t> profile_snapshot{1u << 24};
std::atomic<std::uint32_t> output_snapshot{};
std::atomic<std::uint32_t> config_snapshot{};
std::atomic<std::uint32_t> revision_snapshot{};
Policy policy;
PIO led_pio{};
int led_sm = -1;
std::uint32_t last_color = 0xffffffffu;
std::uint32_t last_tick{};

void publish(const Output& output) {
    const auto& settings = LedSettings::get();
    profile_snapshot.store((static_cast<std::uint32_t>(output.profile_id) << 24) |
                           LedSettings::rgb24(output.profile_color), std::memory_order_release);
    output_snapshot.store((static_cast<std::uint32_t>(output.phase) << 24) |
                          LedSettings::rgb24(output.color), std::memory_order_release);
    config_snapshot.store((static_cast<unsigned>(settings.brightness) << 8) |
                          (static_cast<unsigned>(settings.mode) << 1) | settings.enabled, std::memory_order_release);
    revision_snapshot.store(LedSettings::applied_revision(), std::memory_order_release);
}
} // namespace

void initialize() {
    LedSettings::initialize();
#if OGXM_STATUS_LED_ENABLED
    if (initialized.load(std::memory_order_acquire)) return;
    // PIO USB uses PIO0 SM0/1/2. Prefer the otherwise independent PIO1
    // for WS2812, and never fall back to the USB-owned PIO0 when host enabled.
#if defined(CONFIG_EN_USB_HOST)
    for (PIO candidate : {pio1}) {
#else
    for (PIO candidate : {pio1, pio0}) {
#endif
        if (!pio_can_add_program(candidate, &ws2812_program)) continue;
        const int sm = pio_claim_unused_sm(candidate, false);
        if (sm < 0) continue;
        const unsigned offset = pio_add_program(candidate, &ws2812_program);
        led_pio = candidate;
        led_sm = sm;
        ws2812_program_init(led_pio, static_cast<unsigned>(led_sm), offset,
                            OGXM_STATUS_LED_PIN, 800000, 24);
        pio_sm_put(led_pio, static_cast<unsigned>(led_sm), 0);
        initialized.store(true, std::memory_order_release);
        output_snapshot.store(static_cast<std::uint32_t>(Phase::Starting) << 24, std::memory_order_release);
        return;
    }
#endif
}

void tick(std::uint32_t now_ms, const State& state) {
    // Process RAM changes even when a build disables the physical LED or when
    // no PIO state machine is available. Settings status must still progress.
    LedSettings::service(now_ms);
    if (static_cast<std::uint32_t>(now_ms - last_tick) < 20) return;
    last_tick = now_ms;
    Output output = policy.render(now_ms, state, LedSettings::get(), LedSettings::preview());
    if (!initialized.load(std::memory_order_relaxed)) {
        output.phase = Phase::Off;
        output.color = {};
        publish(output);
        return;
    }
    // WS2812 shifts GRB, most significant bit first, with 24-bit autopull.
    const std::uint32_t color = (static_cast<std::uint32_t>(output.color.g) << 24) |
                                (static_cast<std::uint32_t>(output.color.r) << 16) |
                                (static_cast<std::uint32_t>(output.color.b) << 8);
    if (color != last_color) {
        if (pio_sm_is_tx_fifo_full(led_pio, static_cast<unsigned>(led_sm))) return;
        // Updates are >=20ms apart; a pixel takes ~30us, followed by a low reset.
        // This single nonblocking put cannot delay USB or disturb LED timing.
        pio_sm_put(led_pio, static_cast<unsigned>(led_sm), color);
        last_color = color;
    }
    publish(output);
}

bool available() { return initialized.load(std::memory_order_acquire); }
int pin() { return OGXM_STATUS_LED_PIN; }

const char* state_name() {
    return state_name(static_cast<Phase>(output_snapshot.load(std::memory_order_acquire) >> 24));
}

Snapshot snapshot() {
    Snapshot value;
    const auto profile = profile_snapshot.load(std::memory_order_acquire);
    const auto output = output_snapshot.load(std::memory_order_acquire);
    const auto config = config_snapshot.load(std::memory_order_acquire);
    value.profile_id = static_cast<std::uint8_t>(profile >> 24);
    value.profile_color = profile & 0xffffffu;
    value.phase = static_cast<Phase>(output >> 24);
    value.output_color = output & 0xffffffu;
    value.brightness = static_cast<std::uint8_t>(config >> 8);
    value.enabled = (config & 1) != 0;
    value.mode = static_cast<LedSettings::Mode>((config >> 1) & 3);
    value.applied_revision = revision_snapshot.load(std::memory_order_acquire);
    return value;
}

} // namespace StatusLed
