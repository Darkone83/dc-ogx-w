#include "Board/Config.h"

#if defined(CONFIG_OGXM_BOARD_PI_PICOW_2023)

#include <pico/flash.h>
#include <pico/multicore.h>
#include <pico/stdlib.h>

#include "tusb.h"
#include "Board/board_api.h"
#include "Board/ogxm_log.h"
#include "DeviceServices/DeviceServices.h"
#include "Gamepad/Gamepad.h"
#include "NetworkService/NetworkService.h"
#include "OGXMini/Board/PicoW2023.h"
#include "RemoteControl/RemoteControl.h"
#include "TaskQueue/TaskQueue.h"
#include "USBDevice/DeviceManager.h"
#include "UserSettings/UserSettings.h"
#if defined(CONFIG_EN_USB_HOST)
#include "USBHost/WiredHost.h"
#endif

namespace {

Gamepad gamepads[MAX_GAMEPADS];

void core1_main() {
    flash_safe_execute_core_init();
    // Construct Core1's task queue/IRQ here, before the shared save lifecycle
    // can suspend it. The transport itself uses a nonblocking polling loop.
    TaskQueue::Core1::process_tasks();
    if (!NetworkService::start()) {
        OGXM_LOG("Pico-W-2023: network start rejected; USB output remains active\n");
    }
    while (true) {
        // The UART IRQ only buffers bytes. HTTP/WebSocket parsing and the
        // profile-apply handshake run here, separately from native USB output.
        NetworkService::poll();
        TaskQueue::Core1::process_tasks();
        tight_loop_contents();
    }
}

} // namespace

void pico_w_2023::initialize() {
    board_api::init_board();
    UserSettings& settings = UserSettings::get_instance();
    settings.initialize_flash();
    DeviceServices::initialize();
    const DeviceDriverType driver = settings.get_current_driver();
    for (uint8_t i = 0; i < MAX_GAMEPADS; ++i) {
        gamepads[i].set_profile(settings.get_profile_by_index(i), driver);
    }
    DeviceManager::get_instance().initialize_driver(driver, gamepads);
    RemoteControl::begin(gamepads[0], driver);
#if defined(CONFIG_EN_USB_HOST)
    WiredHost::initialize(gamepads);
#endif
}

void pico_w_2023::run() {
    // Native USB is the console output. Optional GP9/GP10 host input is
    // serviced from Core0 without changing the ESP8285's AT transport.
    tud_init(BOARD_TUD_RHPORT);
    TaskQueue::Core0::process_tasks();
    multicore_reset_core1();
    multicore_launch_core1(core1_main);

    DeviceDriver* driver = DeviceManager::get_instance().get_driver();
    while (true) {
        TaskQueue::Core0::process_tasks();
        DeviceServices::tick(to_ms_since_boot(get_absolute_time()), tud_mounted(), false);
        tud_task();
#if defined(CONFIG_EN_USB_HOST)
        WiredHost::poll();
#endif
        for (uint8_t i = 0; i < MAX_GAMEPADS; ++i) {
            Gamepad& output = i == 0 ? RemoteControl::output_gamepad(gamepads[i]) : gamepads[i];
            driver->process(i, output);
            if (i == 0) RemoteControl::on_output_processed(output);
        }
        // Never insert a millisecond sleep into the configured USB device path.
        if (MAIN_LOOP_DELAY_US > 0) sleep_us(MAIN_LOOP_DELAY_US);
        else tight_loop_contents();
    }
}

#endif // CONFIG_OGXM_BOARD_PI_PICOW_2023
