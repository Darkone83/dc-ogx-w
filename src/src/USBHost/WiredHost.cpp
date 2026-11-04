#include "Board/Config.h"
#if defined(CONFIG_EN_USB_HOST)

#include "USBHost/WiredHost.h"

#include <atomic>
#include <cstdint>
#include "hardware/gpio.h"
#include "host/hcd.h"
#include "pico/time.h"
#include "pio_usb.h"
#include "tusb.h"

#include "Board/board_api_private/board_api_private.h"
#include "USBHost/HostManager.h"

#if defined(CONFIG_EN_BLUETOOTH)
#include "Bluepad32/Bluepad32.h"
#endif

namespace WiredHost {
namespace {
constexpr std::int64_t kConnectDebounceUs = 40000;
constexpr std::int64_t kUnplugDebounceUs = 120000;
constexpr std::uint32_t kCooldownMs = 750;
constexpr std::uint32_t kFeedbackMs = 200;

repeating_timer_t sof_timer{};
std::atomic<bool> timer_running{false};
bool host_running = false;
bool ever_mounted = false;
bool probing_line = false;
bool checking_unplug = false;
bool bt_suspended = false;
std::uint32_t host_started_ms = 0;
absolute_time_t line_high_since{};
absolute_time_t unplug_since{};
std::uint32_t last_feedback_ms = 0;
std::uint32_t ready_again_ms = 0;

bool sof_callback(repeating_timer_t*) {
    // Do not run TinyUSB host task here: only schedule full-speed frames.
    pio_usb_host_frame();
    return true;
}

bool configured_device_present() {
    constexpr std::uint8_t max_addr = CFG_TUH_DEVICE_MAX + CFG_TUH_HUB;
    for (std::uint8_t addr = 1; addr <= max_addr; ++addr) {
        if (tuh_mounted(addr)) return true;
    }
    return false;
}

void stop_host() {
    if (timer_running.exchange(false, std::memory_order_acq_rel)) {
        cancel_repeating_timer(&sof_timer);
    }
    if (host_running) {
        (void)tuh_deinit(BOARD_TUH_RHPORT);
        host_running = false;
    }
    probing_line = false;
    checking_unplug = false;
    ever_mounted = false;
    ready_again_ms = to_ms_since_boot(get_absolute_time()) + kCooldownMs;
    board_api_usbh::enable_host_line_irq_monitoring();
#if defined(CONFIG_EN_BLUETOOTH)
    if (bt_suspended) {
        bluepad32::wired_usb_release_enable_bt_pairing();
        bt_suspended = false;
    }
#endif
}

} // namespace

void initialize(Gamepad (&gamepads)[MAX_GAMEPADS]) {
    HostManager::get_instance().initialize(gamepads);
}

bool sof_timer_running() { return timer_running.load(std::memory_order_acquire); }
bool active() { return host_running; }
void stop() { stop_host(); }

void poll() {
    const auto now = get_absolute_time();
    const auto now_ms = to_ms_since_boot(now);
    if (!host_running) {
        if (static_cast<std::int32_t>(now_ms - ready_again_ms) < 0) return;
        // Before enumeration the two pins are GPIO inputs with 15k pull-downs.
        // Either D+ (full-speed) or D- (low-speed) going high indicates a device.
        const bool line_high = gpio_get(PIO_USB_DP_PIN) || gpio_get(PIO_USB_DP_PIN + 1);
        if (!line_high) { probing_line = false; return; }
        if (!probing_line) { probing_line = true; line_high_since = now; return; }
        if (absolute_time_diff_us(line_high_since, now) < kConnectDebounceUs) return;

#if defined(CONFIG_EN_BLUETOOTH)
        // Wired input wins. The existing Bluepad32 handoff keeps Bluetooth
        // pairing disabled while PIO USB owns the pins and device slot.
        if (!bt_suspended) {
            bluepad32::wired_usb_takeover_disconnect_bt();
            bt_suspended = true;
        }
#endif
        board_api_usbh::suspend_line_irq();
        pio_usb_configuration_t cfg = PIO_USB_CONFIG;
        cfg.alarm_pool = nullptr;
        cfg.skip_alarm_pool = true;
        cfg.tx_ch = 2; // avoid CYW43's lower DMA channels on official Pico W
        tuh_configure(BOARD_TUH_RHPORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &cfg);
        if (!tuh_init(BOARD_TUH_RHPORT)) {
            stop_host();
            return;
        }
        host_running = true;
        host_started_ms = now_ms;
        if (!add_repeating_timer_us(-1000, sof_callback, nullptr, &sof_timer)) {
            // Do not try a jittery Core0 SOF fallback while Wi-Fi is running.
            stop_host();
            return;
        }
        timer_running.store(true, std::memory_order_release);
        last_feedback_ms = now_ms;
        ever_mounted = false;
        checking_unplug = false;
    }

    // USB host processing stays in Core0, never in ESP UART or CYW43 callbacks.
    // The 1kHz timer keeps SOFs flowing during enumeration or brief core stalls.
    tuh_task();
    const bool mounted = configured_device_present();
    if (mounted) ever_mounted = true;
    const bool hcd_connected = hcd_port_connect_status(BOARD_TUH_RHPORT);
    board_api_usbh::store_host_line_connected(hcd_connected);
    // Once a device has mounted, TinyUSB's mount state is authoritative.
    // If enumeration never succeeded, require a 3s grace and a sustained
    // physical disconnect before giving up (avoids a false unplug during reset).
    const bool unplug_hint = (ever_mounted && !mounted) ||
        (!mounted && !hcd_connected &&
         static_cast<std::uint32_t>(now_ms - host_started_ms) >= 3000);
    if (unplug_hint) {
        if (!checking_unplug) { checking_unplug = true; unplug_since = now; }
        else if (absolute_time_diff_us(unplug_since, now) >
                 (ever_mounted ? kUnplugDebounceUs : 1000000)) {
            stop_host();
            return;
        }
    } else {
        checking_unplug = false;
    }
    if (static_cast<std::uint32_t>(now_ms - last_feedback_ms) >= kFeedbackMs) {
        last_feedback_ms = now_ms;
        HostManager::get_instance().send_feedback();
    }
}

} // namespace WiredHost

// The original board_api::usb::disconnect_all() invokes this hook before
// flash writes and reboot. Do not leave the PIO timer running during flash.
namespace board_api_usbh {
void stop_pio_usb_host() { WiredHost::stop(); }
}

#endif // CONFIG_EN_USB_HOST
