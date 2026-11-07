#pragma once
#include "Board/Config.h"
class Gamepad;

// Only this adapter selects CYW43/BTstack versus the ESP8285 UART radio.
namespace RadioBackend {
void run(Gamepad (&gamepads)[MAX_GAMEPADS]);
bool bluetooth_connected();
void process_adaptive_triggers();
}
