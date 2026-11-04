# DC-OGX-W Windows Companion v0.1.3

This is a **separate Windows-only Tauri 2/Rust project**. It does not modify the RP2040 firmware, the existing WebUI or the Linux/macOS Python companion.

## Build environment

- Windows 10/11 x64; Microsoft Edge **WebView2 Runtime** (usually already installed)
- Node.js LTS (includes `npm.cmd`)
- Rust with `stable-x86_64-pc-windows-msvc`
- Visual Studio C++ Build Tools workload **Desktop development with C++** and Windows SDK

From PowerShell in this folder:

```powershell
node --version
npm.cmd --version
rustc --version
cargo --version
.\build-portable.ps1
```

Or manually:

```powershell
npm.cmd install
npm.cmd run dev
npm.cmd run build
```

The build script copies the **single native executable** to `dist-portable/DC-OGX-W.exe`. No MSI/NSIS installer is created. The embedded HTML/CSS/JS is included in the EXE. WebView2 is a separate Windows runtime, **not embedded**.

**Portable preferences:** Tauri is configured with `appDirectoriesOverride: "./app-data"` so WebView2 files and app preferences remain beside the EXE. The executable folder must be writable. Do not run from `Program Files` or a locked-down network share.

## First-run checklist

1. Start the application. Enter the current adapter IP or click **Discover** (both board variants with UDP-capable firmware).
2. Confirm the firmware version, network address and mode.
3. On Bluetooth-capable boards, switch to **Wi-Fi remote** mode before taking control. The ESP8285 variant is Wi-Fi-only.
4. Open **Control**, select Keyboard or an XInput slot, then **Take control**. Esc or Release immediately neutralizes input.
5. Open **LED** or **Device** to Apply session changes or **Save & restart** to make them persistent. Save is not a preview.
6. Use **Wi-Fi** to edit SSID/hostname and restart. The adapter’s AP/STA behavior depends on its firmware; use the current IP displayed by the adapter.

## Native implementation

- `src-tauri/src/network.rs`: exact UDP discovery wire protocol, HTTP API bridge, allowlisted routes
- `src-tauri/src/control.rs`: WebSocket ownership, input snapshots, deadman-aware neutralization, keyboard or native XInput
- `src-tauri/src/main.rs`: Tauri commands and local preferences storage
- `ui/`: standalone frontend, eight original firmware WebUI themes; actual OG Xbox White/Black buttons, keyboard mapping, device panels
- `tests/check-ui.cjs`: portable checks with Node (no external test dependencies)

The firmware's remote API v2 is required. Input snapshots use the firmware's **+Y-up** convention, and 60 Hz or the board-advertised lower rate. This initial build **does not auto-reacquire ownership after a connection failure**. That is intentional: an ambiguous lost HTTP acquire response must not be replayed. After a drop, release input and manually take control again.

## v0.1 scope boundaries

The first build supports connection/discovery, device health, native XInput (slot 1-4) and keyboard control, key rebinding, saved profile selection, input mode switching, OLED brightness/timeouts/apply/save, RGB profile and diagnostic colors/apply/save, and Wi-Fi network configuration.

The **full profile editor/remapper**, **Bluetooth calibration flow**, **firmware uploader** and advanced diagnostics are scheduled for a later version; use the embedded WebUI for those operations today. The Linux/macOS Python script has not been modified.

## Verification

```powershell
npm.cmd run check:ui
cd src-tauri
cargo test
```

`cargo test` and actual Windows EXE production must run with the configured Windows build tools. Hardware networking, XInput, and WebView2 behavior still require a real board test.
