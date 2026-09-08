# Architecture

Module map and repo layout. Each module should keep one primary responsibility. Spec details live in [product-spec.md](product-spec.md); refactor risks and next cleanup steps live in [technical-audit.md](technical-audit.md).

## Module Diagram

```txt
GameHQ.exe
|-- app/          App lifecycle and service wiring
|-- ui/           QML windows/components + AppController + GalleryModel + UI facade helpers
|-- overlay/      OverlayManager: lazy overlay load, topmost show/hide, focus return
|-- capture/      ScreenshotService, FramePumpService, SegmentRecorder, ReplayExporter, AudioCapture, CaptureUtil
|-- core/         Shared cross-module helpers such as GameIdentity
|-- sound/        SoundEngine for UI sounds
|-- input/        InputEngine, DualSenseDevice (behind the RawInputApi seam), InputRateMonitor, XInputDevice, WinMMDevice, HotkeyManager
|-- games/        GameDetector: foreground process, title resolution (cached per process), fullscreen heuristic
|-- storage/      CaptureDatabase, CaptureQueries, CaptureScanner, ThumbnailService, GameIconCache, GameMetadataBackfill, GameRowRepair
|-- config/       ConfigManager, ConfigKeys, SettingsCategories, CaptureLocations, LegacyMigration, and Paths
|-- tray/         TrayIcon and menu
`-- diagnostics/  Logger
```

## Dependency Rules

- QML talks to app state and commands through `AppController` and exposed models.
- `AppController` stays as the QML facade; helper classes such as `CaptureLibraryService`, `CurrentGameService`, `SettingsRouter`, and `ShellActions` handle capture-library, current-game, settings-key, and platform actions behind that API.
- `NavigationState` owns the last-view memory (which page, Settings category, gallery filter and per-game overlay category the app reopens on). It is pure logic over `ConfigManager` — no database, no QML — so the rules in docs/product-spec.md "Navigation memory" are unit-tested in `tests/tst_navigationstate.cpp`. `AppController` restores the gallery filter after the first scan, once the game ids it validates against exist.
- `SettingsRouter` owns the config keys that are not plain values: the startup toggle (an OS side effect that can refuse the change) and the capture roots (persisted by `CaptureLocations` itself). It performs the side effect and returns an outcome; emitting signals and rescanning stay with `AppController`.
- QML never calls Win32/WinRT directly.
- Visual values come from `Theme.qml` per [design-system.md](design-system.md).
- `overlay` owns window/focus mechanics.
- `input` decides controller routing between global actions, overlay, and desktop gallery.
- Settings use an observable configuration facade behind `AppController`; built-in defaults remain separate from persisted user overrides so individual groups can reset safely.
- C++ spells every `config.json` key through the `ConfigKeys` constants in `config/ConfigKeys.h`, never a raw string, so a typo fails to compile instead of silently falling back to the default. The key list there and `ConfigManager::defaults()` are expected to match. QML keeps using string literals (`app.config("capture.mode")`) — it has no access to the constants. `SettingsCategories` holds the page → config-group taxonomy that each page's "Restore defaults" uses.
- `CaptureLocations` resolves portable-aware screenshot/clip roots, validates changes, retains prior managed roots for safe rescans, and passes plain path values across worker boundaries.
- Win32/WinRT code stays in `.cpp` files where possible; headers should remain platform-clean unless the type boundary requires otherwise.

## Repository Layout

```txt
GameHQ/
|-- CMakeLists.txt        VERSION  CHANGELOG.md  README.md
|-- docs/                 technical documentation
|-- src/
|   |-- main.cpp
|   |-- app/ ui/ overlay/ capture/ core/ sound/
|   |-- input/ games/ storage/ config/ tray/ diagnostics/
|-- tests/                opt-in pure-logic tests (-DGAMEHQ_BUILD_TESTS=ON)
|-- assets/
|   |-- icons/
|   `-- sounds/
|-- site/
|-- packaging/            guarded local/release package assembly
|-- out/                  git-ignored CMake/Ninja compiler workspace
|-- build/                git-ignored clean local portable package + data
`-- dist/GameHQ/           git-ignored clean release package
```

## Key Data Flows

**Screenshot:** InputEngine or hotkey -> GameDetector gate -> SDR uses ScreenshotService GDI, HDR uses FramePumpService FP16 WGC plus GPU tone mapping -> async PNG/JPEG write -> AppController facade -> CaptureLibraryService -> CaptureDatabase insert -> ThumbnailService -> gallery refresh -> sound/notification.

**Replay save:** InputEngine hold or hotkey -> current CaptureLocations clip root copied to FramePumpService worker -> SegmentRecorder ring snapshot -> ReplayExporter remux -> AppController facade -> CaptureLibraryService -> CaptureDatabase insert -> gallery refresh -> sound/notification.

**Overlay toggle:** InputEngine or hotkey -> OverlayManager show/hide -> foreground HWND remembered/restored -> QML overlay uses its own GalleryModel -> input routing follows overlay visibility.

**Current-game section:** FramePumpService foreground poll, OverlayManager about-to-show, or a fresh capture commit -> AppController facade -> CurrentGameService -> desktop/overlay sidebars show a `Game` section when the focused known game has captures. CurrentGameService keeps the last detected game through a short run of foreground misses and can fall back to captured games whose stored executable is still running, so app/overlay focus or title drift does not make the row flicker.

## Threading Model

- UI thread: Qt/QML, AppController, model refresh, and most short DB calls.
- Screenshot worker: PNG/JPEG encoding after an SDR GDI grab or HDR tone-mapped WGC readback.
- Replay worker: WGC frame pump, HDR tone mapping/readback, D3D access, Media Foundation segment writing, optional audio capture, and replay export.
- Input/hotkeys: native events and device polling feed Qt signals.

Current DB access stays behind the storage API. `CaptureDatabase` owns the public database facade, mutations, and migrations; read-only capture/game listing SQL lives in `CaptureQueries`. Keep queries short on the UI path; if scanning or repair work grows, move that work behind a worker boundary.

## QML Structure

The desktop gallery entry point remains `src/ui/qml/Main.qml`, but presentational chrome is split into focused components:

- `components/DesktopSidebar.qml` owns the desktop navigation/sidebar presentation.
- `components/DesktopGalleryHeader.qml` owns the title and bulk-selection action row.
- `components/DesktopGalleryGrid.qml` owns the desktop capture grid, empty state, and standalone scrollbar surface.
- `components/DesktopGalleryFooter.qml` owns footer hints and zoom controls.
- `components/DesktopEmptyState.qml` owns the no-captures prompt.
- `components/OverlaySidebar.qml`, `OverlayCaptureStrip.qml`, `OverlayPreview.qml`, and `OverlayFooter.qml` own the overlay's presentational panels.
- `components/MediaStage.qml` is the double-buffered still/clip stage shared by `OverlayPreview.qml` and the desktop `Lightbox.qml`. It owns the async decode-then-promote handoff (the previous capture stays painted until the next one reaches `Image.Ready`) and the media-player/end-of-media wiring. The rules that differ between the two surfaces — what the loader decodes, what paints behind a clip, whether the committed still clears on an empty target — are properties set by each caller.
- `components/OverlayActionMenu.qml` is shared by the overlay and desktop pad action menu.
- `SettingsView.qml` owns the settings category rail while focused page files under `ui/qml/settings/` own General, Capture, Replay, Input, Library, Feedback, and Advanced content.
- Shared settings page, section, row, toggle, category, and combo controls live under `ui/qml/components/` and update from `AppController::configChanged`.
- `helpers/SidebarCategories.js` owns shared desktop/overlay category definitions (`categories()`) and the key→filter mapping behind them (`resolveFilter()`). Every sidebar — desktop mouse click, desktop pad nav, overlay pad nav — resolves a row through it and then applies the result on its own surface: the desktop via `AppController::setGameCategory`, the overlay via its gallery's `setFilter`. Both land on `GalleryModel::setFilter` with the same pair. Do not re-implement the `game` / `game_favorites` special cases in a sidebar.
- `themes/Skin.qml` declares the skinnable surface — colors plus font family, radii, border weight, motion and backdrop — with the Blue theme's values as defaults; the thirteen skins override from it, so an omitted token falls back to those valid base values. `Theme.qml` binds every token through the active skin (`Theme.skins` map, `Theme.skinOrder` for presentation), defaults fresh installs and unknown keys to Obsidian, and re-assigns `activeSkin` on `configChanged` (`theme.active_skin`), which repaints live. Spacing, font sizes and control sizes are deliberately NOT skinnable: a skin restyles, it must not re-lay-out. `components/ThemeBackdrop.qml` paints the window per `backdropStyle` (flat/gradient/wash/scanlines). See docs/design-system.md §0.
- `components/BulkSelection.qml` owns the bulk-selection state machine: the picked-path set, the range anchor, and the base snapshot a shift-extend replays against so dragging back over a range undoes it. It is deliberately pure — model queries and state, no sounds, focus moves, or dialogs. `Main.qml` instantiates it as `bulkSelection` and keeps the `window.bulkX()` API every caller already binds to, delegating and playing sounds around the call; `toggle()` and `selectAll()` return a bool so the caller picks the sound. Callers (grid, header, pad handlers) never touch it directly.

`Main.qml` still owns top-level desktop state, dialogs, and navigation helper functions; `DesktopGalleryGrid.qml` keeps the existing grid API exposed back to that owner.

Since 0.5.99 the grid takes no `host`. It is told what to show (`columns`, `zoomLevel`, `bulkMode`, `bulkIsChecked`) and emits what it wants done (`keyboardActivity`, `bulkToggleRequested`, `bulkDeleteRequested`, `bulkSelectAllRequested`), the same shape `DesktopGalleryHeader` already used. Two details are load-bearing: QML signal handlers run **synchronously**, which is what preserves the key-path order the direct `host.usingGamepad = false` write relied on (it must land before `input.handleKeyPressed()`); and `bulkIsChecked` is passed as a **predicate**, not converted to a signal, so the delegate's `checked` stays a real binding that re-evaluates when the selection changes.

## Refactor Direction

The current safe refactor wave is complete. The near-term target is not a rewrite; future cleanup should stay tied to concrete feature work or isolated pain points:

1. Keep using `core/GameIdentity` for all folder-safe names and identity keys.
2. Keep desktop/overlay sidebar category definitions in `ui/qml/helpers/SidebarCategories.js`.
3. Keep splitting QML along clear view boundaries before extracting behavior-heavy navigation.
4. Keep file actions and capture commit logic behind `CaptureLibraryService`.
5. Keep current-game foreground matching behind `CurrentGameService`.
6. Keep game icon caching in `GameIconCache`, log metadata repair in `GameMetadataBackfill`, duplicate game-row repair in `GameRowRepair`, and read-only capture/game listing SQL in `CaptureQueries`.
