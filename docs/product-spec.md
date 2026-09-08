# GameHQ — Product Specification

This specification describes the intended GameHQ experience. The changelog is
the source of truth for shipped release behavior.

## 1. Product Description

GameHQ is a **portable native Windows app** for game screenshots, replay clips, and an in-game controller-friendly capture gallery — a PC equivalent of the PlayStation capture experience:

```txt
Tap DualSense Share      → screenshot
Hold Share 2 s           → save last 5 minutes of gameplay
PS button                → open GameHQ overlay above the current game
PS again / Circle        → close overlay, return to game
Controller               → browse captures inside the overlay
Triangle                 → favorites · per-game grouping · tray/background mode
```

GameHQ must **not** feel like a desktop app you alt-tab into. The primary UX is the **in-game overlay**; a normal desktop window exists only for settings, folder management, imports and advanced config.

Sibling app: AudioHQ (C++, audio routing). GameHQ follows the same native-first philosophy.

## 2. Technology Stack

```txt
Language:        C++20/23        UI:       Qt 6 + QML
Database:        SQLite          Build:    CMake, Ninja
Capture:         Windows Graphics Capture
Video encoding:  Media Foundation (GPU encoder where possible)
Audio capture:   WASAPI loopback
Input:           Raw Input / HID (XInput fallback)
Packaging:       Portable Windows build
```

Native C++ (not Electron) because the hard parts are all native: window capture, encoding, replay buffer, loopback audio, HID input, global hotkeys, tray, topmost overlay, focus restoration, portable runtime.

## 3. Core Requirements

### 3.1 In-game overlay (core requirement)
Borderless, frameless, always-on-top, controller-first, fast to open/close, couch-readable, usable without mouse. PS toggles it; it must never switch to the normal desktop window.

### 3.2 Close behavior
- **PS**: toggle overlay.
- **Circle**: back within overlay menus; in main gallery → close; in fullscreen viewer → exit fullscreen first.
- On close: hide window → restore focus to the previous game window → input returns to the game.

### 3.3 Input capture while overlay is active
Overlay open ⇒ GameHQ consumes controller input; the game must not react. Overlay closed ⇒ input flows to the game. MVP assumption: overlay focus = input ownership. Must be tested against Steam Input / DS4Windows interference.

## 4. Overlay Technical Constraints

MVP supports **borderless fullscreen, windowed fullscreen, windowed** games. Exclusive fullscreen is not guaranteed. **No injection ever in early versions** (no DX/Vulkan hooks — anti-cheat risk). If the overlay can't appear, tell the user to switch the game to borderless.

## 5. Main User Flows

**Screenshot:** Share tap → capture → sound → save PNG → DB insert → thumbnail → appears in gallery grouped under detected game. Defaults: PNG, original resolution, GameHQ folder, notification+sound on.

**Replay save:** buffer runs in background → Share hold 2 s → export last 5 min → sound → thumbnail → gallery. Defaults: **1920×1080 @ 30 fps, H.264, 12–16 Mbps, 5 min, system audio only.**

**Overlay gallery:** PS → overlay + open sound → latest capture of current game selected → browse (D-pad/analog + tick sounds) → X opens, Triangle favorites → PS/Circle closes + close sound.

## 6. Controller Mapping (default)

```txt
Share tap        screenshot           X          open/play
Share hold 2 s   save replay          Circle     back/close
PS               toggle overlay       Triangle   favorite
D-pad ←/→        (no-op in overlay*)  Square     action menu
D-pad ↑/↓        rows/sections        L1/R1      prev/next capture (overlay)
Left stick       navigate             Options    overlay menu/settings
```

\* In the **overlay**, d-pad ←/→ (and left-stick ←/→) flips captures while browsing. After Cross enters focused video playback, d-pad/stick ←/→ seeks the clip and L1/R1 remains the previous/next capture control. D-pad ↑/↓ still moves the sidebar (categories/games).

PS button may be intercepted (Steam, DS4Windows, Game Bar) → custom bindings required; never rely on PS alone.

## 7. Tap vs Hold

Share down → start timer. Released before threshold → screenshot. Still held at threshold (default **2.0 s**; options 1.0/1.5/2.0/3.0/custom) → save replay, mark consumed; release then does nothing. Never both.

## 8. Custom Bindings

Bindable inputs: keyboard shortcuts, controller buttons/holds/combos (mouse later). Actions: screenshot, save replay, toggle overlay, favorite, delete, open, open-in-folder, copy file/image, fullscreen viewer, play/pause, seek ±. The replay buffer has no hotkey: it is always-on by default, with the master switch in Settings → Replay.
Keyboard fallback defaults: `Ctrl+Shift+S` screenshot · `Ctrl+Shift+E` save replay · `Ctrl+Shift+G` overlay.

## 9. UI Sounds

Console-style, subtle. Events: screenshot, replay saved, overlay open/close, navigation tick, favorite, confirm, error/blocked. Settings: master on/off, per-event on/off, volume 0–100 %, custom sound packs. Option (default **off**): include GameHQ UI sounds in recordings — loopback capture will otherwise record them (acceptable for MVP).

## 10. "Only in Games" Mode

Capture modes: **only when game active (default)** · only whitelisted games · always. Detection: foreground window, process name, window title, fullscreen/borderless heuristic, user whitelist, manual mapping (`Diablo IV.exe → Diablo IV`). New fullscreen app detected → prompt "Add as a game?". MVP: manual whitelist is fine.

## 11. Storage Layout

Never mix into Game Bar's `Videos\Captures` by default. Default root:
`%USERPROFILE%\Videos\GameHQ\<Game>\{Screenshots,Clips}\` (+ `Unknown Game`).
Can watch/import: Game Bar, Steam, NVIDIA, OBS, custom folders — GameHQ becomes
the central gallery while its own files stay clean.

## 12. Portable Mode

```txt
GameHQ/  GameHQ.exe · Qt/ · plugins/
         gamehq-data/ (config.json, gamehq.db, thumbnails/, logs/, replay-cache/, sound-packs/)
         Captures/<Game>/{Screenshots,Clips}/
```

First-launch choice: portable (next to exe) / Windows Videos / custom. Personal default: config+db next to app, captures in `Videos\GameHQ`.

## 13. Database (SQLite)

`gamehq.db` — tables: `captures`, `games`, `settings`, `bindings`, `folders`, `sound_settings`. Full schema: [database.md](database.md). **Hard rule: favorites are never auto-deleted.**

## 14. Replay Buffer Design

No raw frames in RAM. Continuously encode 10-second temporary segments; 5-min buffer = 30 segments rotating in `gamehq-data/replay-cache/`. On save: collect last 30 → merge/remux → final MP4 → DB → thumbnail → sound → notification. Benefits: low RAM, crash-resistant, easy cleanup/length management. Details: [replay-buffer.md](replay-buffer.md).

## 15. Gallery / Overlay UI

Layout - overlay sidebar starts with Game (current focused game's screenshots and clips), then Game Favourites (favourites from that same focused game), Recent / Favorites / Screenshots / Clips / Games. Center: large preview. Bottom: thumbnail strip. Top: game, type, date, buffer status.
Screenshot actions: prev/next, zoom 100 %, fit, fullscreen, favorite, delete, copy image/file, open in folder. Video: play/pause, seek, restart, fullscreen, favorite, delete, open in folder. Overlay = quick actions; desktop window = settings/imports/mappings/storage/sound packs/bindings.

### Navigation memory (last-view rules)

GameHQ reopens where the user left it. `NavigationState` (`src/ui/NavigationState.{h,cpp}`) owns the rules and the config keys; every surface goes through it.

| Rule | Entry point | Behaviour |
| --- | --- | --- |
| R1 | Launch | Restores the last page, the Settings category and the gallery filter. Help is never restored and collapses to the gallery. |
| R2 | Window summon (PS hold, `--activate`, desktop toggle) | Pure raise: page, category and filter are untouched. |
| R3 | Tray open (menu item, single and double click) | Same as R2. |
| R4 | `--open-gallery` | Explicit page intent: leaves Settings/Help and shows the gallery, keeping the saved filter. |
| R5 | "Clip saved" toast click | One-shot jump to the new clip and its game, deliberately **not** persisted, so the next ordinary open restores the saved view. The toast click itself ships with the notification work; the non-persisting path (`setGameCategoryTransient`) is already in place. |
| R6 | Settings category | Stored as a stable key in `ui.settings_category` (`general`, `capture`, `replay`, `input`, `library`, `notifications_sound`, `advanced`, `about`). A pre-0.7.11 numeric value is migrated once at startup; an unknown key opens General. |
| R7 | Gallery filter | Stored as `ui.gallery_filter_category` + `ui.gallery_filter_game` (`-1` = every game). Applied after the first scan so the game id can be validated; a game that no longer exists takes the filter back to the whole library. |
| R8 | Overlay filter | The game binding always follows the foreground game and is never restored. Only the category is remembered, per game, in `ui.overlay_filter.<gameId>`; an absent or unknown entry means `all`. |
| R9 | Precedence | Explicit navigation intent (R5, R4) > restored state > defaults. The post-update greeting still opens over whatever was restored. |
| R10 | Write timing | Persisted as the user navigates (batched by half a second) plus an idempotent flush on hide and on quit. "Restore all defaults" clears `ui.*`, so the next open starts on the defaults again; a single page's "Restore defaults" never touches the remembered view. |

### Window placement (restore rules)

The desktop window reopens where it was closed, on whatever monitor that was. `WindowPlacement` (`src/core/WindowPlacement.{h,cpp}`) decides the rectangle from the saved numbers and the screens that are actually connected; `AppController::restoredWindowGeometry()` supplies both and `Main.qml` only applies the answer.

| Rule | Behaviour |
| --- | --- |
| W1 | Position and size live in `ui.window_x` / `ui.window_y` / `ui.window_width` / `ui.window_height`, written debounced (half a second) while the user moves or resizes. |
| W2 | A monitor left of or above the primary has negative coordinates; those are ordinary coordinates and are restored unchanged. |
| W3 | The saved rectangle is kept whenever at least 120 x 32 px of it lands on one screen's work area — enough title bar to see and drag. A window that hangs over an edge is the user's own choice and is never nudged. |
| W4 | If no connected screen can show that much of it — the monitor was unplugged, or the position was never saved — the window is centred on the primary screen's work area, keeping its size unless that screen is too small for it. |
| W5 | Screens are matched one by one against their own work areas, never against the virtual bounding box, which on an L-shaped desktop covers gaps where no display exists. |
| W6 | "Restore all defaults" clears the geometry with the rest of `ui.*`, so the next open is centred at the default size. |

## 16. Tray & Background

Settings: start with Windows, start minimized, minimize/close to tray, tray icon, notifications, pause capture when not gaming. Tray menu: Open Gallery, Open Settings, Take Screenshot, Save Replay, Buffer ON/OFF, Capture Mode, Settings, Exit. Tooltip shows buffer state + preset + current game. Status states: buffer on/off/paused-no-game/error-audio-missing; overlay active/hidden.

## 17. Notifications

"Screenshot saved [Open] [Show in folder]" · "Replay saved — last 5 minutes [Open] [Show in folder]" · "Capture ignored — no active game" · "Replay buffer is not running". All configurable (popups during games can annoy).

## 18. Audio

MVP: **system audio only** (WASAPI loopback). Later: mic, separate tracks, per-app capture, Discord/Spotify exclusion — explicitly out of the first replay milestone.

## 19. Storage Management

Max storage: 10/25/50/100 GB/unlimited. Auto cleanup: disabled / oldest non-favorites / clips only / both. Rules: never auto-delete favorites; never delete outside managed folders without confirmation; don't touch imported folders unless enabled; warn on low disk.

## 20. External Import

Watch/import Game Bar, Steam, NVIDIA, OBS, custom. Mark `source` accordingly. Infer game from folder/file name, manual mapping, user assignment.

## 21–22. Architecture & Repo Layout

See [architecture.md](architecture.md).

## 23. Roadmap

See [roadmap.md](roadmap.md) (0.1 gallery → 0.2 overlay → 0.3 input → 0.4 screenshots → 0.5 replay → 0.6 overlay gallery → 0.7 storage → 1.0 polish).

## 24. Technical Risks

WGC reliability · overlay vs exclusive fullscreen · focus restoration · game still reacting to controller input · Steam Input / DS4Windows conflicts · PS interception · HDR · GPU encoder compat · A/V sync · WASAPI device changes · anti-cheat · MP4 export reliability.
Mitigations: borderless-first, no injection, PS optional, keyboard fallback, 1080p30 first, system-audio-only first, manual whitelist first, log everything.

## 25. Implementation Rules

One feature per milestone · overlay shell before replay buffer · input before real capture · screenshots before video · no buffer until screenshots reliable · no advanced audio until basic replay works · no auto-delete until favorites reliable · never modify Game Bar folder by default · portable paths tested from day one · no injection.
Quality gates: builds, launches clean, portable works, settings persist, DB migrations work, no crash on missing controller/audio/folder, overlay open/close repeatedly, focus restoration tested, logs written.

## 26. HQCore (later)

Possible shared lib with AudioHQ (logging, config, portable paths, tray, startup, SQLite wrapper, process detection, crash logging). Don't over-engineer now; keep modules clean enough to extract later.

## 27. Final Direction

> **GameHQ is primarily an in-game overlay capture system, not just a desktop gallery.** Fast, quiet, controller-friendly, always available during gameplay, organized around games rather than random files.
