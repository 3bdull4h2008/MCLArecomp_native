# MCLA PC Recomp Launcher — Host Integration Handoff

## 1. Window Geometry & Transparency
- **Fixed Size**: `650 × 446` (`resizable: false`, `decorations: false`).
- **Transparency Mode**: Load `index.html?transparent=1` or add `transparent-window` to `document.body.classList` (and `document.documentElement.classList`).
  - `html.transparent-window`, `body.transparent-window`, and `.launcher-stage` all clear to `background: transparent !important` with outer box-shadows/drop-shadows disabled so WinForms `TransparencyKey = Color.Magenta` + `DefaultBackgroundColor = Color.Magenta` keys out cleanly without dark-magenta halos.
  - **True Per-Pixel Alpha (Optional)**: In WebView2, set `webView.DefaultBackgroundColor = System.Drawing.Color.Transparent;` on a host window that supports per-pixel alpha (e.g., WPF `AllowsTransparency="True"` / WinUI 3 / DirectComposition `WS_EX_NOREDIRECTIONBITMAP`). Standard WinForms `TransparencyKey` does not support per-pixel alpha over WebView2, so keep the color-key fallback when using WinForms.

## 2. Borderless Window Dragging (`dragWindow`)
- Clicking and dragging the top bezel strip (`.window-drag-strip`, excluding the `–` / `✕` buttons and tabs) has `-webkit-app-region: drag` and emits:
  ```json
  { "action": "dragWindow", "payload": { "extra": { "toml": ..., "cliArgs": ... } } }
  ```
- In WinForms (`FormBorderStyle = FormBorderStyle.None`), handle `action == "dragWindow"` by calling `ReleaseCapture(); SendMessage(this.Handle, 0xA1 /* WM_NCLBUTTONDOWN */, 0x2 /* HTCAPTION */, 0);`.

## 3. Two-Way Config Hydration (`ready` + `loadConfig`)
- On startup (after schema initialization), the UI emits `{ "action": "ready", ... }` **once**.
- When the host receives `ready`, read `cache/mcla.toml` and push it into the WebView via:
  ```js
  window.MCLALauncherAPI.loadConfig(tomlJson);
  ```
- `loadConfig` accepts UI-grouped (`{ display: { ... } }`), TOML-grouped (`{ graphics: { ... }, display: { ... } }`), or flat (`{ renderer_mode: "vulkan", mcla_fps: 60 }`) objects and reverse-maps `valueMap` values back to UI labels automatically.

## 4. Action & Payload Contract
Every action dispatched via `MCLAHostBridge` preserves:
- `payload.extra.toml`: sectioned object mapped to real `mcla.toml` / CVar keys (`renderer_mode`, `resolution_scale`, `mcla_fps`, `fullscreen`, `vsync`, `prompts`, `vibration`, `mcla_adaptive_triggers`, `mcla_ui_interpolate`, `mcla_intro`, `deadzone`).
- `payload.extra.cliArgs`: array of `--<cliFlag>=<mappedValue>` flags.
- Actions: `ready`, `dragWindow`, `launchGame`, `saveConfig`, `optionChanged`, `restoreDefaults`, `browseFiles`, `openLogs`, `minimizeWindow`, `exitLauncher`.
- Unwired actions (`prepareShaders`, `rebuildGame`, `editCliFlags`) are hidden (`"visible": false`) in `launcher.schema.json`.
