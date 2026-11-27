# Technical Audit

Historical snapshot from 2026-07-16 covering the C++ service layer, QML,
configuration, storage, capture/replay, input, documentation and tests.
Finding status, counts and manual results below describe that snapshot.
[Current architecture and verification routes](architecture.md)

This audit supersedes the 2026-07-09 pass. That earlier audit's findings all
landed; they are summarized under [Prior Wave](#prior-wave-2026-07-09) rather
than repeated as open items.

## Shape at the July audit

GameHQ is a Qt/QML Windows app with a native C++ service layer:

- `App` wires lifetime and signals for config, database, scanner, galleries,
  screenshots, replay frame pump, overlay, notifications, input, sounds, tray,
  and QML context properties.
- `AppController` is the single QML bridge. It owns QML-facing filter state and
  folder commands while delegating capture-library work (`CaptureLibraryService`),
  current-game lookup (`CurrentGameService`), non-plain config keys
  (`SettingsRouter`), and shell actions (`ShellActions`).
- `GalleryModel` is a thin `QAbstractListModel` over
  `CaptureDatabase::listCaptures(category, gameId)`.
- `CaptureDatabase` owns the storage facade, schema/migrations, and mutations.
  Read queries live in `CaptureQueries`; icon extraction, log backfill, and
  duplicate-row repair live in `GameIconCache`, `GameMetadataBackfill`, and
  `GameRowRepair`.
- `GameDetector` resolves the foreground window, process path, likely game title,
  fullscreen heuristic, and capture gate. The disk-backed title sources (Steam
  `.acf` scan, version-resource reads) are memoized per foreground process, keyed
  by pid and path; the window caption stays live per tick.
- `ScreenshotService` handles SDR GDI screenshots, HDR frame-pump requests, and async image writes.
- `FramePumpService` handles WGC replay capture, HDR tone mapping and screenshot readback on a dedicated MTA worker, plus rolling segments and replay export.
- `OverlayManager` owns lazy overlay QML loading, topmost window placement,
  foreground stealing, and focus restoration.
- `InputEngine` routes controller input between global actions, overlay, and
  desktop gallery, dispatching through a static action table.

The responsibility drift the 2026-07-09 audit called out is largely resolved.
The remaining large units are concentrated in two places that are deliberately
left alone: the replay pipeline and the controller backends. Both were tuned and
hand-verified against real games and real hardware, and neither can be safely
restructured without a live re-verification session.

## Findings

### P0 - No Blocker Found

The tree builds and runs. No compile-time or startup blocker was found during
this audit.

### P1 - Startup Cost Paid on Every Launch - Fixed

`GameMetadataBackfill::run()` (full log scan) and
`GameRowRepair::normalizeDuplicateNames()` (O(n^2) games scan) ran on every
launch forever, and `CaptureScanner::scanFolder` issued two DB round-trips per
file during its directory walk.

Landed: both repair passes are gated behind a `repairs_v1` completion sentinel
(`CaptureDatabase::repairsV1Done()`), so they run once and then skip. Capture
lookups are batched into one `CaptureQueries::captureIndex()` snapshot that the
scan diffs in memory.

### P1 - Startup Repair Was Not Atomic - Fixed

The brand-rename / duplicate-collapse / path-renormalization block in
`CaptureDatabase::migrate()` ran as separate auto-commit statements, so a
mid-run crash left partial repairs behind.

Landed: the whole repair region runs in one transaction with rollback.

### P1 - Config Keys Were Bare Strings - Fixed

About 30 `config.json` keys were spelled as string literals across 14 C++ files,
where a typo silently falls back to the default instead of failing.

Landed: `src/config/ConfigKeys.h` declares each key once as a
`constexpr QLatin1StringView`; every C++ call site now goes through it. QML keeps
using string literals - it has no access to the constants. The page ->
config-group reset taxonomy moved to `SettingsCategories`.

### P1 - Duplicated Capture Helpers, One With a Latent Bug - Fixed

The capture modules each carried their own HRESULT logger, timestamped-filename
builder, and QPC conversion. `AudioCapture`'s naive multiply was a latent
A/V-sync overflow; `FramePumpService` already had the overflow-safe formula.

Landed: `src/capture/CaptureUtil.h` holds one `ok()` logger, one overflow-safe
`qpcNow100ns()`, the stale-segment threshold constant, and the shared filename
builder. The audio clock overflow is gone. Live A/V-sync re-verification is still
outstanding - see the deferred register.

### P2 - QML Duplication Against a Now-Complete Token Set

The `Theme.qml` token set had real gaps (`borderHairline`, `radiusXS`, spacing
steps, letter-spacing), and QML worked around them with hex colors, 20 `Qt.rgba`
literals, and hardcoded font sizes. Several components were triplicated.

Landed: 16 tokens added and the literals swept through `Theme`; `VideoBadge.qml`
replaces three inline copies of the play badge; `SettingsPathField.qml` replaces
two copies of the read-only path box; `SidebarCategories.js` grew a
`resolveFilter()` that all three sidebar dispatch blocks now share.

Not done deliberately: the two settings pages still use different divider tokens
(`Theme.borderLight` vs `Theme.stroke`). Picking one is a visual decision, not a
cleanup - it needs a call, not a refactor.

### P2 - Dead Input Code - Fixed

`TapHoldDetector.{h,cpp}` had zero references, and `Gamepad` carried legacy
`buttonPressed/buttonReleased(int)` signals plus an inert `legacyButton`
parameter threaded through all four backends.

Landed: all removed. `controlIdFor`/`buttonName` stay - they do real translation.

### P2 - Legacy Shims Inlined in Startup - Fixed

The PlayHQ/SavePlay lockfile/DB/folder rename compatibility block was inlined in
`App::init`.

Landed: extracted to `config/LegacyMigration`. `App::init` now uses
`Paths::databasePath()` and documents its construct-before-connect ordering
contract.

### P2 - No Automated Tests - Fixed (First Slice)

The project relied entirely on build/run/log verification and real hardware
tests. That is unavoidable for WGC, controller, and overlay behavior, but not for
pure logic.

Landed: an opt-in `tests/` target (QtTest + CTest, `-DGAMEHQ_BUILD_TESTS=ON`).
As of 0.6.42 it is **31 CTest tests** running in about 17 s, plus **20 Playnite
plugin tests** (`dotnet test`); command documented in
[dev-setup.md](dev-setup.md). `CaptureScanner::inferGameName` moved to
`GameIdentity::inferFromPath` as the testing seam, which also removed a
duplicated "Unknown Game" fallback.

The original "pure logic only" rule has one deliberate exception: storage and
process-lifetime correctness is about what survives a failure, which cannot be
checked without a real database, real files or a real child process. Those
tests use `QTemporaryDir` fixtures and disposable child processes
(`tst_capturedatabaserepair`, `tst_capturepublisher`, `tst_processidentity`,
`tst_portableprofileimporter`, `tst_updatertransaction`).

The rule going forward: if a unit needs half the app to build, it is not pure
logic and does not belong in this target.

## Deferred Register

Nothing here is abandoned. Each entry records why it is not closed and what would
close it.

### Code Complete, Awaiting Human Verification

These landed on `dev` and build clean. The agent that wrote them cannot perform
the verification their acceptance requires, so they are held rather than claimed.

| Area | Committed | Gate |
| --- | --- | --- |
| Capture shared utilities | 0.5.59 | Live-game replay with audio ON; confirm A/V sync after the clock-overflow fix |
| AppController config special-casing (`SettingsRouter`) | 0.5.64 | Roughly 2-minute Settings-UI check: change a capture root, toggle startup, reset a group |
| Table-driven `InputEngine::dispatchAction` | 0.5.66 | Keyboard + pad smoke: screenshot, overlay toggle, replay save, D-pad/L1/R1 hold-to-repeat |
| Theme tokens and literal sweep | 0.5.67 | Visual spot-check of gallery, overlay, settings, toasts |
| Unified sidebar filter-select | 0.5.69 | Mandatory pad check - sits on the hand-verified DualSense path |
| Cached `GameDetector` title resolution | 0.5.73 | Live game: Steam title path, codename/exe fallback path, a game switch, and auto-arm still catching a fullscreen game |
| `BulkSelection.qml` state-machine extraction | 0.5.74 | Bulk mode: shift-click a range, then shift-click back over it — it must undo, not leave a trail; plus the pad flow |
| Capture god-function split (`startPump`, `saveReplayOnWorker`, `remuxConcatImpl`, `buildWriter`) | 0.5.93 | Live game: replay buffer arms, 5 s segment rolls without hitching, Share-hold saves a playable clip with correct duration/audio, and a second save while one is exporting is refused cleanly |
| `DualSenseDevice::parseReport` split | 0.5.94 | Pad in hand: DualSense over USB and BT — D-pad and left-stick nav (no doubled events at the deadzone boundary), face buttons, Share-hold replay save, and a DSX/second-pad failover if available |

### Deliberately Out of Scope for This Wave

High-risk rework of code that was tuned against real games and real hardware. The
common thread: each needs a live re-verification session that a code-only pass
cannot provide, so each should ride along with feature work that already forces
that testing.

- **Split the capture god-functions** — landed in 0.5.93 as a structure-only
  split (literal transcription into phase helpers, no behavior change intended):
  `startPump` → createDevices/createCaptureItem/attachRecorder/createSession,
  `saveReplayOnWorker` → saveGuard/freezeRing/instantThumbnail/runExport,
  `remuxConcatImpl` → openSegment/beginConcatWriter/copySamples (the duplicated
  video/audio copy loops unified), `buildWriter` →
  createSegmentSink/addVideoStream/addAudioStream. Still needs the multi-title
  live re-verification above before it counts as verified.
- **Split `DualSenseDevice::parseReport`** — landed in 0.5.94 as a
  structure-only split (literal transcription): buttonBlockBase (USB/BT/DS4
  offset table) → decodeButtons → decodeStickNav (hysteresis unchanged) →
  routeReport (active-pad selection/steal). Needs the pad re-verification above.
- **Unify stick deadzone hysteresis across backends** — landed in 0.5.98 as
  `input/StickNav.h`. The deferral said this needed a real-hardware matrix
  because it changes nav feel. Narrowing the scope to *structure only* (each
  backend keeps its tuned deadzone values) made a feel change impossible by
  construction, which turned a subjective question into an equivalence check —
  and that is machine-provable: 1,765,920 comparisons against the original
  per-backend logic over the full raw input range, zero mismatches. Worth
  generalising: when an item is blocked on "needs hardware", first check whether
  the scope can be narrowed to something provably behaviour-preserving.
- **Decouple `DesktopGalleryGrid` from its `host`** — landed in 0.5.99. The
  deferral held that swapping the 11 host touchpoints for signals would reorder
  the hand-verified pad path. It does not: QML signal handlers run
  synchronously, so `keyboardActivity()` → `window.usingGamepad = false`
  completes before `input.handleKeyPressed()` is reached, exactly as the direct
  write did. Reads became properties (`columns`, `zoomLevel`, `bulkMode`), and
  `bulkIsChecked` stayed a callable predicate rather than a signal so the
  delegate's `checked` remains a binding that re-evaluates on selection change.

### Reassessed and implemented as a parameterized stage (0.5.95)

- **Shared `MediaStage.qml` for Lightbox/OverlayPreview.** The 2026-07-16 pass
  recommended dropping this: the two surfaces share a widget tree, not behavior.
  On the video path they do opposite things — Lightbox leaves the still layer
  empty and lets `VideoOutput` cover the stage, while `OverlayPreview` paints the
  clip's *thumbnail* there until `videoFocused` flips. `OverlayPreview` also
  tracks a committed-vs-requested frame index, carries a `_modelRevision`
  dependency, and clears state on an empty source; Lightbox commits eagerly in
  `openAt()` and has no equivalent.

  Built anyway in 0.5.95 at the owner's direction, with the divergence made
  explicit rather than hidden. `components/MediaStage.qml` owns only what is
  identical — the async decode-then-promote handoff and the player/end-of-media
  wiring — and every rule that differs is a property set by the caller
  (`targetUrl`, `stillVisible`, `videoSource`, `videoVisible`). One behavior flag
  remains: `stopOnEmptySource`, ON for the Lightbox only; the overlay leaves the
  player alone and re-sources it when playback is focused.

  Index tracking and the revision dependency stayed at the overlay call site
  (`onCommitted`/`onCleared` signals) rather than moving into the component.

  0.5.96 closed the remaining gap the split had preserved. The Lightbox used to
  blank `targetUrl` for every clip, so its still layer painted nothing and the
  stage went black while the player loaded — a visible flicker when stepping
  quickly through a mixed gallery. It now decodes the clip's *thumbnail* like the
  overlay does and keeps the still visible; the video surface sits above the
  still and covers it once it has a frame. With that, `targetUrl` is empty only
  when there is genuinely nothing to paint, both callers want clear-on-empty, and
  the `clearOnEmptyTarget` flag that existed purely to tell them apart was
  removed — the two surfaces converged instead of staying parameterized.

## Prior Wave (2026-07-09)

The earlier audit's four phases all landed and are no longer open items:

1. **Low-risk extraction.** `core/GameIdentity` centralized folder-safe names and
   identity keys, replacing duplicated `folderSafeGameKey`/`sanitizeFolder` call
   sites; `hasCapturesForGame()` replaced `listCaptures(...).isEmpty()` existence
   checks; `SidebarCategories.js` unified sidebar category definitions.
2. **AppController facade cleanup.** `ShellActions`, `CaptureLibraryService`, and
   `CurrentGameService` moved work behind a stable QML API.
3. **Database boundary cleanup.** `GameIconCache`, `GameMetadataBackfill`,
   `GameRowRepair`, and `CaptureQueries` left `CaptureDatabase` as mostly SQL and
   migrations.
4. **QML decomposition.** Desktop and overlay chrome componentized; the action
   menu is shared.

The documentation drift that audit recorded (`database.md`, `storage.md`,
`capture-engine.md`, `architecture.md` describing classes that never existed) was
corrected at the time. `Main.qml` is now 726 lines, down from about 1,391;
`OverlayWindow.qml` is 406, down from about 911.

## Direction

Keep public QML and C++ APIs stable. Do not open a broad rewrite - the remaining
large units are large because they are verified, and the cost of re-verifying
them is the real constraint, not the code.

Concretely:

1. Close the seven verification gates above; they are cheap and they unblock
   nothing else, but they are honest debt.
2. Keep every `config.json` key in `ConfigKeys.h`, folder-safe names in
   `GameIdentity`, sidebar category rules in `SidebarCategories.js`, and visual
   values in `Theme.qml`.
3. Grow `tests/` opportunistically - when a pure-logic helper appears, cover it.
4. Let the deferred capture/input splits ride along with feature work that
   already requires live testing.
</content>
</invoke>
