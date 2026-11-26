# Release manifest — MCLA-PC

## 1. Release name (decided, with evidence)

Official short title: **Midnight Club: LA**
Edition descriptor: **Complete Edition**
Release display name: **Midnight Club: LA — Complete Edition (PC release)**

Evidence (guest bytes are source of truth — see AGENTS.md rule 3):

| Claim | Evidence |
|---|---|
| Guest XSTR title is "Midnight Club: LA" | `build/cache/mcla_pe.bin` @ file offset 9779362 → VA `0x829538A2` reads `Midnight Club: LA`, followed by `Rockstar Games` / `Rockstar San Diego` publisher strings. Same string at file offset 116476 → VA `0x8201C6FC` |
| Publisher credit alongside the title | `Rockstar Games` @ offset 9779444, `Rockstar San Diego` @ offset 9779462 (XSTR block) |
| Project config name differs | `config/mcla_manifest.toml:2` says `Midnight Club: Los Angeles` — full-spelling variant, not the guest's `LA` abbreviation |
| "Complete Edition" is a retail SKU / folder name, not the guest title | `docs/XEX_ANALYSIS.md:5` — dump folder `Midnight Club - Los Angeles - Complete Edition (USA, Europe)/default.xex`. Zero hits for `Complete Edition` in `mcla_pe.bin` (measured 2026-11-09) |
| Title-screen logo wording | `docs/EXTERNAL_AI_BRIEF.md:56` records the on-screen logo as "Midnight Club Los Angeles Complete Edition" from pixel-diff (observed rendering, distinct from the XSTR table) |
| Project identity shorthand | `docs/HANDOFF_NEXT_AGENT.md:27` — `Midnight Club: Los Angeles (Xbox 360 Xenon) native PC port` |
| No plaintext legal copyright in the guest image | `Complete Edition` 0 hits, `Take-Two` 0 hits, `Rockstar Games, Inc.` 0 hits, `(C)` 1 hit (zlib), `©` 15 hits (fonts/UI) in `mcla_pe.bin`. The intro legal page ships inside `legals.xsf` (Scaleform) and is not plaintext in the PE — its exact copyright line is therefore UNVERIFIED offline and must not be quoted in the release manifest until read from a boot capture. |

Recommended strings for the release (descriptive, not trademark claims):

- Product display name: `Midnight Club: LA — Complete Edition (PC release)`
- Folder name: `Midnight Club LA - Complete Edition (PC Release)`
- EXE file name: keep `mcla.exe` for this release (renaming breaks the
  soak/tooling paths that hardcode it — see §4). Surface the friendly
  name via Windows version info (`FileDescription` / `ProductName`) and
  the install folder instead.

### Version-info snippet (NOT wired in — for the next authorized build)

```rc
1 VERSIONINFO
 FILEVERSION 1,0,0,0
 PRODUCTVERSION 1,0,0,0
 FILEFLAGSMASK 0x3fL
 FILEFLAGS 0x0L
 FILEOS 0x40004L
 FILETYPE 0x1L
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "040904B0"
        BEGIN
            VALUE "FileDescription", "Midnight Club: LA - Complete Edition (PC release)"
            VALUE "ProductName", "Midnight Club: LA - Complete Edition (PC release)"
            VALUE "CompanyName", "MCLA-PC project"
            VALUE "FileVersion", "1.0.0.0"
            VALUE "ProductVersion", "1.0.0.0"
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x409, 1200
    END
END
```

Do not put `Rockstar` or `Take-Two` in `CompanyName` — that would misclaim
authorship. The guest-credit lines above are the only vendor names the
image itself carries.

## 2. Icon set (delivered)

Original artwork, multi-size, tiny-legible. License: `src/icon/LICENSE.txt`
(also embedded as a PNG `License` tEXt chunk by `src/icon/make_icons.py`).

| File | Sizes | Role |
|---|---|---|
| `src/icon/mcla-release.ico` | 16 / 32 / 48 / 64 / 128 / 256, 32 bpp, 9,973 B | Wired into the exe resource (see §3) |
| `src/icon/mcla-release-16.png` | 16 px | Taskbar / tray source (drawn natively at 16 px) |
| `src/icon/mcla-release-32.png` | 32 px | Taskbar / Explorer source |
| `src/icon/mcla-release-48.png` | 48 px | Explorer medium-icons source |
| `src/icon/mcla-release-64.png` | 64 px | Large-icons source |
| `src/icon/mcla-release-128.png` | 128 px | Extra-large source |
| `src/icon/mcla-release-256.png` | 256 px | Vista+ jumbo / source artboard |
| `src/icon/make_icons.py` | — | Regenerator + license string (deterministic) |

Replaced by: the previous single-frame `icon.ico` (256 px only, 270,398 B,
SHA256 `5D06CEEA…B42869`, `mcla.rc:1` before this change). That file is
left on disk untouched for rollback. It had exactly one image entry, so
Explorer scaled a 256 px bitmap down to 16 px; the new set ships a native
16 px frame instead (PIL verify: 32/244 white-M pixels at 16 px).

Design: midnight-navy rounded tile (top `#101A38` = boot clear colour
16,26,56 from `docs/HANDOFF_3D_ASSETS.md:4`), bold white "M" (Arial Black),
gold accent bar. No Rockstar/Take-Two marks or retail textures (see
`LICENSE.txt`).

## 3. Wiring (what changed, minimal)

- `mcla.rc:1` — `IDI_ICON1 ICON "icon.ico"` → `IDI_ICON1 ICON "src/icon/mcla-release.ico"`
  (this is the exe's main icon group; it drives the Explorer desktop icon,
  the Start-menu tile, and the taskbar running-window icon).
- `CMakeLists.txt:257` — `DEPENDS ... icon.ico` → `DEPENDS ... src/icon/mcla-release.ico`
  so a rebuild picks the new set up. No other CMake change.
- Debug icon: Debug/Release share the one `mcla.rc` → one `MCLA_RC_RES`
  (`CMakeLists.txt:251-259`), so the debug build now also embeds the
  multi-size set. The old `icon.ico` remains on disk if a future change
  wants to split icons per configuration (that would need a per-config
  `MCLA_RC_RES` — a larger CMake change, deliberately not made here).

Verification (standard tooling, no `mcla.exe` rebuild):
- ICO header parsed: `reserved=0 type=1 count=6` entries 16/32/48/64/128/256 @ 32 bpp.
- `PIL.Image.open` decodes all six PNG sources and the `.ico` (RGBA).
- `llvm-rc` compiles the edited `mcla.rc` standalone (see session log) —
  resource compile only; exe link deliberately untouched.

## 4. What to verify on a clean install

1. Fresh checkout: `src/icon/mcla-release.ico` exists; `mcla.exe` Properties
   → Details shows FileDescription/ProductName (only after the version-info
   snippet in §1 is wired — verify the icon regardless).
2. Explorer: desktop shortcut, Start-menu entry, and taskbar all show the
   navy/gold "M". Test at 100% and 150% DPI — 16 px must still read as "M".
3. Taskbar pin + unpin: pinned icon matches the running-window icon
   (same AppUserModelID group). Re-pin after install; Windows caches the
   old icon until the shortcut is rebuilt.
4. Run from a folder with spaces (`Program Files\Midnight Club LA...`):
   icon loads, no missing-resource error (rc path has no spaces).
5. `sigcheck`-style check: `mcla.exe` resource tree contains `IDI_ICON1`
   with 6 image entries across the five sizes.
6. No leftover `icon.ico` reference: `mcla.rc` and `CMakeLists.txt:257`
   both name `src/icon/mcla-release.ico`.

## 5. Risks (3)

### Risk 1 — Icon licensing
The set is original and ships under `LICENSE.txt`, but the *game name*
itself is a third-party trademark. Using "Midnight Club: LA — Complete
Edition" as an install-folder / FileDescription string is descriptive
naming of the emulated title, not an endorsement claim — yet storefronts,
AV vendors, or automated content ID can still flag it. Keep the
"Not affiliated with Rockstar Games" line in `LICENSE.txt`, never add
Rockstar/Take-Two to `CompanyName`, and do not later swap in a traced
retail logo without a fresh license review. Trademark dilution is the
risk, not copyright on this artwork.

### Risk 2 — Taskbar pinning parity
Windows keys the taskbar button to the pinned shortcut, not to the running
exe. Users pinned to the old single-frame `icon.ico` keep that cached
bitmap (and the old AppUserModelID grouping) after the icon changes, so
the release can show two different icons for the same app, or a pinned
tile that never updates. There is no supported "push new icon to pinned
apps" API — the mitigation is the §4 re-pin check and, if the installer
ships one, having the installer rebuild the Start-menu/taskbar shortcuts
from the new exe. Document "re-pin after upgrade" in release notes.

### Risk 3 — Desktop-shortcut creation perms
Three different privilege paths, three different failure modes:
(a) per-user `Desktop` (`%USERPROFILE%`) needs no admin but is silently
skipped by some enterprise policies; (b) all-users `Desktop` (Public)
needs elevation, so a non-elevated installer must either prompt or
defer; (c) per-machine redirected folders (OneDrive Desktop) need the
*user's* token, not the installer's. A shortcut written for the wrong
principal shows as a broken/blank icon (the `.lnk` points at a path the
viewer cannot resolve) even when the exe icon is correct. Ship no
desktop shortcut by default or write only the per-user one, and verify
on a clean machine with a non-admin account before claiming support.
