#include "OGXMini/OGXMini.h"
#include "pico/flash.h"
#include "pico/stdlib.h"

int main()
{
    stdio_init_all();
    flash_safe_execute_core_init();

    // The Pico W runtime starts the USB output/input paths, Bluetooth,
    // SoftAP/WebUI and remote-input/profile services on their owning cores.
    OGXMini::initialize();
    OGXMini::run();
    return 0;
}
