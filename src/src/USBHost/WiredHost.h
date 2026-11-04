#pragma once

#include "Board/Config.h"
class Gamepad;

// Experimental second USB port. Native RP2040 USB stays in device mode.
// Call only from the USB/device Core0 loop; the timer IRQ clocks PIO SOFs.
namespace WiredHost {
    void initialize(Gamepad (&gamepads)[MAX_GAMEPADS]);
    void poll();
    void stop();
    bool sof_timer_running();
    bool active();
}
