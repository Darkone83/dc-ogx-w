#include "DeviceServices.h"

#include <cstdio>
#include "Board/StatusLed.h"
#include "Board/LedSettings.h"
#include "Board/board_api.h"
#include "Display/OledDisplay.h"
#include "Display/OledSettings.h"
#include "UserSettings/UserSettings.h"
#include "FirmwareUpdate/FirmwareUpdate.h"
#include "NetworkService/NetworkService.h"
#include "NetworkService/NetworkSettings.h"
#include "RemoteControl/RemoteControl.h"

namespace DeviceServices {
namespace {
NetworkService::SaveAction deferred = NetworkService::SaveAction::None;
std::uint32_t response_completed_at{};
}

void initialize() {
    NetworkSettings::initialize();
    OledSettings::initialize();
    StatusLed::initialize();
}

void tick(std::uint32_t now_ms, bool usb_configured, bool bluetooth_connected) {
    // A queued update takes ownership before any flash operation. Stale or
    // physical input cannot be sent while the update pauses XIP on both cores.
    RemoteControl::set_maintenance(FirmwareUpdate::active());
    RemoteControl::tick(now_ms);
    FirmwareUpdate::tick(now_ms);
    NetworkService::note_runtime(usb_configured, bluetooth_connected, false, RemoteControl::session_active());

    NetworkService::Completion completion;
    if (deferred == NetworkService::SaveAction::None && NetworkService::take_completion(completion)) {
        deferred = completion.action;
        response_completed_at = now_ms;
    }
    if (NetworkService::install_cancel_requested()) {
        if (deferred == NetworkService::SaveAction::Firmware)
            deferred = NetworkService::SaveAction::None;
        NetworkService::acknowledge_install_cancel();
    }
    // In particular, ESP SEND OK only means modem acceptance. Keep servicing
    // the modem for one second before resetting Core1 so its reply can drain.
    if (deferred != NetworkService::SaveAction::None &&
        static_cast<std::uint32_t>(now_ms - response_completed_at) >= 1000) {
        const auto action = deferred;
        deferred = NetworkService::SaveAction::None;
        if (action == NetworkService::SaveAction::Profile) {
            RemoteControl::commit_after_response();
        } else if (action == NetworkService::SaveAction::Firmware) {
            if (!FirmwareUpdate::failed() && FirmwareUpdate::active())
                FirmwareUpdate::commit_after_response();
        } else if (action == NetworkService::SaveAction::Network) {
            board_api::usb::disconnect_all();
            if (!NetworkSettings::commit())
                std::printf("dc-ogx-w: network settings could not be saved\n");
            board_api::reboot();
        } else if (action == NetworkService::SaveAction::Led) {
            board_api::usb::disconnect_all();
            if (!LedSettings::commit())
                std::printf("dc-ogx-w: LED settings could not be saved\n");
            board_api::reboot();
        } else if (action == NetworkService::SaveAction::Oled) {
            board_api::usb::disconnect_all();
            if (!OledSettings::commit())
                std::printf("dc-ogx-w: OLED settings could not be saved\n");
            board_api::reboot();
        }
    }

    const auto network = NetworkService::health();
    StatusLed::State led;
    led.network_ready = network.ready;
    led.station_configured = network.station_configured;
    led.station_connected = network.station_connected;
    led.station_failed = network.station_failed;
    led.station_joining = network.station_joining;
    led.station_retry_pending = network.station_retry_pending;
    led.network_error = network.error;
    led.usb_configured = usb_configured;
    led.control_owned = RemoteControl::session_active();
    led.input_fresh = RemoteControl::input_fresh();
    led.bluetooth_connected = bluetooth_connected;
    led.updating = FirmwareUpdate::active();
    led.update_error = FirmwareUpdate::failed();
    led.profile_id = RemoteControl::applied_profile_id();
    StatusLed::tick(now_ms, led);
    // Atomics only; OLED I2C traffic is handled by the radio core.
    OledDisplay::publish_controller(usb_configured, RemoteControl::session_active(),
        RemoteControl::input_fresh(), RemoteControl::applied_profile_id(),
        static_cast<std::uint8_t>(UserSettings::get_instance().get_current_driver()));
}
} // namespace DeviceServices
