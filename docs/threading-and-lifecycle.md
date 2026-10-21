# Threading and lifecycle

Which thread owns what, how GameHQ starts and stops, which files it may delete, what survives a crash, and how errors are reported. Everything here describes the current code under `src/`.

## 1. Threads

The GUI thread owns `App`, `AppController`, the QML engine, every model, `CaptureDatabase` and the config. Worker threads never touch QML or the database. They send results back with queued signals or `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`.

| Thread | Owner / created by | Work | Back to the GUI thread |
|---|---|---|---|
| GUI (main) | `QApplication` in `src/main.cpp` | QML, tray, models, DB, config, notifications, XInput poll timer, Raw Input `WM_INPUT` (message-only window in `src/input/DualSenseDevice.cpp`), `IntegrationService` / `ExternalProviderHost` local sockets, `QNetworkAccessManager` users (updates, Discord webhook) | n/a |
| Frame pump worker (`QThread m_thread`) | `FramePumpService` (`src/capture/FramePumpService.cpp`) | `FramePumpWorker` is moved to it; `RoInitialize(MTA)`; all WGC/D3D11 work, segment recording, stale replay-cache sweep | Worker signals connected to the service (auto/queued); state signals carry a generation so stale ones are dropped |
| Segment prep thread (`std::thread`) | `SegmentRecorder::startPrep` (`src/capture/SegmentRecorder.cpp`) | Builds the next segment's MF writer ahead of time | Atomic state flag; joined by `joinPrep()` |
| Replay export (`ReplayExportTask : QThread`) | `FramePumpWorker::runExport` | Remux-concat of leased segments, thumbnail, `.part` publish | `QThread::finished` queued to the worker, which emits `clipSaved` / `clipFailed` |
| Game-audio drain (`std::thread`) | `AudioCapture::startDrainThread` (`src/capture/AudioCapture.cpp`) | Game (per-process) loopback only: drains WASAPI packets into a FIFO that `poll()` reads | FIFO; joined in `stop()` |
| Screenshot encode pool (`QThreadPool`, max 2) | `ScreenshotService` (`src/capture/ScreenshotService.cpp`) | PNG/JPG encode into a reserved `.part`, then publish | `captured` / `failed` emitted from the pool thread; the receivers live on the GUI thread, so delivery is queued |
| Capture scan pool (`QThreadPool`, one worker) | `CaptureScanner::scanAllAsync` (`src/storage/CaptureScanner.cpp`) | Directory walk and image thumbnail decoding against an index snapshot. It makes no DB calls | `invokeMethod(this, finishAsyncScan, Qt::QueuedConnection)`; DB writes happen in `apply()` on the GUI thread |
| Video thumbnail thread (`QThread::create`) | `ThumbnailService::ensureThumbnail` (`src/storage/ThumbnailService.cpp`) | MTA COM thread for the MF first-frame grab | Synchronous: the caller does `start()`, `wait()`, `delete` |
| Mouse hook thread (`std::thread`) | `MouseHookDevice::start` (`src/input/MouseHookDevice.cpp`) | `SetWindowsHookExW(WH_MOUSE_LL)` plus its own message loop; observes only and always calls `CallNextHookEx` | Signals queued to the GUI thread, stamped with a hook generation |
| Sony HID readers (`std::thread` per pad) | `SonyHidReader::open` (`src/input/SonyHidReader.cpp`) | Overlapped HID reads | `invokeMethod(..., Qt::QueuedConnection)` in `DualSenseDevice` |
| WinMM worker (`std::thread`) | `WinMMDevice` (`src/input/WinMMDevice.cpp`) | Joystick sampling and the slow device rescan (161 ms measured) | Queued `drainSamples()` / `applyScanResult()` |
| GameInput dispatcher (`std::thread`) | `GameInputQtDispatcher` (`src/gameinput/GameInputQtDispatcher.cpp`) | Takes callback batches from `GameInputEventQueue` | Queued `eventsReady` |
| TDLib receive (`QThread::create`) | `telegram::TdJsonTransport::start` (`src/telegram/TdJsonTransport.cpp`) | Blocking `td_receive` loop | Queued `received`, guarded by a mutex-protected target pointer |

Rules the code follows:
- Only the GUI thread touches QML, `AppController`, the models and `CaptureDatabase`. There is one named SQLite connection (`"gamehq"`, opened in `CaptureDatabase::open`). The portable importer opens its own short-lived connections before `App` exists.
- Worker lambdas capture values or shared results, never the owning QObject's members by reference. `ReplayExportTask` explicitly holds only its `SegmentLease` and value captures.
- Segment files used by an export are pinned through `SegmentLease`. Ring trimming and sweeps go through `SegmentLease::removeIfUnleased`.
- `CapturePublisher`'s reservation state and journal are guarded by a single `QMutex`, so encode and export threads can reserve names concurrently.
- `EventLoopStallMonitor` (`src/input/EventLoopStallMonitor.h`) logs GUI-thread stalls of 50 ms or more with a `Perf:` prefix.

## 2. Startup and shutdown order

### `main()` (`src/main.cpp`)
1. Probe modes run before `QApplication` and return early: `--assert-version`, `--release-trust-self-test`, `--localization-assets-self-test`, `--hidhide-allow-self`.
2. `QApplication` is created (`setQuitOnLastWindowClosed(false)`, so the app stays in the tray), followed by `StartupLocalization`.
3. Optional `--import-portable` runs under the instance lock.
4. Update recovery: if an interrupted update transaction exists, `GameHQUpdater.exe --recover` runs and this process exits. If maintenance is active, the process exits with code 4.
5. Single instance: `QLockFile` `%TEMP%/gamehq.lock`. A second instance forwards its arguments through `IntegrationClient` and exits. `ApplicationMutex` is acquired next, for Setup and Uninstall.
6. `App app; app.init();` then `qtApp.exec()`. After leaving the scope that holds `App`, `session.marker` is removed.

### `App::init()` (`src/app/App.cpp`), in order
1. `Paths::ensureDirectories()` runs, then `Logger::install(Paths::logsDir())`. Startup fails if an essential directory cannot be created.
2. Previous-session check: an existing `session.marker` is logged and passed to `InputDiagnostics`. A fresh marker is then written.
3. `IntegrationService` is started.
4. `ConfigManager::loadOrQuarantine()` runs. A result of `Unrecoverable` aborts startup.
5. Localization loads: `LocaleRegistry`, `LanguageManager`, `LanguagePreference`.
6. `CaptureLocations` is created, then `CapturePublisher::setOwnershipJournal(<data>/pending-captures.journal)` and `StartupManager`.
7. `CaptureDatabase::open()` runs (startup aborts on failure). Then `CaptureScanner`, both `GalleryModel`s and `AppController` are created.
8. `ScreenshotService` sweeps stale screenshot `.part` files. `FramePumpService` sweeps stale clip `.part` files and starts the worker thread.
9. Tray, `GameInputFocusController`, `OverlayManager` and `NotificationCenter` are created. The config-quarantine notice, if any, is posted.
10. `UpdateService`, `HotkeyManager`, `SoundEngine`, `InputEngine`, `GameSessionPresetBinder` and `SteamInputHelper` are created, then Share (`share::Service`, Telegram wiring, external provider host).
11. QML `Main` loads (startup aborts if it has no root object). After that, `m_input->start()` arms input, except during post-update validation, where capture is put in `prepareForUpdate`.
12. `QTimer::singleShot(0)` triggers `AppController::rescan` (async scan) and `refreshHdrStatus`.

### Teardown
The tray Exit action posts `QCoreApplication::quit` as a queued call. `exec()` returns and `App` is destroyed:
1. The `~App()` body runs `GameDetector::setExternalContext(nullptr)` and `m_overlay->setNeutralHandoffSource(nullptr)`.
2. Members are destroyed in reverse declaration order (see `src/app/App.h`). QML (`m_engine`, declared last) goes first, then Share/Telegram, `IntegrationService`, `UpdateService`, `NotificationCenter`, and then:
   - `~FramePumpService` calls `stopPump` with `BlockingQueuedConnection`, then `m_thread.quit()` and `wait()`. On the worker, `onThreadFinished` runs `finishExport()`, which joins a running export. `ReplayExportTask::finish` waits 5 s and then keeps waiting rather than kill the export. Then `teardown()` and `RoUninitialize`.
   - `~ScreenshotService` calls `m_encodePool.waitForDone()`.
   - `~InputEngine` calls `shutdown()`: it disconnects consumers, then `MouseHookDevice::stop()` (`WM_QUIT` to the hook thread, which calls `UnhookWindowsHookEx`, then join), the GameInput dispatcher shutdown (join), and clears the pads (Sony readers and WinMM worker are joined in their destructors).
   - Overlay, `GameInputFocusController` (declared before overlay and input so it outlives both), then `HotkeyManager` (`UnregisterHotKey`), tray and `AppController`.
   - `~CaptureScanner` calls `m_pool.waitForDone()` before `CaptureDatabase` closes the connection.
3. `main()` removes `session.marker` and returns. Only now are the lock file and mutex released.

Worker threads are always joined before the objects they post to or read from are destroyed. A clip whose export finishes during shutdown is published on disk, but its queued `clipSaved` is never delivered, so it is not inserted by the save path. It is in the managed clips root, where the scanner indexes media.

## 3. Files GameHQ may delete

| What | Where | Rule | Source |
|---|---|---|---|
| Stale capture `.part` | Screenshot and clip base roots | Only paths listed in `pending-captures.journal`, under that root, not active in this process, a regular file (not a symlink), and older than 600 s. With no journal set, nothing is deleted. Any other `.part` (for example a browser download) is left alone | `CapturePublisher::sweepStale` |
| Failed reservation | Its own `.part` | `CapturePublisher::discard` after a failed encode or remux | `src/capture/CapturePublisher.cpp` |
| Replay segments `*_clip.mp4` | `<data>/replay-cache` | Ring trim; segments older than 600 s, or unreadable on restore, unless leased; empty per-game cache folders are removed | `SegmentRecorder.cpp`, `sweepStaleReplayCache` |
| User-confirmed capture delete | The capture's file, then its thumbnail | Permanent `QFile::remove` (no Recycle Bin). The file goes first; if that fails, the library row is kept and a "Couldn't delete" toast appears. The DB row is soft-deleted (`deleted_at`) | `CaptureLibraryService::deleteItem` |
| Unusable thumbnails, rotated logs, `.update/*` artifacts, Share temp session folders | App data / package `.update` | GameHQ-owned locations only | `ThumbnailService.cpp`, `Logger.cpp`, `main.cpp`, `ShareSecurity.cpp` |

How it knows a `.part` is its own: `reserve()` creates `<final>.part` with `QIODevice::NewOnly` and appends the path to the journal before handing it out. `publish()` renames it to the final name (the rename never overwrites) and drops the journal entry. A `.part` left behind keeps its entry so a later sweep can remove it.

What it never does:
- Delete captures automatically. Media missing from disk is hidden through a session-only TEMP table (`missing_captures`) and shown again when it returns, with favorites intact (`CaptureDatabase.h`).
- Sweep, write into, or trim watched (imported) folders. The scanner only reads them; thumbnails go to `<data>/thumbnails`.
- Note: the user-confirmed delete path does not check the capture's source or favorite flag. Confirming a delete removes the file whether it is in a managed root or a watched folder.

## 4. After a crash

- **Detection:** `session.marker` still exists at the next start. App logs `previous session ended unexpectedly`, and the input diagnostics export reports it. No dialog is shown.
- **Replay ring:** when the buffer is re-armed, `SegmentRecorder::restoreRing` re-adopts `*_clip.mp4` segments left on disk that are under 10 min old and pass `ReplayExporter::hasReadableVideo`. Empty or truncated segments are removed. On worker start, `sweepStaleReplayCache` removes segments older than 10 min.
- **Unfinished captures:** an encode or remux interrupted mid-way leaves only a `.part`, which the scanner ignores. It is removed by the journal sweep on a later start once it is 600 s old.
- **Database:** default SQLite journaling (no WAL pragma is set). `PRAGMA foreign_keys = ON`. Schema migrations run inside transactions with `PRAGMA user_version`. No integrity check runs at startup.
- **Config:** an unreadable `config.json` is renamed to `config.json.corrupt-<stamp>.json`, and GameHQ starts on defaults with a one-time warning notification. If the rename fails, startup is refused.
- **Interrupted update:** handed to `GameHQUpdater.exe --recover` before the instance lock is taken.
- **Crash dump:** `CrashDump::install` (`src/diagnostics/CrashDump.cpp`) runs right after the log is installed. An unhandled SEH exception (unhandled-exception filter) or `std::terminate` (terminate handler, exception code `0xE0000001`) writes `logs/gamehq-crash-YYYYMMDD-HHMMSS-<pid>.dmp` from a pre-started writer thread, so it also works after a stack overflow. Dumps hold thread stacks, thread info and referenced memory, never full process memory (~100 KiB). The module list in the dump identifies the exact binaries; symbols come from the build's separate debug info. At most 5 dumps are kept; only files with that exact name pattern are pruned. Nothing is uploaded. **Not covered:** fail-fast terminations (`RaiseFailFastException`, `__fastfail`, heap-corruption fail-fast such as `0xC0000374` when Windows terminates the process directly) bypass every in-process handler and write no dump here; use Windows Error Reporting LocalDumps for those. If the logs folder is unwritable, no dump is written and the process still dies normally (writer wait capped at 30 s). Covered by `tst_crashdump` (real child-process crashes).

## 5. When a capture counts as valid

**Replay clip** (`FramePumpWorker::saveReplayOnWorker`, `runExport`, `ReplayExporter::concat`):
1. `saveGuard` checks that the buffer state allows a save, that the recorder is active, that no export is running, and that no update is pending.
2. `freezeRing` requires a non-empty leased snapshot, and `hasClosedMedia()` must report a closed segment inside the replay window.
3. A name is reserved under `<clips root>/<game>/Clips/` as `yyyy-MM-dd_HH-mm-ss[_N].mp4.part`.
4. `concat` fails the whole clip if any segment is unreadable or wrote no video samples, if audio presence differs between segments, if audio is required (the recorder has an audio track) and a segment lacks it or no audio samples were exported, or if `Finalize` fails. There is no minimum-duration rule beyond this.
5. Only after a successful `concat` does `CapturePublisher::publish` rename the `.part` to `.mp4`. Then `AppController::commitClip` inserts the row (`type = video`, `source = GameHQ`).

**Screenshot** (`ScreenshotService::encodeAndSave`): refused when 8 or more encodes or 256 MiB are pending. Otherwise it reserves, writes with `QImageWriter` to the `.part`, publishes after the writer closes, then calls `commitCapture`.

**Commit result** (`CaptureCommitOutcome`, `src/ui/CaptureLibraryService.h`):
- `Indexed`: a row was inserted, or the scanner already had one.
- `MediaOnly`: the file is on disk but there is no row. Toast: "Saved to disk, not in library".
- `Failed`: neither.

The scanner skips files newer than a 3 s settle time on watcher-triggered passes, so the service's own commit, with its executable path, lands first.

## 6. Errors and logging

- **Logger** (`src/diagnostics/Logger.cpp`): a Qt message handler writes `<ISO time> DBG|INF|WRN|ERR|FTL <message>` to `<data>/logs/gamehq.log`. It flushes every line, rotates at 8 MiB and keeps `gamehq.1.log` to `gamehq.3.log`. If the file cannot be opened, lines go to stderr.
- **Data directory** (`Paths::dataDir`): `<package>/gamehq-data` when `portable.flag` is present (dev builds: `build/gamehq-data`), otherwise `QStandardPaths::AppDataLocation`.
- **Categories:** none. There is no `Q_LOGGING_CATEGORY`. Code uses plain `qInfo`, `qWarning` and `qCritical` with a component prefix (`FramePump:`, `Capture:`, `DB:`, `Config:`, `MouseHook:`, `Perf:`) or a chain tag such as `ReplaySave[<id>]`, `Screenshot[<id>]` or `CaptureFeedback[<id>]`.
- **Result types:** `bool` plus a `QString* error` or `QString& error` out-parameter (for example `CapturePublisher::publish`, `IntegrationService::start`); outcome enums (`CaptureCommitOutcome`, `ConfigManager::LoadResult`, `ProcessIdentity::WaitOutcome`); result structs (`CaptureDeletionResult`, `Paths::DirectoryStatus`, `CaptureScanner::Delta`); `std::optional` in update parsing. `std::expected` is not used.
- **To the user:**
  - Capture outcomes reach `NotificationCenter` toasts through `App` (`postCaptureOutcome` replaces the receipt card for the same operation id), plus an `error` sound.
  - Replay rejection reasons are localized in `localizedReplayReason`.
  - Failures and media-only commits are shown even when success toasts are muted. They are not shown if notifications are disabled entirely.
  - Fatal `init()` failures are logged with `qCritical` and the process exits with code 1. Portable-import failures use a native `QMessageBox`.

## See also
- [architecture.md](architecture.md)
- [capture-engine.md](capture-engine.md)
- [replay-buffer.md](replay-buffer.md)
- [storage.md](storage.md)
- [database.md](database.md)
