# Planned USB host input — candybar PCB (not enabled yet)

**Scope:** Firmware planning for a future USB-A *host-input* port on dc-ogx-w. This does **not** electrically pass Xbox USB D+/D− straight through. The RP2040 receives a wired controller over USB host, processes its input through existing profiles/remapping, and emits the selected USB controller protocol through its separate native USB device connector.

## Reserved pin map

| Signal | RP2040 pin | Mode |
|---|---|---|
| USB host D+ | **GP9** | PIO-USB D+ |
| USB host D− | **GP10** | Consecutive PIO-USB D− |
| ESP8285 UART0 | GP0/GP1 | Separate network modem |
| WS2812 | GP8 | Independent status LED |
| OLED SDA/SCL | GP14/GP15 | I²C1 |
| Xbox USB output | RP2040 native USB | Existing USB device; unchanged |

**Hardware note for PCB:** USB host downstream ports normally have **15 kΩ pull-down resistors on both D+ and D−**, not 4.7 kΩ. Include suitable ~22 Ω line-series resistors if required by the selected PIO-USB wiring design, ESD protection, an appropriately current-limited 5 V VBUS power switch, VBUS sensing and over-current recovery. Verify exact BOM/layout against the final PIO-USB example and USB electrical requirements. The controller host port cannot be powered directly from a 3.3 V GPIO.

## Driver integration (to implement after hardware)

1. Check PIO, state-machine, DMA and interrupt allocations against the current WS2812/USB stack and the ESP8285 service scheduling.
2. Enable PIO-USB as a USB **host** while keeping TinyUSB's native-device transport. Confirm RP2040 timing/core allocation for the hardware revision.
3. Attach the existing OGX-Mini controller parsers to a normalized **wired input source**, not a duplicate profile/output pipeline.
4. Add explicit capability and input-owner state; release the previous source to **neutral** before wired/controller/BT/remote handoff.
5. Provide attach/detach, power fault and unsupported device states to OLED and WebUI without blocking controller output.
6. Test a controller matrix on physical PCB: XInput, HID, hotplug, suspend/resume, low-speed/full-speed behavior, and simultaneous radio traffic.
7. Add regression tests for source switching and stale reports, then lift the CMake host feature gate.

## Current build behavior

`DC_OGX_USB_HOST_DP_PIN=9` and `DC_OGX_USB_HOST_DM_PIN=10` document the planned wiring. The option `DC_OGX_USB_HOST_ENABLED` defaults OFF. Turning it ON **fails CMake intentionally** because there is **no working host stack integration** yet. Do not advertise wired passthrough as a shipping feature before integration and hardware validation.

**References:** <https://github.com/sekigon-gonnoc/Pico-PIO-USB> and <https://github.com/wiredopposite/OGX-Mini>. Respect upstream licenses and attribution.
