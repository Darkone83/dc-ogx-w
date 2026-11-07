#include "OGXMini/Board/RadioBackend.h"
#include <pico/flash.h>
#include <pico/stdlib.h>
#include "Board/board_api.h"
#include "NetworkService/NetworkService.h"
#if defined(CONFIG_EN_BLUETOOTH)
#include "Bluepad32/Bluepad32.h"
#include "BLEServer/BLEServer.h"
#endif

namespace RadioBackend {
void run(Gamepad (&gamepads)[MAX_GAMEPADS]) {
    flash_safe_execute_core_init();
#if defined(CONFIG_EN_BLUETOOTH)
    board_api::init_bluetooth();
    NetworkService::start();
    board_api::set_led(true);
    BLEServer::init_server(gamepads);
    // The 2026 BTstack loop owns its radio callbacks. NetworkService installs
    // its existing serialized poll hook before entering that loop.
    bluepad32::run_task(gamepads);
#else
    (void)gamepads;
    NetworkService::start();
    while (true) {
        NetworkService::poll();
        tight_loop_contents();
    }
#endif
}

bool bluetooth_connected() {
#if defined(CONFIG_EN_BLUETOOTH)
    return bluepad32::any_connected();
#else
    return false;
#endif
}

void process_adaptive_triggers() {
#if defined(CONFIG_EN_BLUETOOTH)
    bluepad32::process_pending_adaptive_triggers();
#endif
}
}
