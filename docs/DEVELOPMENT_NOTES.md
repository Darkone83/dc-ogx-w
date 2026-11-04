# Development notes (0.2.4 baseline)

This is the detailed source documentation retained from the pre-branding development snapshot. **Some snapshot values are superseded:** the current OLED default is 128×32, not 128×64, and the current splash/theme system is described in the new user-facing README. For current end-user setup and UI information see [../README.md](../README.md).

---

# dc-ogx-w

Personal RP2040 controller-adapter source project for the Raspberry Pi Pico
C/C++ SDK and VS Code. It combines the OGX controller drivers with a SoftAP,
WebUI, remote browser/Python input, live controller-profile editing, saved
home Wi-Fi settings, configurable GPIO8 WS2812 diagnostics and profile colours,
and a staged firmware updater.

## Choose the board

These are two different hardware targets with separate CMake presets:

| Hardware | Preset | Pico SDK board | Input | Output |
|---|---|---|---|---|
| RP2040-Pico W-2023 with ESP8285 | `pico-w-2023` | `pico_w_2023` | Wi-Fi from browser/Python | Built-in USB connector |
| Official Raspberry Pi Pico W with CYW43439 | `pico-w` | `pico_w` | Wi-Fi from browser/Python and Bluetooth controllers | Built-in USB connector |

The **Pico W-2023 is the default** for a fresh configuration. It uses the custom
SDK board definition in `boards/pico_w_2023.h`. Its onboard ESP8285 occupies
**UART0, GP0 TX / GP1 RX**, initially at **115200 baud, 8N1**. Both builds use
the board's built-in USB connector as the console/PC **device output**. Input
arrives over Wi-Fi, or over Bluetooth on the official board. No external USB
connector or USB-host wiring is used. A keyboard or gamepad connected to the
PC can send its input through the supplied browser/Python tools.

The clone has no Bluetooth hardware. Its build omits Bluepad32, BTstack, BLE,
and Wii Bluetooth sources and dependencies. Both targets accept USB output
modes, including original Xbox and XInput. Wii, PS1/PS2, GameCube, Dreamcast and
N64 non-USB output modes are excluded from these builds. The official Pico W
keeps Bluetooth controller support. The project does not compile or download
Pico-PIO-USB, or initialize a wired USB-host port.

## Open and build in VS Code

1. Extract the source and open the **dc-ogx-w** folder in VS Code.
2. Install the official **Raspberry Pi Pico** extension and its **Pico SDK
   2.3.0**, Arm toolchain, CMake and Ninja. Git is also needed for the first
   dependency download.
3. Use the **Pico SDK terminal**, with those tools available on `PATH`, and run
   the configure/build commands for your board below. The preset selects the
   correct hardware; an extension import configured as official `pico_w`
   selects the CYW43439 build.
4. Keep each board's build directory. Switch boards by selecting the other
   preset, rather than changing a configured build directory's board.

For the **ESP8285 clone**:

```sh
cmake --preset pico-w-2023
cmake --build --preset pico-w-2023 --parallel
```

For the **official Pico W**:

```sh
cmake --preset pico-w
cmake --build --preset pico-w --parallel
```

The initial-install outputs are respectively `build/pico-w-2023/dc-ogx-w.uf2` and
`build/pico-w/dc-ogx-w.uf2`. Each build also emits a board-specific `.ogxw`
package for subsequent WebUI updates, plus its ELF, map, and
`compile_commands.json`. The default editor configuration reads the clone's
compile database. For official-board development, change
`C_Cpp.default.compileCommands` in `.vscode/settings.json` to
`${workspaceFolder}/build/pico-w/compile_commands.json`.

**Terminal → Run Task** exposes matching Configure and Build tasks. Configure
once, then use the Build task for subsequent edits. These tasks require CMake
to be available to the VS Code process. The terminal commands above are also
usable directly. Ordinary builds let Ninja reconfigure when needed; they do
not force a new configure on every invocation.

The SDK remains outside this project. CMake uses `PICO_SDK_PATH` or the Pico
extension's SDK setup. If neither locates your SDK, add an explicit path to
the configure command, for example in PowerShell:

```powershell
cmake --preset pico-w-2023 "-DPICO_SDK_PATH=$env:USERPROFILE/.pico-sdk/sdk/2.3.0"
```

With a manually installed toolchain, keep CMake/Ninja/Arm GCC on `PATH` and use
the equivalent SDK directory. A build without presets can use
`cmake -S . -B build/pico-w-2023 -G Ninja -DPICO_BOARD=pico_w_2023 -DCMAKE_BUILD_TYPE=Release`.
Use a distinct `-B` directory and `-DPICO_BOARD=pico_w` for the official board.

The first configure downloads pinned TinyUSB and libfixmath
dependencies under the selected build directory. Only the official target
also downloads Bluepad32 and BTstack. The build never patches the installed
SDK; Bluetooth compatibility changes use CMake target properties or private
build-directory copies. Generated files belong to the build directory. They
can be regenerated without resetting the source or an SDK installation.

## First network connection

Connect the PC to **dc-ogx-w-XXXX**, password **ogxremote**, then open
**http://192.168.4.1**. `XXXX` is derived from the RP2040's unique board ID.

On the clone, this requires compatible **ESP8285 AT firmware** already running
on the module. Building the RP2040 application does not install or replace that
firmware. Startup configures the module's access point and TCP server through
UART; unsupported or nonresponsive AT firmware cannot provide an AP. The
separate [ESP8285 diagnostic tool](tools/esp8285/README.md) provides a raw USB
serial bridge and a read-only identification probe for checking the module.
It is a separate application, not an adapter output mode.

The adapter probes the base UART baud and 460800, resets a responding AT
interpreter, and requests **volatile** 460800 baud when supported. It falls back
to the base rate if negotiation fails. The module's permanent UART settings and
firmware are not changed. Read the actual baud and recommended update rate from
network status after startup.

The WebUI provides status, on-screen/keyboard input, stick/trigger settings,
button remapping and explicit profile saving. Applying a profile changes RAM;
**Save** writes flash and restarts the adapter. Remote control has one owner.
Input becomes neutral after **250 ms without a fresh sample**, while ownership
remains reserved for **five seconds** after the last input or heartbeat. A
heartbeat keeps ownership without holding any buttons or sticks. Short delays
can therefore recover with the same token, without another Take control click.
Explicit Release, page close and control-socket closure release ownership.
The Wi-Fi-only clone always retains Wi-Fi as its selected input source. A lost
session produces neutral output while the browser reconnects; it does not
switch to another source or replay held controls. The browser retries a broken
session up to eight times with backoff while the user's Take control intent is
still active. Explicit Release/Esc cancels that intent. Another owner is never
displaced.

Version 0.2.0 fixes a cross-core timestamp race that could immediately expire
fresh input. Failed HTTP status requests also no longer terminate an otherwise
healthy WebSocket control stream. Browser focus loss neutralizes and pauses
input, retaining ownership via heartbeat; returning starts from neutral.

Version **0.2.4** adds connection-health ping/pong independently of controller
acknowledgements, bounded automatic recovery for Wi-Fi-only input, explicit
Bluetooth/configuration/Wi-Fi modes, physical-controller calibration and a
separate Remapping tab. A timed-out ESP send with a completely transmitted body
now closes the affected connection and resynchronizes the AT interpreter; it
does not immediately reset every connection or retransmit ambiguous input.

### A PC controller in the browser

Open the adapter's WebUI, select **PC gamepad** on the Control tab, click the
page, and press a button on the controller connected to your PC. Select that
controller and take control. The UI checks the actual `navigator.getGamepads()`
capability and catches access errors; an ordinary HTTP URL no longer disables
gamepads automatically. The PC controller can be connected to the PC by USB or
Bluetooth; input reaches the adapter over Wi-Fi in either case.

If your browser blocks gamepad access, use the optional localhost bridge in
[tools/pc/README.md](tools/pc/README.md). Its Windows launcher serves the same
device UI locally and forwards traffic to the adapter. Keyboard and on-screen
controls remain available. After reconnection, release buttons and center the
sticks before fresh gamepad input is enabled.

### Input modes on the Bluetooth board

The official Pico W starts in **Bluetooth / physical** mode. Source changes are
explicit and include a neutral output handoff:

| Mode | Bluetooth link | USB output |
|---|---|---|
| Physical | Controller connection allowed | The connected controller, through the selected profile |
| Wi-Fi | Existing controller disconnected; new connections inhibited | Fresh browser/Python input, otherwise neutral |
| Configure | Controller connection allowed for live measurements | Neutral throughout calibration and remapping |

Releasing Wi-Fi control does not silently restore Bluetooth output. Select
Physical mode explicitly to return to it. Wi-Fi-only boards keep Wi-Fi mode
and hide Bluetooth-only calibration controls.

For the official board, enter Configure mode, connect the controller, and open
**Calibration**. Begin with sticks centered and triggers released. Capture the
center after at least 32 stable reports, move selected axes through their full
range, then apply. The UI shows raw readings before calibration/profile shaping
and rejects stale or incomplete captures. Select only axes that your controller
has. Calibration belongs to the selected profile; **Save and restart** persists
it along with the profile. The **Remapping** tab edits button and D-pad
destinations. All configuration changes keep console output neutral.

Network status identifies the board, transport, Bluetooth availability, UART
baud and recommended input rate. Use the supplied client/UI's selected rate:
the 115200-baud ESP path recommends **30 Hz**, while the official Wi-Fi path
recommends **60 Hz**. UART bandwidth and the module's AT-command handling make
the clone a distinct transport; hardware latency still needs measurement.

Original Xbox is the default output on fresh settings. For the clone, choose
another output mode through `OGXM_FIXED_DRIVER`. On the official board a
connected Bluetooth controller can also use the inherited shortcuts:
**Start + D-pad Up** for XInput or **Start + D-pad Right** for original Xbox,
held for approximately three seconds. A fixed PC-testing configuration is:

```sh
cmake --preset pico-w-2023 -DOGXM_FIXED_DRIVER=XINPUT
cmake --build --preset pico-w-2023 --parallel
```

Use the corresponding `pico-w` preset for the official board. CMake remembers
options per build directory; set `-DOGXM_FIXED_DRIVER=` to restore selectable
output modes.

## Join your home Wi-Fi

1. Open the WebUI's **Wi-Fi** tab and select **Scan networks**.
2. Select your **2.4 GHz** network from the results, or enter its SSID manually
   for a hidden network. Enter its password and enable **Join a saved network**.
   Leave the hostname as `dc-ogx-w`, or
   choose another single DNS label if you use more than one adapter.
3. Choose **Save and join**. Credentials are saved and the adapter restarts.
4. Connect the PC to that network. Open **http://dc-ogx-w.local**, or use the
   router-assigned address shown in the **Wi-Fi** tab.

The recovery access point remains **dc-ogx-w-XXXX / ogxremote**, at
**192.168.4.1**, including when the router is missing or credentials are wrong.
Return to that access point to correct settings. Blank password fields preserve
the saved password; the **no password** checkbox explicitly selects an open
network. **Forget network** erases the saved Wi-Fi credentials and returns to
AP-only operation. Network setting changes restart the adapter.

The official board uses lwIP mDNS. The clone requests `AT+MDNS` from its installed
ESP8285 firmware. In 0.2.4, its first join/address/mDNS setup gets a bounded
startup phase before a new controller lease is granted, so control cannot defer
the first name announcement forever. A failed name-service attempt releases
that startup gate; it cannot permanently prevent Wi-Fi control. Later retries
use backoff and wait while a controller owns input. A changed station address
restarts the responder.

The Wi-Fi/Device pages show the mDNS state, explicit command rejection or timeout,
attempt count, modem firmware identifier and command-timeout count. **Retry local
name** queues a responder restart without saving/rebooting. Release active
control to let that deferred command run. `active` means the responder was
started/accepted, not that another PC has successfully resolved the name.
An explicit AT rejection requires checking the installed modem firmware; a
timeout is a separate transport failure. The full URL is
**http://dc-ogx-w.local/**. The bare name `dc-ogx-w` depends on router DNS, and
`.local` requires the client and network to pass mDNS multicast. The station IP
shown in the UI remains a direct way to reach the adapter.
The scanner shows up to 16 strongest visible networks, with signal in dBm,
channel and security. Select fills the SSID exactly. Selecting a different
secured network requires its password; a blank password preserves the saved
credential only for the same SSID. Hidden networks can be entered manually.
The official radio exposes coarse WPA/RSN flags, so **WPA2 / RSN** is an
identification category rather than proof of compatible authentication.
Unsupported security is shown in the results with selection disabled.

The Wi-Fi tab distinguishes joining, connected, retry queued, bad password,
missing network, waiting for an IP address, and other join failures. It shows
the device's connection code and any reason for deferring a join. **Retry
joining** uses the already saved network without saving or restarting.
Loading these settings does not depend on the firmware updater being available.

STA connection attempts and scans are bounded and deferred while remote control
is owned, an update is active or a save is pending. An idle dashboard or observer
WebSocket can remain open. Retry and Scan release this page's own controller
ownership first; an owner in another tab or Python app must release it there.
Scans also wait for an ongoing join. ESP scanning temporarily occupies the AT
command pipe, and the page tolerates delayed status replies during that work.
AP+STA share one radio; the router can determine the access point's effective
channel during setup. Once home-Wi-Fi DHCP succeeds and the active controller
releases ownership, the transport disables SoftAP (STA-only). SoftAP is
restored after an extended station outage. Avoid rapid mode switching on
transient disconnects.

The ESP8285 setup disconnects an association inherited from modem-stored
credentials before attempting the network saved in this project. A successful
join command and confirmed station address are required before reporting a
connection. This also prevents an unrelated inherited connection from
suppressing the intended join.

## GPIO8 WS2812 status LED

Connect one WS2812's **DIN to GPIO8** and its ground to board ground. Supply the
LED at the voltage specified for your LED/module; GPIO8 supplies data, not LED
power. For a 5 V LED that requires a higher logic-high level, use a suitable
3.3 V-to-5 V data buffer. The implementation sends 800 kHz **GRB** data to one
pixel and defaults to brightness 24/255.

Both supplied presets explicitly select GPIO8. For an existing 0.2.1 build,
reconfigure with `cmake --preset pico-w-2023` (or `pico-w`) before rebuilding so
the previous cached GPIO9 value is replaced. A command-line
`-DOGXM_STATUS_LED_PIN=<gpio>` overrides the preset when another pin is desired.
The WebUI displays the pin reported by the running firmware.

Open the **LED** tab to enable/disable the pixel, set brightness, choose a mode,
and edit the eight profile colours and twelve diagnostic colours. **Apply for
this session** changes RAM only. **Save and restart** persists the settings in
their own checksummed record. **Test for 2 seconds** previews a colour and then
returns to normal indication; tests use the applied brightness, so zero stays
dark. Updates and update errors take priority over previews.

| Mode | Behaviour |
|---|---|
| Profiles with diagnostic overrides (default) | Actual applied profile colour while ready; connection, USB, paused-input and update states temporarily override it |
| Profile colours only | Actual applied profile colour, with a short blink on profile change; enabled update/error indications still take priority |
| Diagnostic states only | Connection, control and update states determine the colour |

| Default diagnostic colour/pattern | Meaning |
|---|---|
| White, slow blink | Starting |
| Blue, steady | Recovery AP ready (diagnostics mode) |
| Orange, regular blink | Joining the saved Wi-Fi network |
| Amber, double flash | Reconnecting or a queued retry |
| Orange-red, triple flash | Wi-Fi join failed or radio error |
| Yellow, short flash every 2 seconds | USB device not configured by a host |
| Amber, blink | Remote ownership retained, input paused/stale and neutral |
| Purple, pulse | Firmware update session |
| Red, fast blink | Failed update |
| White, three quick flashes | Applied profile changed (profile-only mode uses the profile colour) |
| Cyan, steady | Connected to home Wi-Fi (diagnostics mode) |
| Green, steady | Fresh remote input or Bluetooth controller connected (diagnostics mode) |

The **Profiles** tab now lists all eight saved slots. Select one and press
**Load saved profile** to load its mapping and tuning into RAM; selecting the
dropdown alone does not discard an editor draft. The active-profile label and
LED change only after the firmware's two-core application handshake completes.
Use the existing profile Save action to make that slot active after reboot.

A claimed PIO state machine generates the waveform without adding timer IRQs
or blocking the USB loop. No LED is required to run the firmware. Disable it with
`-DOGXM_STATUS_LED_ENABLED=OFF` if GPIO8 is needed elsewhere.

## Update firmware through the UI

Install this version **once with BOOTSEL and its UF2**. Older firmware does not
have the upload endpoints. After that, build normally and open **Device →
Firmware update**:

1. Select `build/pico-w-2023/dc-ogx-w-pico_w_2023.ogxw` for the clone, or
   `build/pico-w/dc-ogx-w-pico_w.ogxw` for the official board.
2. Choose **Upload and verify**. Controller input pauses during the update.
3. After verification succeeds, choose **Install and restart**.

The package includes board/layout markers, image length, boot-vector/boot2
validation and CRC32 checks. Uploads go to a separate staging area. Incomplete
uploads and rejected packages leave the running firmware in place. The final
installer copies from SRAM using ROM flash routines, preserving the last 24 KiB
of settings. This updates the RP2040 application, including its WebUI; it does
not replace the clone's ESP8285 AT firmware.

Keep power connected during installation. This is not an A/B bootloader: a power
loss during the final copy can require BOOTSEL/UF2 recovery, and no automatic
rollback is implemented. The firmware image is limited to **1000 KiB** on both
boards so the running and staged images fit the conservative 2 MiB layout.
Oversized builds fail at link/package time. See the
[updater format and implementation notes](tools/firmware/README.md).

## Configuration

| CMake setting | Purpose |
|---|---|
| `OGXM_WIFI_SSID_PREFIX` | AP name prefix; default `dc-ogx-w` |
| `OGXM_WIFI_PASSWORD` | AP passphrase; default `ogxremote`; clone excludes commas |
| `OGXM_WIFI_CHANNEL` | AP channel 1–11; default 6 |
| `OGXM_FIXED_DRIVER` | Optional fixed USB output mode, such as `XINPUT` or `XBOXOG` |
| `OGXM_FIXED_DRIVER_ALLOW_COMBOS` | Keep official-board Bluetooth mode shortcuts enabled with a fixed default |
| `OGXM_ESP_UART_ID`, `OGXM_ESP_UART_TX_PIN`, `OGXM_ESP_UART_RX_PIN` | Clone UART wiring; defaults 0, 0, 1 |
| `OGXM_ESP_UART_BAUD` | Clone initial/base UART baud; default 115200 before volatile negotiation |
| `SYSCLOCK_KHZ` | Clone default 120000; official target retains 240000 |
| `OGXM_STATUS_LED_ENABLED` | Enable external WS2812; default ON |
| `OGXM_STATUS_LED_PIN` | WS2812 DIN GPIO; default 8 |
| `OGXM_STATUS_LED_BRIGHTNESS` | Brightness 0–255; default 24 |

UART options describe the connection to the onboard module. Pin settings must
match the hardware; the base baud is used for probing and fallback.
Debug builds use a separate logging UART1 on GP4/GP5. The external LED uses GPIO8;
no unidentified clone onboard LED, ESP reset, enable or boot GPIO is driven.

## Source map

| Path | Implementation |
|---|---|
| `src/main.cpp`, `src/OGXMini/OGXMini.cpp` | Startup and board-specific runtime selection |
| `src/OGXMini/Board/PicoW.cpp` | Official Pico W native USB output, BT and Wi-Fi scheduling |
| `src/OGXMini/Board/PicoW2023.cpp` | Clone native USB output, ESP UART/network and remote-input scheduling |
| `src/Board/`, `boards/` | Application pin definitions and custom SDK board |
| `src/Board/StatusLed.cpp`, `StatusLedPolicy.cpp`, `LedSettings.cpp` | GPIO8 PIO output, profile/diagnostic policy and persistent LED settings |
| `src/DeviceServices/` | Core0 maintenance, deferred persistence/restart and LED coordination |
| `src/USBDevice/`, `src/Descriptors/` | Console/PC USB output, including OG Xbox and XInput |
| `src/Input/`, `src/Bluepad32/`, `src/BLEServer/` | Controller slots and official-board Bluetooth integration |
| `src/USBHost/`, `src/Wii/` | Retained foundation sources; wired-host and Wii-output implementations are excluded from these builds |
| `src/NetworkService/NetworkService.cpp`, `WebProtocol.cpp` | Shared HTTP/WebSocket service and framing |
| `src/NetworkService/Cyw43Transport.cpp` | Official-board AP/STA, DHCP/mDNS, lwIP sockets and radio timer |
| `src/NetworkService/EspAtProtocol.cpp`, `EspAtTransport.cpp` | Clone AT protocol engine and UART transport |
| `src/NetworkService/web/index.html` | Editable WebUI, embedded during the build |
| `src/NetworkService/NetworkSettings.cpp` | Saved station credentials, hostname and redacted settings API |
| `src/NetworkService/WifiScan.cpp` | Bounded discovery results, validation, deduplication and signal ordering |
| `src/FirmwareUpdate/`, `tools/firmware/` | Validated update transactions, SRAM/ROM installer and package generator |
| `src/RemoteControl/` | Input protocol, ownership, timeout, arbitration and profile API |
| `src/Gamepad/`, `src/UserSettings/` | Controller state, shaping/remapping and flash persistence |
| `tools/pc/` | Python keyboard/gamepad client and local browser bridge |
| `tools/esp8285/` | Separate ESP8285 UART diagnosis project |
| `cmake/`, `CMakePresets.json` | Dependency pins, board source selection and build settings |

Edit application source or `index.html`, then rebuild the selected preset.
Web assets and optional Bluetooth GATT headers are generated inside its build
directory. SDK 2.3 uses `cmake/linker/` overrides; a legacy SDK linker path is
retained in `cmake/generated.cmake`.

## PC input and API

In a Python virtual environment:

```sh
python -m pip install -r tools/pc/requirements.txt
python tools/pc/ogx_remote.py --host 192.168.4.1 desktop
python tools/pc/ogx_remote.py --host 192.168.4.1 web
# Once joined to home Wi-Fi, if mDNS is available:
python tools/pc/ogx_remote.py --host dc-ogx-w.local desktop
```

The local browser bridge serves the same UI on localhost and forwards its API
traffic to the adapter, enabling the browser's PC-gamepad path. See
[the PC client README](tools/pc/README.md) and [API reference](docs/remote-api.md).

## Scope and validation

The two Release targets and clone Debug build passed with **Pico SDK 2.3.0 and
Arm GNU 14.2.Rel1**. See [build and protocol validation](docs/validation.md)
for measured memory use, dependency checks and host test results. Both
targets reserve 8 KiB per core for stacks and keep the final 24 KiB of the
firmware's **2 MiB flash layout** for settings/Bluetooth storage. The clone
keeps that layout even though its Bluetooth region is unused; a larger flash
chip is not assumed or required.

Both targets enable networking with the built-in USB output modes. GPIO console
outputs and wired USB-host input are excluded. The WebUI updater and external
WS2812 are implemented in this version. Their physical operation still needs
validation on each board; source tests and builds cannot establish it.

Compiling and linking verify source integration. USB enumeration, ESP8285 AT
compatibility, official-board Wi-Fi/Bluetooth coexistence, and end-to-end
controller timing still require tests on the actual boards and target consoles.

The controller foundation comes from
[MegaCadeDev/OGX-Mini-2026](https://github.com/MegaCadeDev/OGX-Mini-2026)
at commit `2c5b41096e14e2ff7b0b0f84c1b1cce8e403e34b`, derived from
[wiredopposite/OGX-Mini](https://github.com/wiredopposite/OGX-Mini).
Original source notices are retained in `LICENSE` and the relevant components.


## Optional OLED and UDP discovery foundation

The SSD1306 status display uses I2C1 **GP14 SDA / GP15 SCL**, address 0x3C, with a 128x64 framebuffer. WS2812 remains on **GP8** and the ESP8285 UART on GP0/GP1. OLED failures do not block USB or networking.

The official Pico W's native CYW43 transport supports UDP discovery at port 38473 using `DC-OGX-W|DISCOVER|1` and periodic discovery announcements. The ESP8285 AT-based board does not enable UDP discovery in this pass to avoid adding AT command traffic during control sessions. Windows application and UDP controller input will be separate future work. See `docs/oled-udp-foundation.md`.
