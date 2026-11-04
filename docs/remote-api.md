# dc-ogx-w API v2

This reference describes remote firmware **0.2.4** on both the official `pico_w` and ESP8285 `pico_w_2023` targets. Both serve the same WebUI and API at `http://192.168.4.1` on their own access point. See [README.md](../README.md) for board selection, network settings and build instructions.

The API controls player 0 while the adapter retains its selected output mode on the built-in USB connector, including OG Xbox and XInput. Both boards accept network input; the official board also supports Bluetooth controller input. These builds have no wired USB-host input and exclude Wii/GPIO console-output modes. Version 0.2.4 adds explicit source/configuration modes, per-profile Bluetooth calibration, transport health probes, automatic Wi-Fi-only client recovery, and bounded mDNS setup/retry diagnostics. The LED default remains GPIO8.

## Endpoints

POST bodies are JSON objects with `Content-Type: application/json`. HTTP connections close after a response; use WebSocket for continuous input.

| Method | Path | Purpose |
|---|---|---|
| GET | `/` | Embedded WebUI |
| GET | `/api/status` | Controller, profile, output and network status |
| GET | `/api/network` | AP/STA, hostname, transport and LED status |
| GET | `/api/network/config` | Saved station settings, excluding password |
| POST | `/api/network/config` | Validate, save settings and restart |
| POST | `/api/network/retry` | Retry joining the saved network after the response completes |
| POST | `/api/network/mdns/retry` | Queue a responder restart after the response completes |
| GET | `/api/network/scan` | Scan phase, generation, results and failure reason |
| POST | `/api/network/scan` | Queue a bounded nearby-network scan |
| GET | `/api/led` | Current LED settings, revisions and persistence state |
| POST | `/api/led/apply` | Apply LED settings in RAM |
| POST | `/api/led/save` | Stage LED settings for persistence and restart |
| POST | `/api/led/test` | Bounded colour preview without saving |
| GET | `/api/oled` | OLED state, hardware detection, current settings and board defaults |
| POST | `/api/oled/apply` | Apply OLED settings live without writing flash |
| POST | `/api/oled/save` | Stage OLED settings for deferred flash persistence and restart |
| GET | `/api/update` | Firmware upload state and progress |
| POST | `/api/update/begin`, `/chunk`, `/finish`, `/install`, `/cancel` | Staged firmware update; see updater reference |
| GET | `/api/profile` | Current editing profile, revisions and effective-default flags |
| GET | `/api/profiles` | Eight saved slots and requested/applied profile IDs |
| POST | `/api/profile/select` | Load a saved profile into RAM through the application handshake |
| POST | `/api/profile` | Validate and apply a partial or complete profile in RAM |
| POST | `/api/profile/save` | Save the applied revision and restart |
| GET, POST | `/api/control/mode` | Read/select `wifi`, `physical`, or `configure` source policy |
| GET, POST | `/api/calibration` | Read raw Bluetooth input; capture/apply/reset per-profile calibration |
| POST | `/api/control/acquire` | Acquire the exclusive remote-input lease |
| POST | `/api/control/release` | Release the lease |
| POST | `/api/control/heartbeat` | Keep ownership without refreshing input |
| POST | `/api/control/input` | Submit one complete controller state |
| GET, upgrade | `/ws` | WebSocket carrying the same input JSON |

## OLED configuration API

`GET /api/oled` returns a snapshot similar to:

```json
{"ok":true,"available":true,"detected":true,"enabled":true,
 "brightness":224,"idle_brightness":32,"dim_after_s":0,"off_after_s":0,
 "invert":false,"flip":false,"fade":true,"dirty":false,"save_pending":false,
 "defaults":{"enabled":true,"brightness":224,"idle_brightness":32,
             "dim_after_s":0,"off_after_s":0,"invert":false,"flip":false,"fade":true}}
```

POST an object to `/api/oled/apply` or `/api/oled/save`. All fields are optional; unspecified fields retain their current values. `brightness` and `idle_brightness` accept integers 0–255. `dim_after_s` and `off_after_s` accept exactly `0`, `30`, `60`, `120`, `300`, `600`, `900`, `1800`, `3600`. `enabled`, `invert`, `flip`, `fade` must be booleans. Unknown properties or invalid values receive `400` with a specific error string.

- `/api/oled/apply` updates RAM only, and returns `{"ok":true,"applied":true,"restart_required":false}`.
- `/api/oled/save` stages the new values and returns `{"ok":true,"restart_required":true,"rebooting":true}`. Its flash commit is deferred until the response drains, followed by a restart; a dropped response cancels staging.
- Firmware update or another deferred save blocks settings mutations with `409`/`423`; Bluetooth-capable boards may require Configuration mode before POST. GET is read-only.

OLED preferences are stored in the adapter's `oled-v1` NVS record and persist across browsers. **The WebUI's keyboard remapping does not use this API**; it is saved locally in the browser and applied to outbound normalized controller states before sending them.

## Ownership and controller input

On a Bluetooth-capable board, first select Wi-Fi mode as described below and wait for the handoff. The Wi-Fi-only clone always uses Wi-Fi mode. Open `ws://192.168.4.1/ws`, then POST `{}` to `/api/control/acquire`. A successful response is:

```json
{"ok":true,"token":123451,"timeout_ms":250,"owner_timeout_ms":5000,"ping_supported":true}
```

The nonzero 32-bit token identifies the current owner. Another acquisition receives `409 control_busy`. Retrying acquisition with the current `token` returns that ownership without extending its timeout. Ownership lasts **5 seconds** after the last accepted input or heartbeat. This is separate from the **250 ms input deadline**: a fresh heartbeat never keeps a held button or stick active.

When a client is idle or temporarily paused, send this WebSocket message approximately once per second, or POST the same `token` to `/api/control/heartbeat`:

```json
{"type":"heartbeat","token":123451}
```

A valid heartbeat returns `ok:true`, `heartbeat:true`, `token`, `lease_ms` and `input_active`. Normal continuous input already refreshes ownership and needs no extra heartbeat. A successful input/heartbeat binds the token to its WebSocket; another live WebSocket using it is rejected as `control_stream_in_use`. Closing the bound socket releases ownership, while closing an unrelated monitoring socket does not. A short input gap neutralizes output but retains the token, allowing the same client to resume with a newer complete input state. After the ownership deadline, a new lease must be acquired. On Wi-Fi-only boards, the supplied clients may do this automatically after a transport failure while the user's control intent remains active; ownership conflicts stop recovery.

Acquisition can return `503 network_initializing` before any lease mutation. This means the ESP is finishing bounded initial station/address/mDNS setup or an already-running disruptive radio command. Read `network.control_ready` and `control_waiting_for`; retain user intent and retry with backoff. An mDNS failure releases the initial gate rather than blocking control forever.

Send complete states continuously, including unchanged states and neutral states. Read `network.input_hz` from `/api/status`, or `input_hz` from `/api/network`, for the transport's recommended rate: 30 Hz for the default 115200-baud ESP8285 connection and 60 Hz for the official CYW43 transport. Higher ESP UART rates can advertise a higher recommendation. Submit each state as a UTF-8 WebSocket text message or to `/api/control/input`:

```json
{
  "type":"input",
  "token":123451,
  "seq":1,
  "buttons":1,
  "dpad":0,
  "lx":0,"ly":32767,
  "rx":0,"ry":0,
  "lt":0,"rt":128
}
```

All ten numeric fields are required. `type` may be omitted; when present, it must be `"input"`. Unknown input fields are rejected.

| Field | Range or meaning |
|---|---|
| `token` | Integer 1–4294967295; must match the lease |
| `seq` | Integer 0–4294967295; increment for every update |
| `buttons` | Integer bit mask 0–4095 |
| `dpad` | Integer bit mask 0–15; opposite directions are invalid |
| `lx`, `ly`, `rx`, `ry` | Integers −32768–32767; zero is centered |
| `lt`, `rt` | Integers 0–255; zero is released |

**Positive X means right; positive Y means up.** The adapter converts wire coordinates before applying its existing shaping helpers. Remote buttons are digital; pressure-sensitive face-button values, motion and touchpad data are outside this protocol version.

| Button | Mask | Button | Mask |
|---|---:|---|---:|
| A | `0x001` | B | `0x002` |
| X | `0x004` | Y | `0x008` |
| L3 | `0x010` | R3 | `0x020` |
| Back | `0x040` | Start | `0x080` |
| LB | `0x100` | RB | `0x200` |
| System | `0x400` | Misc | `0x800` |

For OG Xbox output, the usual LB/RB mappings produce White/Black. Unsupported controls depend on the selected output driver. D-pad masks are Up=`1`, Down=`2`, Left=`4`, Right=`8`; combine adjacent directions for diagonals.

The first sequence value is unrestricted. Subsequent values must advance modulo 2³² by less than 2³¹. Duplicates, older packets, invalid inputs and wrong tokens never extend the lease. Successful input responses include `token`, `accepted_seq` and the latest Core0 `applied_seq`. WebSocket success acknowledgments are throttled to approximately twice per second and may be omitted while a response is pending; clients must not wait for an acknowledgment before sending their next state.

POST `{"token":123451}` to `/api/control/release` when finished. After 250 ms without a valid update, the adapter discards queued input and supplies neutral output for the entire remaining ownership reservation. Ownership ends at release, stream closure or the separate 5-second deadline. The selected source remains Wi-Fi and output remains neutral until another fresh session is deliberately started. The official board does not automatically resume Bluetooth after remote ownership ends; returning to Physical mode is an explicit action. Source changes include an 8 ms neutral handoff and coordinated radio/USB application.

A WebSocket binds to a token only after a successful input or heartbeat. A second live WebSocket cannot use that token (`409 control_stream_in_use`). Closing the bound connection releases its token; a stale close from an older connection cannot release a different owner. A failed HTTP status poll does not terminate a healthy control stream.

The WebUI clears held input on blur or when hidden and pauses input while retaining ownership with heartbeats. Returning to the page resumes from neutral; keys must be released/pressed again, and a gamepad must return to neutral before it is rearmed. Closing/leaving the page, pressing Release/Esc, or changing away from the Control tab releases the session and stops automatic recovery. A definitive ownership rejection also stops recovery. Firmware maintenance rejects acquisition and input with `423 maintenance_active` and holds USB output neutral.

### Connection health, independently of input

Firmware advertises `ping_supported:true`. Send this WebSocket text frame without a token:

```json
{"type":"ping","id":7}
```

The response is `{"ok":true,"type":"pong","id":7}`. IDs must be integers from 0 through 4294967295. This path does not acquire a token, bind the socket, extend ownership, or refresh input. It does not require the controller mailbox. Under backpressure, one pending application pong is retained and coalesced to the most recent ID; WebSocket protocol close/pong frames keep priority. Successful input/heartbeat replies can also prove connection health. A missing low-rate input acknowledgement alone must not terminate a stream that is answering health probes.

On a broken Wi-Fi-only stream, the supplied browser closes its old socket, tries to release only its remembered token, and acquires a fresh lease with bounded backoff (at most eight attempts). It sends neutral first and discards held input. Explicit release, an ownership conflict, a mode change, or maintenance ends automatic recovery. A stale token is never used to take another client's lease.

## Bluetooth source modes and calibration

`GET /api/control/mode` returns `mode`, `requested_mode`, `mode_pending`, `bluetooth_supported`, `bluetooth_connected`, `bluetooth_allowed`, and `usb_suppressed`. These mode fields also appear under `control` in `/api/status`.

POST `{"mode":"wifi"}`, `{"mode":"physical"}`, or `{"mode":"configure"}`. A pending handoff returns `202`; a settled/no-op request returns `200`. Wait for `mode_pending:false` before acquiring input or editing profiles. Changing mode clears the old lease and any unfinished calibration capture. The official board starts in Physical mode after boot; source choice is not persisted. Wi-Fi-only boards accept only `wifi` and return `409 bluetooth_not_supported` for the other choices.

| Mode | Bluetooth behavior | Console output |
|---|---|---|
| `physical` | Connection and input enabled | The physical controller through its profile/calibration |
| `wifi` | Existing controller disconnected; new controller connections inhibited | Fresh remote input, otherwise neutral |
| `configure` | Connection retained/allowed for measuring raw input | Neutral regardless of controller buttons/axes |

The calibration mode disconnects the physical controller **from console output**, while retaining its Bluetooth reports for measurement. Raw readings cannot be captured if the radio link itself is disconnected. Bluetooth-board profile edits, selection and saves, Wi-Fi scans/joins/settings, LED changes and OTA mutations require settled Configure mode (`409 configuration_mode_required` otherwise). Read-only status/settings remain available. Update cancellation and the deferred mDNS retry diagnostic remain available outside Configure mode for recovery; normal save/update guards still apply. Maintenance and pending-save guards apply to mode/calibration mutations as well.

`GET /api/calibration` exposes `supported`, `mode`, `profile_id`, `revision`, `applied_revision`, `dirty`, `phase`, `samples`, `connected`, `fresh`, `sample_age_ms`, `raw`, `calibration`, and `draft`. Raw axes use the same normalized X-right/Y-up ranges as remote input, **before** center/range correction, curves, deadzones or remapping. A connection generation prevents samples from an earlier controller connection being accepted as a fresh capture.

POST actions:

| Body | Effect |
|---|---|
| `{"action":"begin","mask":63}` | Start a new center/rest capture; mask bits select LX, LY, RX, RY, LT, RT |
| `{"action":"center"}` | Accept at least 32 stable center/rest reports and begin range capture |
| `{"action":"apply"}` | Validate measured ranges and apply the draft in RAM through the profile handshake |
| `{"action":"cancel"}` | Discard the unfinished capture |
| `{"action":"reset"}` | Restore identity calibration in RAM |

The mask is 1–63 and defaults to all six controls. Unselected controls use identity calibration. Center capture rejects movement greater than 2048 stick units or 8 trigger units. A valid stick range must extend at least 4096 units on both sides of its center; trigger span must be at least 32 units. Both capture stages require at least 32 reports. A disconnected/stale controller, controller replacement, unstable center or incomplete range rejects application rather than silently saving bad values.

Calibration is a separate 30-byte versioned record for each profile slot, preserving the existing UserProfile layout. Apply/reset changes RAM and advances the shared profile revision. Existing `/api/profile/save` persists the applied profile and its calibration together before restart. The Bluetooth-only calibration never shapes remote Wi-Fi input a second time. Button/D-pad destinations continue to use the existing profile API; the WebUI presents those fields in a dedicated Remapping tab.

## Profiles and live application

`GET /api/profile` returns `profile`, `revision`, `applied_revision`, `applied_profile_id`, `dirty`, `save_pending`, `recovered_invalid_profile`, and `effective`. `GET /api/profiles` lists all eight slots plus requested `profile_id` and `applied_profile_id`. POST `{"id":3}` to `/api/profile/select` to load saved slot 3 into RAM. This discards the current unsaved editor contents; the UI requires an explicit **Load saved profile** action. The response has the same shape as `GET /api/profile`.

Stored profiles are cached before Core1 starts, so loading never touches NVS from a network callback. A corrupt slot recovers to valid defaults with its own slot ID. Selection uses the normal two-core handshake: requested `profile_id` can change before `applied_profile_id`. The status LED follows the actual Core0-applied ID. Selecting or editing alone does not persist; Save writes that slot and makes it active after reboot. Changing `profile.id` in an edit still chooses the slot written by Save.

A partial update preserves omitted fields:

```json
{
  "revision":1,
  "profile":{
    "joystick_settings_l":{"dz_inner":0.08,"curve":1.0},
    "trigger_settings_r":{"dz_inner":0.05},
    "button_a":2
  }
}
```

The optional `revision` provides optimistic concurrency: a mismatch returns `409 profile_revision_changed`. Unknown fields and invalid types are rejected. A changed profile returns `202` with a new revision; poll until `applied_revision == revision`. A no-op already applied returns `200`. Revisions restart after reboot, so fetch them again after reconnecting.

The left/right namespaces are `joystick_settings_l`, `joystick_settings_r`, `trigger_settings_l` and `trigger_settings_r`. Their numeric values use human-readable decimals, quantized internally to 16.16 fixed point.

| Namespace | Fields | Accepted range |
|---|---|---|
| Stick/trigger | `dz_inner` | 0–0.95 |
| Stick/trigger | `dz_outer`, `anti_dz_outer` | 0.01–1 |
| Stick/trigger | `curve` | 0.05–4 |
| Stick | `anti_dz_circle`, `anti_dz_square`, `anti_dz_square_y_scale` | 0–0.95 |
| Stick | `anti_dz_circle_y_scale` | 0–1 |
| Stick | `axis_restrict` | 0–0.0095 |
| Stick | `angle_restrict`, `anti_dz_angular` | 0–0.89 |
| Stick | `diag_scale_min`, `diag_scale_max` | 0–2 |
| Stick | `uncap_radius`, `invert_x`, `invert_y` | Boolean |
| Trigger | `anti_dz_inner` | 0–0.95 |

Restriction fields retain legacy serialized units: the shaping code multiplies `axis_restrict`, `angle_restrict` and `anti_dz_angular` by 100. Do not interpret their API values directly as percentages or degrees.

For both sticks and triggers, `dz_inner` must be below `dz_outer` and `anti_dz_outer`. Stick circle/square anti-deadzones must remain below the output outer range. Combined circle/ellipse and square compensation is also validated to avoid invalid shaping divisions. A half-unit fixed-point rounding tolerance permits exported profiles to round-trip.

Top-level profile fields include:

- `id`: integer 1–8; `analog_enabled`: Boolean.
- `button_a/b/x/y/l3/r3/back/start/lb/rb/sys/misc`: destination masks 0–4095. Zero unmaps; combined masks are permitted.
- `dpad_up/down/left/right`: destination masks 0–15, excluding opposing directions.
- `analog_off_up/down/left/right/a/b/x/y/lb/rb`: integer indices 0–9. Their canonical order is Up, Down, Left, Right, A, B, X, Y, LB, RB.

Booleans also accept numeric 0/1. Untouched XInput sticks use mode-specific effective defaults: left/right inner deadzones 0.08/0.09, curve 0.35 and circular clamping. `effective.xinput_stock_l/r` identify this behavior; raw default profile values alone do not describe it.

Application uses a two-core handshake between the USB-output core and the board's radio runtime. The physical and remote profiles are updated at the coordinated handoff before the applied revision is published. USB output continues. New Bluetooth reports use the new mapping; unchanged already-shaped reports are not reverse-transformed. The clone has only the remote-input source.

## Explicit save and restart

Once the requested revision is applied, POST `{"revision":2}` to `/api/profile/save`. The `202` response reports `restart_required:true` and `saving:true`. Further edits are blocked while saving is pending.

The required response-completion signal depends on the transport. On the official CYW43 board, the **entire HTTP response must be acknowledged by the TCP peer**. On the ESP8285 clone, the module must accept the complete response through its AT `SEND OK` results. That is not proof that the PC received or acknowledged those bytes. The runtime keeps the network running for one additional second, then performs the USB-disconnect/flash-save/restart lifecycle. `network.save_delivery` reports `peer_acknowledged` or `module_accepted` to make this distinction visible.

A connection or send failure before the required completion signal cancels the pending save. The response acknowledges scheduling, not completed persistence: reconnect and read the profile after restart to verify it. Slider changes alone never write flash.

## Wi-Fi configuration and local name

`GET /api/network/config` returns `station_enabled`, `station_ssid`, `hostname`, `password_saved`, and `pending`. It never returns saved credentials. POST a partial settings object to change them:

```json
{"station_enabled":true,"station_ssid":"Home WiFi","station_password":"your-password","hostname":"dc-ogx-w"}
```

Omitting `station_password` keeps the saved value; an empty string explicitly selects an open network. `{"forget":true}` clears saved credentials and disables STA. Hostnames are normalized single DNS labels of up to 32 characters. SSIDs support up to 32 UTF-8 bytes. Passwords accept 8–63 printable ASCII characters, 64 hexadecimal digits, or empty for open networks. A successful response schedules saving and reboot only after the full reply completes; reconnect and query settings after restart. The recovery AP is available for joining and automatically enters standby when the station has a usable address; it returns after a sustained station outage.

`network.ap_active` reports whether the onboard recovery SoftAP is currently enabled (it is **off** after a confirmed home-network join, and restored after a sustained disconnect). `network.station_connected` requires a known IPv4 address. `station_joining`, `station_retry_pending`, `station_ip`, `ap_ip`, `hostname`, `mdns_available`, `station_error`, `station_reconnects`, and `recoveries` expose connection state. `ip` selects the station address when connected, otherwise AP. The intended full URL is `http://dc-ogx-w.local/`; `.local` also depends on client/network multicast support. `network.channel` is the configured AP channel, which may differ from the effective channel during AP+STA.

The clone performs one bounded initial mDNS stop/start attempt after obtaining the station address before granting a fresh control lease. It then retries failures at 5/10/20/40/60-second backoff independently of station-address polling. Later attempts defer while control/update/save owns the radio. An address change queues responder restart. Added diagnostics are:

| Field | Meaning |
|---|---|
| `control_ready`, `control_waiting_for` | Acquisition eligibility and active setup/scan/synchronization reason |
| `mdns_state` | `not_configured`, `waiting_for_station`, `queued`, `starting`, `active`, `retry_wait`, `deferred`, or CYW43 `failed` |
| `mdns_error` | Explicit rejection, stop/start timeout, disconnection or official-board name conflict/start failure |
| `mdns_attempts` | Actual responder-start/restart attempts |
| `modem_firmware` | Bounded sanitized AT version when reported by GMR; empty on CYW43 |
| `command_timeouts` | ESP runtime command deadlines, including recoveries that preserve other sockets |

`mdns_available:true`/`active` means native startup was accepted on ESP or probing succeeded on CYW43. It is not proof of successful resolution by a remote computer. A rejected command is distinct from a timeout and is not labeled as proven lack of modem support.

POST `{}` to `/api/network/mdns/retry`. The request returns `202 {"ok":true,"mdns_pending":true}` and is handed to the transport after complete reply delivery plus 250 ms. Abandoned replies do not start work. Repeated requests coalesce, and an existing owner defers actual radio work. The clone requires a connected station with a known address; the official board may restart its AP responder when no station is configured. A pending save or active update blocks the mutation. This operation does not save settings or reboot.

POST `{}` to `/api/network/retry`. An unconfigured station returns `409 station_not_configured`; an already connected station returns `200` with `already_connected:true`; an accepted request returns `202` with `retry_pending:true`. No manual retry is handed to the transport until the whole reply completes plus a 250 ms settling interval. Repeated retries coalesce. Actual remote ownership, updates and pending saves defer radio joins; idle dashboard/WebSocket connections do not. Release ownership before retrying if control is active. A queued/already-in-progress attempt does not imply successful association; poll connection status.

The clone now requires successful completion of its intended `CWJAP` plus a valid station address before setting `station_connected`. Unsolicited events from an inherited modem association do not qualify. `network.station_waiting_for` is empty or one of `control`, `update`, `save`, `scan`, `radio`, `retry_backoff`, or `command_queue`. The `station_waiting_for_address` status code means DHCP/address acquisition is in progress. The official board reports `station_dhcp_timeout` if an associated link does not obtain an address before its deadline.

## Nearby-network discovery

POST `{}` to `/api/network/scan`. An accepted or coalesced request returns `202` with `{"ok":true,"scan_pending":true,"generation":1}`. A radio that is not ready returns `503 network_not_ready`. Update and pending-save gates apply as for other network mutations. The UI releases its own controller ownership first; other owners must release their own session.

The transport receives the request after the complete response and a 250 ms settling interval. GET exposes the queued generation while the response is pending, so old results cannot masquerade as the new scan. Abandoned responses cannot initiate a scan. Coalesced and delayed responses cannot repeat work after its generation has already been dispatched.

GET `/api/network/scan` returns:

```json
{
  "ok":true,"state":"complete","generation":1,"age_ms":250,
  "error":"","blocked_by":"","truncated":false,
  "networks":[
    {"ssid":"Home WiFi","rssi":-48,"channel":6,"security":"wpa2","supported":true}
  ]
}
```

States are `idle`, `queued`, `running`, `complete` and `failed`. An empty completed list is valid. While queued, `blocked_by` identifies `control`, `update`, `save`, `joining`, `radio` or `command_queue`. Results are cached in RAM. Up to 16 strongest visible networks are retained, ordered by RSSI and deduplicated by SSID/security or BSSID. `truncated` reports omitted weaker entries. Names must be 1–32-byte visible UTF-8; hidden/non-text names are omitted. Hidden networks can be entered manually.

Security values are `open`, `wep`, `wpa`, `wpa2`, `wpa_wpa2`, `wpa3`, `wpa2_wpa3`, `enterprise` and `unknown`. `supported` screens out known-incompatible join modes; it does not guarantee authentication. CYW43 returns coarse WPA/RSN flags and cannot reliably distinguish WPA3 or enterprise from RSN, so the UI labels `wpa2` as **WPA2 / RSN**. The official build joins open or WPA2-PSK/AES networks. The clone uses its modem firmware's personal-network capabilities.

ESP scanning uses `CWLAP`, with optional setup and legacy fallback. Its scan command deadline is 15 seconds; the official asynchronous scanner uses 20 seconds. Deadlines begin when actual scanning starts. Queued scans wait for control and ongoing joins. ESP HTTP replies can be delayed while scanning occupies the modem command pipe. Reconcile a lost start reply with GET/generation before repeating POST. Fixed failure codes include `scan_timeout`, `scan_failed`, `scan_response_invalid` and `scan_not_ready`.

## LED diagnostics and profile colours

The default data pin is **GPIO8**, selected through `OGXM_STATUS_LED_PIN`. All WebUI labels read `network.led.pin` from the running firmware. Reconfigure an existing build with the supplied preset to update its old cached pin.

`GET /api/led` returns `enabled`, `brightness` (0–255), `mode` (`combined`, `profiles`, or `diagnostics`), eight `profile_colors` strings and `diagnostic_colors`, plus `revision`, `applied_revision`, `dirty`, `save_pending` and `test_active`. Colours use `#RRGGBB` format. Diagnostic keys are `starting`, `access_point`, `joining_wifi`, `reconnecting_wifi`, `wifi_failed`, `usb_disconnected`, `input_paused`, `updating`, `error`, `profile_changed`, `wifi_connected` and `control_active`.

POST a partial object to `/api/led/apply`, for example `{"enabled":true,"brightness":24,"mode":"combined","diagnostic_colors":{"joining_wifi":"#FF6E00"}}`. Omitted fields are preserved. If `profile_colors` is supplied, all eight entries are required. Unknown fields and invalid values are rejected. Changes enter a nonblocking mailbox; Core0 applies them between USB service visits. Poll revisions to confirm application. Live changes never write flash.

POST the settings object (or `{}` for current settings) to `/api/led/save` to schedule persistence after the full response and restart. An incomplete response cancels persistence; any already-applied RAM edits remain dirty. LED settings use a separate 76-byte versioned/CRC-protected NVS record. Missing or corrupt records load defaults without writing flash.

POST `{"color":"#00FF00","duration_ms":2000}` to `/api/led/test`. Duration is 1–5000 ms, default 1500 ms. Preview returns to the policy automatically, works even when the normal LED is disabled, and uses the applied brightness. Update/error indications override preview. No preview writes flash.

`network.led` reports `available`, `pin`, `state`, `profile_id`, `profile_color`, `output_color`, `brightness`, `enabled`, `mode` and `applied_revision`. `profile_color` is unscaled; `output_color` is the brightness/pattern-scaled value sent to the pixel. In combined mode, steady profile colour remains visible during fresh control; diagnostics override meaningful faults/transitions. Profile-only mode suppresses ordinary network/USB states; enabled update/error indications still override it. The README lists default patterns.

## Firmware updates

The WebUI uses the board-specific `.ogxw` package generated by CMake. The complete API and format are documented in [tools/firmware/README.md](../tools/firmware/README.md). Upload, verification and final installation are separate operations. Firmware input is chunked within the normal HTTP body limit. Profile/network/LED mutations and controller acquisition are blocked while an update owns maintenance. A failed or interrupted upload leaves the running application intact. Installation preserves settings but is not power-fail-safe A/B replacement; BOOTSEL remains the recovery path.

## Telemetry, errors and limits

`/api/status` includes versions, output mode, profile revisions, lease/counter information, `output_source`, `processed`, `physical`, rumble and `network`. `processed` is the state consumed by the adapter output driver; it does not prove the console has sampled or acted on it. Telemetry uses non-consuming snapshots and +Y-up coordinates.

Within `control`, `active` indicates reserved ownership and `input_active` indicates fresh remote output. `lease_ms` is the remaining ownership time; `input_remaining_ms` is the remaining input time. `timeout_ms` stays 250, while `owner_timeout_ms` is 5000. `expired` counts stale-input transitions and `owner_expired` counts complete session timeouts. Heartbeats can keep `active:true` while `input_active:false`; this is intentional. `maintenance` reports update/maintenance inhibition.

The `network` object is also returned by `/api/network`. It includes `board` (`pico_w` or `pico_w_2023`), `transport`, `bluetooth_available`, `uart_baud`, `input_hz`, `save_delivery`, `error`, and the existing AP/readiness/connection counters. A UART transport becomes ready only after its AT initialization and server setup complete. Check `ready` and `error` rather than assuming that a successful RP2040 startup means the ESP module is operational.

Errors are JSON objects with `ok:false` and `error`. Typical statuses are `400` for invalid input/profile, `409` for ownership/sequence/revision conflicts, `503 service_busy` for transient mailbox contention, and `429` for excess WebSocket streams. Retry transient contention without extending the input sequence backwards.

Limits: one input owner; three TCP clients, at most two WebSockets; 2048-byte HTTP headers; 4096-byte HTTP bodies/responses; 1024-byte WebSocket messages; an eight-state input queue. Queue overflow drops the oldest state. JSON parsing is bounded to 192 tokens and six nested container levels. Binary WebSocket messages are rejected. Send HTTP request bodies with Content-Length framing; transfer-encoded bodies are unsupported. Requests with a cross-origin `Origin` are rejected; the supplied PC bridge serves its UI and API on one origin.
