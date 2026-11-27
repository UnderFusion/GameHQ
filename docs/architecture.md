# Architecture

Current architecture reviewed against `e6c50b0` on 2026-11-27. Each module
keeps one primary responsibility. Product behavior and earlier findings are
recorded separately. [Product specification](product-spec.md), [July audit snapshot](technical-audit.md)

## Module Diagram

```txt
GameHQ.exe
|-- app/           App lifecycle and service wiring, StartupManager (Windows Run entry), ReleaseNotes
|-- ui/            QML windows/components + AppController + GalleryModel + UI facade helpers
|                  (CaptureLibraryService, CurrentGameService, NavigationState, SettingsRouter, ShellActions)
|-- overlay/       OverlayManager, OverlayPresenter, ForegroundAcquirer: lazy overlay load, topmost show/hide, focus return
|-- notify/        NotificationCenter + ToastModel: non-activating, capture-excluded toast window
|-- capture/       ScreenshotService, FramePumpService, SegmentRecorder, ReplayExporter, ReplayExportTask, AudioCapture, CaptureUtil
|-- core/          Shared helpers: GameIdentity, GamePins, ProcessIdentity, WindowPlacement, ApplicationMutex, UpdateMaintenance
|-- sound/         SoundEngine for UI sounds
|-- input/         InputEngine, DualSenseDevice (behind the RawInputApi seam) with the opt-in SonyHidReader,
|                  XInputDevice, WinMMDevice, MouseHookDevice, HotkeyManager, ControllerArbitration,
|                  PhysicalControllerRegistry, binding resolution/runtime, mapping presets, HidCloakMonitor
|-- gameinput/     GameInputRouter over Microsoft GameInput (IGameInputApi seam, GameInputQtDispatcher)
|-- games/         GameDetector: foreground process, title resolution (cached per process), fullscreen heuristic; SteamAppLookup
|-- storage/       CaptureDatabase, CaptureQueries, CaptureScanner, CaptureFolderWatcher, ThumbnailScheduler, ThumbnailService, GameIconCache,
|                  GameMetadataBackfill, GameRowRepair
|-- config/        ConfigManager, ConfigKeys, SettingsCategories, SettingsApplyPolicy, CaptureLocations, LegacyMigration,
|                  PortableProfileImporter, and Paths
|-- localization/  LanguageManager, LocaleRegistry, StartupLocalization: UI language selection and translators
|-- share/         ShareService + ShareProviderRegistry; built-in providers and the external provider host
|-- telegram/      TDLib runtime loader, transport and TelegramAccount for Telegram Integrated (dormant unless built in)
|-- integration/   IntegrationService over LocalIntegrationServer (`GameHQ.Local.v1` pipe, e.g. Playnite plugin)
|-- updates/       UpdateService: GitHub discovery, signed manifest and artifact verification, handoff to the updater
|-- security/      ReleaseManifest + ReleaseTrust: signed release manifest verification (shared with GameHQUpdater)
|-- tray/          TrayIcon and menu
`-- diagnostics/   Logger

Separate executables built from src/:
|-- launcher/      GameHQLauncher: package entry point that forwards its command line to app\GameHQ.exe
`-- updater/       GameHQUpdater: staged, transactional package swap with data snapshot, health check and recovery
```

## Dependency Rules

- QML talks to app state and commands through `AppController` and exposed models.
- `AppController` stays as the QML facade; helper classes such as `CaptureLibraryService`, `CurrentGameService`, `SettingsRouter`, and `ShellActions` handle capture-library, current-game, settings-key, and platform actions behind that API.
- `NavigationState` owns the last-view memory (which page, Settings category, gallery filter and per-game overlay category the app reopens on). It is pure logic over `ConfigManager` — no database, no QML — so the rules in docs/product-spec.md "Navigation memory" are unit-tested in `tests/tst_navigationstate.cpp`. `AppController` restores the gallery filter after the first scan, once the game ids it validates against exist.
- `WindowPlacement` (`src/core/`) decides where the desktop window reopens: plain rectangle logic over the saved geometry and the connected screens' work areas, so a monitor at negative coordinates restores unchanged and only a rectangle no screen can show is recentred (docs/product-spec.md "Window placement", `tests/tst_windowplacement.cpp`). `AppController::restoredWindowGeometry()` gathers the screens and the config; `Main.qml` applies the result and does no screen maths of its own.
- `SettingsRouter` owns the config keys that are not plain values: the startup toggle (an OS side effect that can refuse the change) and the capture roots (persisted by `CaptureLocations` itself). It performs the side effect and returns an outcome; emitting signals and rescanning stay with `AppController`. `SettingsApplyPolicy` centrally classifies every built-in key as live or replay-buffer-restarting; group resets classify only keys that actually changed and coalesce a mixed group to one restart.
- QML never calls Win32/WinRT directly.
- Visual values come from `Theme.qml` per [design-system.md](design-system.md).
- `overlay` owns window/focus mechanics.
- `input` decides controller routing between global actions, overlay, and desktop gallery.
- Settings use an observable configuration facade behind `AppController`; built-in defaults remain separate from persisted user overrides so individual groups can reset safely.
- C++ spells every `config.json` key through the `ConfigKeys` constants in `config/ConfigKeys.h`, never a raw string, so a typo fails to compile instead of silently falling back to the default. The key list there and `ConfigManager::defaults()` are expected to match. QML keeps using string literals (`app.config("capture.mode")`) — it has no access to the constants. `SettingsCategories` holds the page → config-group taxonomy that each page's "Restore defaults" uses.
- `CaptureLocations` resolves portable-aware screenshot/clip roots, validates changes, retains prior managed roots for safe rescans, and passes plain path values across worker boundaries.
- Win32/WinRT code stays in `.cpp` files where possible; headers should remain platform-clean unless the type boundary requires otherwise.

## Ownership and dependency direction

The composition root creates services and connects their lifetimes. The
capture, input, share and update wiring live in the corresponding
`App_*Wiring.cpp` files. The
desktop and overlay depend on the controller and model contracts. Domain
helpers stay below storage, capture, platform adapters and UI. Game identity
is a Qt Core-only static target shared by GameHQ and its tests.
[App composition](../src/app/App.cpp), [App wiring contract](../src/app/App.h), [Build graph](../src/CMakeLists.txt)

| Owner | Current responsibility |
| --- | --- |
| GUI dispatcher | QML, controller and gallery models, overlay native ownership, settings and the live SQLite facade. A model update follows the corresponding storage commit. |
| Capture worker | WGC, D3D, HDR processing and recording. COM and the media pipeline are started and torn down on the capture worker. A separate export task is joined before the worker's shutdown completes. |
| Screenshot encoding pool | Bounded PNG/JPEG work after readback, with completion reported to the GUI. |
| Library scan and thumbnails | The scanner gathers folder changes off the GUI thread and publishes a delta. GUI-owned storage applies reconciliation. The thumbnail scheduler bounds live decode work and checks for stale results before publishing. |
| Input producers | Backend readers publish events. The GameInput dispatcher bounds queued delivery and routes the drain to the GUI. Binding and overlay routing stay in the input engine. |
| Updater helper | Separate native process owns staged install, recovery and rollback. The running application provides its own health result. |

[Capture worker](../src/capture/FramePumpService.cpp), [Capture pipeline](../src/capture/FramePumpPipeline.cpp), [Scanner](../src/storage/CaptureScanner.cpp), [Thumbnail scheduler](../src/storage/ThumbnailScheduler.cpp), [GameInput delivery](../src/gameinput/GameInputQtDispatcher.cpp), [Shutdown contracts](threading-and-shutdown.md)

## Decisions that affect extensions

- **Single-instance and updater ownership.** `QLockFile` decides instance
  admission. The application mutex reports process lifetime to Setup and
  Uninstall. The updater ties a handoff to its authorizing process, owns
  package replacement, and preserves recovery state until confirmed health
  and cleanup. [Application entry](../src/main.cpp), [Mutex contract](../src/core/ApplicationMutex.h), [Update lifecycle](updater.md)
- **Controller identity.** Physical controller identity, backend route and
  binding identity have distinct roles. Reuse the registry and binding
  normalization when adding a producer. The input engine consumes the routed
  events. [Physical registry](../src/input/PhysicalControllerRegistry.h), [Binding runtime](../src/input/BindingRuntime.h), [Controller guide](controller-input.md)
- **Asynchronous library work.** Carry paths, IDs and results across threads.
  The GUI-side library service owns commits and model publication. Thumbnail
  generation follows capture commit and must tolerate a deleted or changed
  row before its result returns. [Library service](../src/ui/CaptureLibraryService.cpp), [Gallery model](../src/ui/GalleryModel.cpp)
- **Overlay panels.** The surface owns activation, focus and input mode.
  A component receives explicit values and emits requests. Keep Win32
  window mechanics in the overlay C++ layer and avoid recurring work while
  a panel is inactive. [Overlay window](../src/ui/qml/OverlayWindow.qml), [Layout panel](../src/ui/qml/components/OverlayLayoutPanel.qml), [Presenter](../src/overlay/OverlayPresenter.cpp)

## Repository Layout

```txt
GameHQ/
|-- CMakeLists.txt        VERSION  CHANGELOG.md  README.md
|-- docs/                 technical documentation
|-- src/
|   |-- main.cpp
|   |-- app/ ui/ overlay/ notify/ capture/ core/ sound/
|   |-- input/ gameinput/ games/ storage/ config/ localization/
|   |-- share/ telegram/ integration/ updates/ security/ tray/ diagnostics/
|   `-- launcher/ updater/   separate GameHQLauncher / GameHQUpdater executables
|-- tests/                opt-in unit, integration and synthetic tests (-DGAMEHQ_BUILD_TESTS=ON)
|-- assets/
|   |-- icons/  installer/  release-notes/
|   `-- sounds/
|-- i18n/                 translation catalogs and localization state
|-- integrations/         Playnite companion plugin and share-provider samples
|-- third_party/          vendored GameInput and TDLib inputs
|-- tools/                i18n, test evidence, release-manifest, input-receiver and manual validation
|-- site/
|-- packaging/            guarded local/release package assembly
|-- out/                  git-ignored CMake/Ninja compiler workspace
|-- build/                git-ignored clean local portable package + data
`-- dist/GameHQ/           git-ignored clean release package
```

## Key Data Flows

**Screenshot.** InputEngine or hotkey → capture policy → GDI or HDR frame
readback → async image write → library commit → gallery publication and
capture feedback. Thumbnails are scheduled separately.

**Replay save.** InputEngine hold or hotkey → frame worker → ring lease →
export/remux → library commit and capture feedback. `clipSaving` is a
progress signal. Outcome comes through the saved or failed path.
[Capture wiring](../src/app/App_CaptureWiring.cpp), [Replay and capture engine](capture-engine.md)

**Overlay toggle:** InputEngine or hotkey -> OverlayManager show/hide -> foreground HWND remembered/restored -> QML overlay uses its own GalleryModel -> input routing follows overlay visibility.

**Current-game section:** FramePumpService foreground poll, OverlayManager about-to-show, or a fresh capture commit -> AppController facade -> CurrentGameService -> desktop/overlay sidebars show a `Game` section when the focused known game has captures. CurrentGameService keeps the last detected game through a short run of foreground misses and can fall back to captured games whose stored executable is still running, so app/overlay focus or title drift does not make the row flicker.

## Threading Model

Details (which object lives on which thread, shutdown order, file deletion rules, crash recovery, error/logging conventions) live in [threading-and-lifecycle.md](threading-and-lifecycle.md). In short:

- UI thread: Qt/QML, AppController and its helpers, models, overlay/toast windows, tray, sound, hotkeys, Raw Input/XInput polling, folder watching, network checks, and most short DB calls.
- Frame pump (`FramePumpService`, a `QThread` with a moved worker): WGC frame pump, HDR tone mapping/readback, D3D access, and Media Foundation segment writing (`SegmentRecorder`). The recorder prepares the next segment on its own `std::thread`; `AudioCapture` drains game-audio loopback on its own `std::thread`; each replay export runs on a `ReplayExportTask` (`QThread`) owned and joined by the worker.
- Screenshot encoding: `ScreenshotService` thread pool (2 threads) writes PNG/JPEG after the SDR GDI grab or HDR readback.
- Library scan: `CaptureScanner` runs directory work in its one-worker pool. The live thumbnail scheduler has its own bounded workers.
- Input devices: `WinMMDevice`, `MouseHookDevice` (WH_MOUSE_LL), the opt-in `SonyHidReader`, and `GameInputQtDispatcher` each run one `std::thread` and hand events to the UI thread.
- Telegram Integrated (dormant unless built in): `TdJsonTransport` runs the TDLib receive loop on its own `QThread`.

Current DB access stays behind the storage API. `CaptureDatabase` owns the public database facade, mutations, and migrations; read-only capture/game listing SQL lives in `CaptureQueries`. Keep queries short on the UI path; if scanning or repair work grows, move that work behind a worker boundary.

## Stored state and command contracts

Configuration uses sparse dotted-key overrides and atomic saves. The
settings facade emits the individual value notifications and writes one
geometry batch. Replay restart notification follows the save attempt for
changed batch values. Configuration defaults and migration behavior are part
of the schema contract. [Settings facade](../src/ui/AppController_Settings.cpp), [Configuration storage](../src/config/ConfigManager.cpp)

Storage schema changes go through migrations. Keep supported portable path
forms and prior records readable. Protocol changes retain the transport and
version contracts for supported peers. Updater state names, health and
release signature semantics are maintained with their acceptance tests.
[Storage](storage.md), [Stored paths](../src/config/Paths.cpp), [Playnite protocol](integration-protocol.md), [Share protocol](share-provider-api-v1.md), [Updater contract](updater.md)

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
- `SettingsView.qml` owns the settings category rail while focused page files under `ui/qml/settings/` own General, Capture, Replay, Input, Library, Sharing, Feedback, Advanced, and About content.
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

## Extension example

The layout options panel is a reproducible starting point for a new overlay
panel. Define the presentation in a component, pass in state and stable
option keys, and route requests through the overlay window. Register the
component in the QML module. Add tests for values, close behavior and focus
transitions at the owning layer. The existing fixture instantiates the
production panel offscreen and verifies row keys, values and footer hints.
[Panel source](../src/ui/qml/components/OverlayLayoutPanel.qml), [Module registration](../src/CMakeLists.txt), [Offscreen fixture](../tests/qml/tst_overlaylayout.qml)

In a test-enabled build configured as `out-evidence`, run:

```powershell
ctest --test-dir out-evidence -R '^tst_overlaylayout$' --output-on-failure
```

Use the clean-checkout commands for a broader verification receipt. Report
CPU/GPU or latency results with the workload and toolchain. Keep native
overlay, live capture and real-controller acceptance in their hardware
matrix. [Regression evidence](testing/regression-evidence.md), [Profiling recipe](audit/t45-profiling/README.md), [Hardware acceptance](testing/acceptance-matrix.md)
