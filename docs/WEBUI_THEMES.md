# Embedded WebUI and appearance

The WebUI is the single embedded, fully offline HTML document [`src/NetworkService/web/index.html`](../src/NetworkService/web/index.html). It is compressed and embedded into RP2040 firmware by `src/NetworkService/embed_web.py`. The ESP8285 AT path is sensitive to simultaneous requests, so **do not** add remote fonts, CDNs, icon requests, or additional live status polling for appearance.

## Navigation

- Desktop: left column with Control, Profiles, Wi-Fi, LED, Device and the Appearance selector. The existing DOM tab IDs and API handlers are preserved.
- Mobile: a compact header with a menu control and an accessible drawer. Selecting a destination closes it, retaining room for actual controller controls.
- The DXC emblem is an embedded data URI; it does not create a second network request.

## Eight themes

| Theme | Appearance |
|---|---|
| Darkone Green | Dark charcoal, vivid lime-green accents, angular hardware feel |
| Theia-inspired Dark | Restrained slate/blue developer-tool style; no copied Theia assets |
| Midnight Blue | Navy with cyan-blue accents |
| Synthwave | Purple/magenta with rounded luminous surfaces |
| Ember | Warm charcoal/copper/orange industrial styling |
| Retro Terminal | Near-black, green, monospace and minimal radii |
| Theia-inspired Light | Neutral light workbench palette |
| Arctic Light | Cool white and blue with generous spacing |

Theme selection sets CSS variables and `data-theme` on the document root. `localStorage['dc-ogx-w-theme']` persists the selection **per browser origin** when storage is available. No API call or adapter flash write occurs. It is not a profile setting; browser theme preferences do not roam between devices. A storage-denied browser can still select a theme for the current tab.

## Profiles: live response preview

The embedded Profiles editor shows live XY stick plots and LT/RT progress while editing deadzones and response curves. It reads `navigator.getGamepads()` on the device running the browser and uses a **local approximate transformation** for inner/outer deadzones, response curve and inversion. The adapter still performs its actual native profile processing. It does **not** claim to show USB output measured on the RP2040, nor does it implement a full firmware shaping simulator.

This feature is intentionally local to prevent additional ESP8285 AT traffic. Physical wired/BT input arriving **at the adapter** is not available to this browser preview until device-side input telemetry is explicitly added. A browser connected to an actual gamepad reports its source in the preview. When unavailable, the panel shows guidance rather than fake readings.

## Testing and constraints

Run `python tests/web/test_branding.py` to verify required IDs, themes, inline assets and embedded size. With Playwright and Chromium installed, run `python tests/web/test_layout.py` for desktop/mobile behavior using mocked device APIs and a synthetic locally connected controller. The browser test uses an opaque origin, so it does **not** prove localStorage survival across separate launches. Confirm that on physical hardware.

Keep user feedback next to the setting being edited. Preserve keyboard focus, touch targets, controller controls and mobile responsiveness. Never make UI theming depend on an unverified server response.
