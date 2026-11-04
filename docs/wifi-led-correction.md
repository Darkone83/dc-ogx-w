# Wi-Fi scanner, join correction and GPIO8 — 0.2.2

The complete ZIP contains the VS Code/Pico SDK source project. The correction
ZIP overlays the preceding 0.2.1 project under the same `dc-ogx-w/` directory.
Merge corresponding files if you have local edits. Neither archive includes
SDK checkouts or build binaries, and the correction removes no project files.

## Build for GPIO8

Both presets explicitly set `OGXM_STATUS_LED_PIN=8`. Reconfigure and build your
current ESP8285 clone from the project directory:

```powershell
cmake --preset pico-w-2023
cmake --build --preset pico-w-2023
```

Reconfiguration replaces the previous cached GPIO9 value. Use `pico-w` for the
official board. The LED driver default and fallback are GPIO8, and WebUI labels
read the running firmware's pin. Wire WS2812 DIN to GP8; power/ground details
are in the main README.

Install the resulting UF2, or use the board-specific `.ogxw` package with the
existing UI updater. Reload the page after installing the new firmware.

## Scan and join

1. Join `dc-ogx-w-XXXX`, password `ogxremote`, and open `http://192.168.4.1`.
2. Open **Wi-Fi** and choose **Scan networks**.
3. Select your network. Results show signal in dBm, channel and security.
4. Enter its password, enable joining, and choose **Save and join**.
5. After restart, check the connection card for an assigned IP or exact error.

Hidden networks can be entered manually. Selecting or typing a different SSID
requires its password or the explicit open-network choice. A blank password
preserves the credential only when editing the same saved SSID.

Scan and Retry release this page's remote-control ownership first. A session
in another tab or Python app must release ownership there. Queued scans show
what they are waiting for. ESP scans can briefly delay HTTP status replies;
the page reconciles status without blindly repeating the scan-start request.

The recovery AP remains available while joining, then automatically enters standby
once the home network assigns an IPv4 address. The transition disconnects users
on the AP, so connect through the assigned home-network IP or `http://dc-ogx-w.local`
if mDNS works. After a sustained home-Wi-Fi outage, the recovery AP returns.

## Join corrections

The old ESP path could accept an inherited modem association and skip the
SSID selected in this project's settings. Initialization now disconnects that
association, and connection reporting requires the intended join command to
succeed followed by a valid station address. Failed/timed-out joins clear
pending address work. Bounded command scheduling prevents read-only observer
traffic from indefinitely postponing scans or joins.

Retry previously retained this page's ownership and could defer its own
attempt. It now releases that session first. The connection panel separates
normal startup/address acquisition from failures, shows the connection code,
and identifies why an attempt is waiting.

If the board still cannot join, capture the **Connection code** and the scan
result's signal/security/channel. Those observations distinguish authentication,
missing-network, address-timeout and radio problems.

## Validation

See [validation.md](validation.md) for build/test results. Software tests cover
the scanner, actual embedded UI, response/generation ordering and inherited
associations. Physical RF discovery and joining still require the actual board.
ZIP timestamps use MST (UTC−7), five minutes in the past.
