# Changelog

All notable public releases of GameHQ are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Changed

- Release notes in the other fifteen languages are no longer attached to a
  GitHub Release as separate files. They stay in the repository, and the
  release page links them once, so the download list shows only the installable
  packages and the files needed to verify them.

## [0.7.7] - 2026-09-07

### Added

- GameHQ is now available in sixteen interface languages. It follows your
  Windows language the first time it starts, and you can pick a different one in
  Settings.
- Changing the language takes effect immediately across the application,
  including the overlay and the tray menu, without restarting GameHQ. Your
  choice is remembered.
- Setup now runs in your language and passes that choice to GameHQ's first
  launch, so the installer and the application no longer disagree.
- The Playnite extension and the launcher's error dialogs are localized as well.
- What's New is shown in your language whenever a translated version of those
  notes exists. When it does not, the complete English text is shown instead,
  never a mixture of languages.

### Changed

- Dates, numbers and item counts now follow the selected language instead of the
  system default.
- Support GameHQ is now a normal sidebar link under About, matching the other
  entries in size and alignment while keeping its pink accent.
- The language picker moved to the very bottom of the sidebar, below every link.
- The version badge next to the logo now opens About, and turns green with an
  arrow when a new GameHQ version is available; clicking it then opens the
  update.
- Clicking the GameHQ logo and wordmark now returns to the All view.
- The Games section of the sidebar scrolls on its own once the library outgrows
  the available space, so the categories and tools stay in place.

### Fixed

- A capture that cannot be deleted now tells you instead of failing silently.
  It stays in the library rather than disappearing while its file remains on
  disk, and a notification suggests the most likely cause — a program still
  using the file — and what to try.
- Sidebar rows now sit at the same distance from the panel's left and right
  edges.
- Hovering the version badge now stays highlighted until the pointer leaves,
  without showing a redundant tooltip.

## [0.7.6] - 2026-08-31

### Fixed

- Fixed an input path that could cause system-wide mouse stuttering or brief
  freezes while GameHQ was running. Global Mouse Back/Forward/Middle monitoring
  is now installed only while a mouse binding exists or mouse-binding capture
  requires it.
- When global mouse monitoring is required, the Windows low-level mouse hook
  now runs on a dedicated worker thread instead of the GUI thread. Hook
  start/stop and rapid-restart handling were also hardened so held buttons are
  released cleanly and stale queued input cannot survive across hook lifetimes.
- WinMM joystick discovery no longer performs potentially slow device scans on
  the GUI thread, preventing periodic GameHQ interface stalls on systems where
  discovery is slow.

### Added

- Added lightweight, rate-limited performance diagnostics for slow XInput and
  WinMM operations and GUI event-loop stalls. Any remaining hitch should now
  leave useful `Perf:` evidence in `gamehq.log`.

## [0.7.5] - 2026-08-07

### Added

- The full About release-notes view now lists recent version links across the
  top, with the newest selected by default (0.7.5). The current-version summary
  stays compact, while users who skipped 0.7.3 or 0.7.4 can read those changes
  offline without leaving GameHQ.
- The desktop update banner is now a compact notification that opens the
  update-aware release dialog (0.7.5). Available updates open directly on the
  complete changelog with Update, Remind me later and Skip version actions at
  the top, plus version links for the new, current and recent releases.

### Fixed

- The compact About view now shows "Skip version" for a real available update
  without evaluating a removed preview-only helper (0.7.5), preventing a QML
  runtime warning and restoring the intended skip action.
- Closing a gallery video or another desktop dialog no longer leaves
  controller navigation without a visible focus indicator (0.7.5). Focus is
  restored through the verified native foreground path, waits until the main
  window is active, and returns to the active Settings panel instead of the
  hidden gallery grid.
- Custom Global controller gestures remain active when a Desktop or Overlay
  binding uses the same button (0.7.5). Primary-scope trigger ownership now
  suppresses only contextual fallbacks, never Global tap, hold or multi-tap
  actions when the engine represents the fallback scope as Global.
- Pressing Cross during video playback now toggles Play/Pause exactly once
  (0.7.5). Playback now owns the button for its entire press cycle, so the
  Desktop Confirm tap can no longer resume the clip on release and holding the
  button cannot enter Bulk Select behind the lightbox.
- DualSense Share and PS buttons no longer stop responding after the first
  press (0.7.5). When Sony Raw Input delivered a press and the preferred
  GameInput mirror arrived while the button was held, the capability router
  silently transferred ownership of the held control to GameInput; the Sony
  release was then rejected, the control stayed logically held forever, and
  every later press — including standard buttons routed the same way — was
  swallowed as a duplicate. The router now uses press-cycle ownership: the
  first accepted press owns the cycle, mirrored presses only join it as
  participants (never a second action, never an ownership transfer), any
  participant's release closes the cycle exactly once, and a release with no
  open cycle is explicitly ignored. Dead-owner safe releases and lifecycle
  resets are preserved. Regression tests cover repeated Share/PS handoff
  cycles for both Capture and Guide at router and provider-integration level.

## [0.7.4] - 2026-08-07

### Fixed

- HDR screenshots now use the FP16 capture and GPU tone-mapping path by
  default (0.7.4). The implementation was present in earlier public builds,
  but its hidden rollout flag still defaulted to off, forcing HDR games
  through the overexposed 8-bit SDR capture path.
- One controller tap no longer moves UI selection twice (0.7.4). Windows
  mirrors every XInput pad through the legacy WinMM joystick API; the mirrored
  press trailed the real one by a few milliseconds, survived the arbitration
  hold, and was replayed ~250 ms later as a second action. Mirrored presses
  born inside the duplicate window are now dropped outright and
  `heldPressSurvives` rejects them defensively.
- The WinMM VID:PID skip introduced alongside the mirror fix is removed again
  (0.7.4): VID:PID names a controller *model*, not a physical endpoint, so the
  filter could hide a legacy-only pad sharing its model with an XInput one and
  re-logged the skip on every two-second rescan. Arbitration's duplicate-window
  drop is the sole — and sufficient — mirror defense; a production-sequence
  regression test pins both the mirror and the genuine-failover path.
- GameInput now requests non-exclusive background delivery while a game holds
  focus (0.7.4), including Guide/Share delivery where the controller and
  firmware expose those buttons. Share/Capture remains hardware-dependent and
  the GameSir G7 Pro is still unverified.

### Changed

- The button-identification probe now explains keyboard-macro buttons (0.7.4).
  If a controller button only produces a keyboard event (GameSir-style Share
  buttons emit the Windows screenshot shortcut), the probe says so and points
  at the Keyboard bindings section or a vendor remap to an unused key such as
  F13; the no-event message notes that the current firmware mode may disable
  the button entirely.
- The keyboard-macro probe message is phrased as a possibility, not a verdict
  (0.7.4): the probe can only observe that keyboard input arrived while no
  controller input did — it cannot prove the key came from the pad.

## [0.7.3] - 2026-08-05

### Highlights

- **Controller bindings redesigned.** Assign button combinations, triple taps,
  configurable gestures, and complete assignments from one editor.
- **Modern controller foundation.** A GameInput pipeline adds dedicated
  Share/Capture identities, extra-button discovery, wireless and hot-plug
  handling, plus safe legacy fallback. Individual controller models remain
  unverified until hardware-tested.
- **Controller-first interface.** Settings and other scrollable screens now
  support reliable pad navigation, automatic follow-scroll, right-stick
  scrolling, and visible scrollbars.
- **PS-button desktop shortcut.** Hold PS for two seconds to bring GameHQ
  forward; hold it again to hide GameHQ and return focus to the previous game
  or application.
- **Major input hardening.** Duplicate actions, lost presses, high-polling
  controller slowdown, and crashes caused by reentrant dispatch are prevented.

### Fixed

- Settings can now be driven entirely with a controller (0.7.3). Right from
  the category list enters the options panel on its topmost control; Up/Down
  jump to the nearest control in the row below/above instead of visiting
  every segment of a segmented control; Left/Right move within the focused
  row — including between the Primary and Secondary assignment slots of a
  binding card — before backing out to the category list. Cross on an
  assignment slot opens the assignment editor, and while any Input dialog is
  open (assignment editor, conflict, compatibility, restore-defaults) all
  pad directions stay trapped inside it, Cross activates the focused control,
  and Circle is the only way out (cancelling a running capture first).
  Dialogs land the pad cursor on their safe button when they open, closing
  one returns focus to the control that opened it, and quiet buttons such as
  the editor's Change/Record now show a visible focus ring.
- After scrolling a Settings page with the right stick, the next D-pad press
  now continues from what is on screen instead of jumping back to the row the
  pad cursor was left on (0.7.3). Down enters at the top of the visible area
  and Up at the bottom, so the view no longer snaps away from the section the
  user just scrolled to. Entering the options panel from the category list
  behaves the same way, and Left still backs out to the categories.
- The blue "modified" indicator on customized binding rows and assignment
  slots is now half-height and vertically centred, so it no longer pokes
  past the cards' rounded left corners (0.7.3).
- Binding a Hold gesture to Toggle Overlay no longer locks the app in an
  endless overlay show/hide loop (0.7.3). Opening the overlay cancels all
  pending input patterns; that cancellation could land in the middle of the
  hold dispatch and rewind it, so the same hold re-fired the toggle roughly
  20 times per second — regardless of releasing the button — until the
  process ran out of Window Manager handles and crashed. The pattern
  recognizer now detects the mid-dispatch invalidation and stops, so the
  hold fires exactly once. The same guard covers Press bindings whose action
  closes or opens the overlay.
- Two defensive reentrancy guards in the input path (0.7.3, pre-release
  hardening of the same bug class). Raw HID fallback delivery no longer holds
  a reference into the per-device pressed-state store while emitting control
  edges, so a handler that tears the device down mid-batch (binding reload,
  provider detach, disconnect) can no longer leave delivery running on freed
  state; held buttons still release exactly once. Keyboard-hotkey dispatch
  now copies its action id before emitting, so a handler that clears or
  rebinds the same hotkey during dispatch cannot invalidate the action being
  delivered. Both guards are covered by new regression tests.
- Losing the window focus now cancels every in-flight desktop gesture (0.7.3,
  release hardening). A button pressed in the gallery — Cross for confirm, or
  its one-second bulk-select hold — could previously complete after focus had
  already moved to a game, firing a desktop action into the background. Focus
  changes now cancel pending patterns the same way opening the overlay does,
  so the PS-hold Show/Hide GameHQ handoff performs each action exactly once.
- Upgrading from 0.7.1 with a saved PS-button (Guide) "Press" assignment no
  longer disables the new two-second PS hold (0.7.3). The stored overlay
  toggle is migrated from Press to a single Tap — it still toggles the
  overlay, now on release — so it can coexist with the hold. A Guide Press
  the user bound to a different action is preserved untouched; the default
  PS-hold action is instead switched off for that controller profile (never
  both actions from one press) and can be re-assigned in Settings → Input.
- L1/R1 can no longer switch the Settings category while a dropdown opened
  with the pad is still showing (0.7.3); the dropdown keeps the pad to itself
  until it commits or cancels.

### Changed

- Scrollable sections now show a scrollbar (0.7.3). Settings pages, the
  assignment editor dialog, Help, and the release notes share one themed
  scrollbar that is visible whenever the content overflows — not only while
  scrolling — so it is obvious a section scrolls before the first move.
- The pad cursor survives its own actions (0.7.3). Activating a control that
  rebuilds or disables itself — most visibly "Restore defaults", which
  reloads every binding card — used to strand the controller focus so no
  further navigation worked; focus now returns to that control, or to
  whatever sits closest to its old position.
- Simpler gallery navigation with the pad (0.7.3). The thumbnail grid no
  longer row-wraps: pressing Left on the leftmost column enters the sidebar,
  pressing Right on a row's last thumbnail simply stops, and pressing Right
  in the sidebar returns to the grid. Up/Down (held for auto-repeat) remain
  the way to move between rows. L1/R1 keep jumping between the panels as a
  shortcut for now.

### Added

- Hold the PS button for 2 seconds to summon the GameHQ window over the game
  with real focus — hold again while it is focused to hide it and return
  focus to the game (0.7.3). Exposed as a new bindable action ("Show / Hide
  GameHQ Window") in Settings → Input. The PS overlay toggle consequently
  moved from press to tap, so a hold no longer opens the overlay on the way;
  if the overlay is open when the window is summoned, it steps aside and the
  game still gets focus back afterwards.
- Right-stick scrolling on the desktop window (0.7.3). The right stick now
  works like a mouse wheel wherever a view can scroll: the gallery grid, the
  Settings options pages, the assignment editor dialog, Help, and the full
  release notes. Scrolling moves only the viewport — the selection stays put,
  and the next D-pad move snaps the view back to the selected item. Exposed
  as bindable "Scroll Up"/"Scroll Down" desktop actions (Sony and XInput
  backends; WinMM pads have no reliable right-stick axis mapping).
- Pad selection now always stays on screen (0.7.3). The gallery grid
  guarantees the selected tile is fully visible after every D-pad step, and
  the sidebar's games list follows the pad cursor when it walks rows outside
  the clipped viewport — previously the cursor could disappear above or
  below the view with no auto-scroll. The Settings options panel now really
  follows the pad focus too: its reveal-on-focus hookup targeted the window
  through a non-Item attached property, so it silently never connected and
  the focused control could walk out of view.
- Cross-provider controller integration (0.7.3). All input providers — Sony
  Raw Input, GameInput, XInput, WinMM and the selective Raw HID fallback —
  now report into one shared physical-controller registry and route
  Share/Guide presses through one shared capability router, so a single
  physical button press can never execute twice through two provider
  pipelines. GameInput Share/Guide stays withheld for a controller that
  cannot be safely correlated with a connected legacy pad (for example two
  identical controllers, or a remapper changing the reported identity).
  Bindings saved for "this controller" now fire regardless of which provider
  delivers the edge and survive reconnects.
- The selective Raw HID fallback is now a real input producer: a bound button
  on a gamepad-class HID device no backend drives is decoded from its own
  HID report descriptor (press and release), routed through the shared
  dedup, and executes its assigned action. Devices without bound buttons
  keep the zero-cost ignored path.
- Settings → Input now shows a "Controller button layout changed" row with
  "Review buttons" (3-second probe) and "Confirm current layout" actions, so
  extra buttons can be re-enabled after a firmware or mode change instead of
  staying silently disabled.
- The complete GameInput v3 standard-control map (including L3/R3 stick
  clicks, C/Z face buttons and the four paddle flags) is defined and tested,
  and buttons whose reported label names a standard control are excluded
  from "Extra Button N" enumeration so one physical button cannot appear
  under two identities.

### Fixed

- Modern-controller correction gate (post-audit, pre-beta). GameInput
  standard-control readings are shadow-only again until Sony/XInput/WinMM feed
  the same physical-controller registry — routing both pipelines could execute
  one physical press twice. A quick button tap can no longer be lost when
  readings coalesce under load, and a stale reading can no longer resurrect a
  just-removed controller. Turning modern controller support Off and back to
  Auto works again. A controller's anonymous identity is now deterministic
  across sessions and detection order, conflicting device IDs behind one
  container are never merged, and capability changes update the existing
  registry entry. Share and Guide availability are reported per button instead
  of jointly. A changed extra-button layout now releases its held controls
  through the capability router and stays silent until reconfirmed. GameInput
  reading callbacks no longer rebuild device descriptors or touch the layout
  database on every reading.

### Added

- Groundwork for modern controller support: GameHQ now pins Microsoft's
  GameInput 3.5.262, ships the MIT-licensed header and loader in-tree, and a
  small probe tool verifies that the runtime can be found — bundled next to
  the app, installed system-wide, or not at all, in which case the existing
  controller support simply keeps working.

- **Button combinations.** A controller action can now be assigned to two
  buttons: hold the first, press the second. Several combinations can share the
  same first button — the second one decides which action runs. Combinations are
  controller-only, and the buttons keep their normal jobs when the combination
  is not completed.
- **Triple tap**, alongside single and double tap. Tapping three times runs the
  triple-tap action only; it no longer also runs the single and double ones on
  the way there.
- **Edit Assignment dialog.** Assigning a button now opens one dialog that shows
  the whole assignment: single button or combination, how it is pressed, and how
  long a hold takes. Recording a button is an explicit step, so your controller
  keeps navigating the dialog until you ask it to listen — and while it is
  listening, pressing Share records Share instead of taking a screenshot.
- Settings → Input → Gesture timing now also exposes the **multi-tap interval**
  and the **combination window**, and the dialog explains in plain words when an
  action has to wait for one of them.
- The copied diagnostics now include the gesture timing, your controller
  assignments, the last recognized patterns, and whether the Guide/PS button has
  ever reached GameHQ this session — the usual reason a combination never fires.

### Changed

- Controller gestures now count taps instead of hard-coding "double tap", which
  is what will make triple taps assignable. Every existing assignment keeps
  working exactly as before, including per-controller and cleared ones.
- The hold threshold in Settings is now the single source of truth for how long
  "hold" means. Built-in hold bindings follow that setting instead of keeping a
  copy of it, so changing it applies everywhere at once. Hold-to-bulk-select in
  the gallery keeps its own shorter, deliberate one second.

### Fixed

- A controller or keyboard assignment that has become unreadable — corrupted in
  the database, or saved by a newer version of GameHQ that this one does not
  understand — is now ignored and the action falls back to its built-in default
  instead of being guessed at and possibly firing the wrong thing. Every
  ignored assignment is listed in the copied diagnostics so it is obvious which
  one needs to be set again.

- Assigning a controller button to an empty binding slot now keeps the gesture
  that slot really means — a second Screenshot button is a tap, a second Save
  Replay button is a hold — instead of a plain press that was then reported as
  conflicting with everything else sharing the button. The capture prompt now
  shows the gesture being assigned, and clearing a binding no longer quietly
  turns its tap, hold, or double-tap into a press on the next assignment.
- Settings cards no longer log a warning and now fade smoothly when a card
  changes colour, for example when a binding notice appears.
- Choosing Replace in the binding conflict dialog no longer risks losing the
  displaced action's custom assignment if saving fails halfway: the previous
  assignment is put back exactly as it was, not reset to the factory default.
- The overlay now verifies that Windows actually gave it the screen focus when
  it opens, retrying briefly when the system refuses. If focus could not be
  taken, the overlay says so with a small notice instead of silently leaving
  the game reacting to your controller behind it.
- Backend switching (for example DSX changing between Sony and Xbox modes) was
  hardened to preserve the first press and prevent duplicate actions: the
  first button press after a switch is held briefly and then performed once
  the old input path proves it has really gone quiet. A quick tap that waited
  stays a tap — it never turns into a hold — and a press that was only an
  echo of the old path is still discarded.
- The controller button probe now listens for the full three seconds it
  advertises and reads every report from pads up to 8000 Hz, so a quick tap at
  any moment of the window is caught. Only an extreme flood makes it sample,
  spread evenly across the window, and it says so in the result.
- Binding problems now say what actually went wrong instead of sharing one
  message: a shortcut Windows already owns, a binding that could not be saved,
  and a controller button your controller never reports are three different
  notices.
- One button press can no longer trigger two actions when controller software
  (such as DSX) makes the same pad visible to Windows through several input
  APIs at once. GameHQ now treats near-simultaneous reports from a second API
  as echoes of the same press, and only hands control to another API when the
  current one goes silent, disconnects, or a better connection to the same
  controller appears.
- Custom bindings for Xbox-type controllers now follow the physical controller
  where it can be identified, instead of whichever controller happens to sit
  in the same slot. When the controller cannot be identified, Settings says so
  plainly, and a new "Adopt per-slot bindings" action lets you copy older
  slot-based assignments to a recognized controller — nothing is migrated or
  deleted behind your back.

- Pressing the Share button while a clip is playing now grabs one frame from
  that clip. It previously grabbed the frame *and* took a desktop screenshot
  from the same press, saving two files where the user asked for one. Holding
  the same button still saves the replay, and double-tapping it still opens
  the overlay.
- A keyboard shortcut that Windows or another application already owns is no
  longer saved as if it worked. GameHQ now claims the shortcut first and tells
  you when it cannot, leaving your previous shortcut in place instead of
  showing one that silently does nothing until the next restart.

### Changed

- Settings now explains what a button assignment actually does instead of
  refusing anything that looks similar. A genuine clash — two things that
  would fire at once — opens a dialog offering Replace, Choose another, or
  Cancel. Assignments that only *look* like clashes are kept and explained:
  sharing one button between a tap and a hold, an assignment that replaces
  another only while a clip is playing, and a duplicate that has no effect
  each get their own note.
- Assigning the same action to two different keys is no longer reported as a
  conflict. Having both Ctrl+Shift+S and F12 take a screenshot is valid and
  now works without a warning.

### Added

- Controller diagnostics in the copied summary (Settings → Advanced → Copy
  diagnostics): which controller backend is active and every switch, each
  device Windows offered with how GameHQ classified it, measured event rates,
  the last controller inputs received, whether the overlay actually took
  focus, whether a pad is hidden by HidHide, and whether the previous session
  ended unexpectedly. Serial numbers and full device paths are never included.
- "Identify a controller button" in Settings → Input: press it, then press any
  button on your controller within 3 seconds — GameHQ records what actually
  arrived, including buttons it does not recognize, so unsupported controllers
  (such as the GameSir G7 Pro) can be mapped from a report instead of
  guesswork.
- A design document for Exclusive Controller Mode, the feature that would stop
  games receiving input while the overlay is open
  (`docs/design/exclusive-controller-mode.md`). It is not implemented; the
  document records why, and what a future implementation would have to
  guarantee.

### Additional reliability improvements

- Controllers that GameHQ does not drive can no longer slow the app down.
  A pad polling at up to 8000 times a second — several current models
  advertise this — was re-examined on every single report, on the same thread
  that draws the interface. Each device is now identified once and then
  recognised instantly, and its reports are not read any further.
- The Windows raw-input messages GameHQ receives while a game is in the
  foreground are now completed the way Windows documents, instead of being
  dropped once handled.

- The log now records how many controller reports each device sends per
  second, summarised on an interval and only when the number changes
  meaningfully, so a flooding controller is visible without filling the log.

## [0.7.1] - 2026-07-26

### Added

- GameHQ can now check for updates, download and install them, restart
  automatically, and roll back safely when an update fails.
- HDR-aware capture uses GPU tone mapping so screenshots and SDR replay videos
  no longer appear washed out or overexposed on HDR displays.
- A standard per-user Windows Setup package is now available alongside the
  Portable package.
- The new Playnite integration discovers GameHQ, launches it with games,
  forwards game state, and restores that state after reconnects.
- Settings and About have been redesigned with clearer diagnostics, update
  controls, offline release notes, and a What's New view.

### Changed

- Setup now uses restrained GameHQ artwork and a shorter welcome message while
  keeping the standard Windows wizard layout and per-user installation.
- The GameHQ logo is cleaner without the former red recording indicator, and
  Setup uses the normal rounded app logo on later wizard pages.
- Release manifests now use the production Ed25519 trust key, while private
  signing material remains outside the repository and ordinary CI.

### Fixed

- When the gallery is empty, controller focus now stays in the sidebar instead
  of moving into a thumbnail area with nothing to select.
- Controller shortcuts now follow the input backend and device producing real
  button activity, so stale virtual controllers cannot block input in games.
- Screenshots and replay thumbnails are published atomically, and unreadable
  cached thumbnails are regenerated from the working video.
- Portable imports preserve existing data, failed database or configuration
  recovery rolls back safely, and invalid storage paths are reported.
- Long-path handling, IPC reconnects, update discovery, log rotation, and
  Setup/update conflict prevention are more reliable.

## [0.7.0] - 2026-07-25

### Added

- GameHQ can now check for updates, download and install them, restart
  automatically, and roll back safely when an update fails.
- HDR-aware capture uses GPU tone mapping so screenshots and SDR replay videos
  no longer appear washed out or overexposed on HDR displays.
- A standard per-user Windows Setup package is now available alongside the
  Portable package.
- The new Playnite integration discovers GameHQ, launches it with games,
  forwards game state, and restores that state after reconnects.
- Settings and About have been redesigned with clearer diagnostics, update
  controls, offline release notes, and a What's New view.

### Changed

- Setup now uses restrained GameHQ artwork and a shorter welcome message while
  keeping the standard Windows wizard layout and per-user installation.
- The GameHQ logo is cleaner without the former red recording indicator, and
  Setup uses the normal rounded app logo on later wizard pages.
- Release manifests now use the production Ed25519 trust key, while private
  signing material remains outside the repository and ordinary CI.

### Fixed

- When the gallery is empty, controller focus now stays in the sidebar instead
  of moving into a thumbnail area with nothing to select.
- Controller shortcuts now follow the input backend and device producing real
  button activity, so stale virtual controllers cannot block input in games.
- Screenshots and replay thumbnails are published atomically, and unreadable
  cached thumbnails are regenerated from the working video.
- Portable imports preserve existing data, failed database or configuration
  recovery rolls back safely, and invalid storage paths are reported.
- Long-path handling, IPC reconnects, update discovery, log rotation, and
  Setup/update conflict prevention are more reliable.

## [0.6.45] - 2026-07-25

### Fixed

- The unsigned Beta workflow now installs the valid Qt 6.8.3 module set and
  stops on the first failed native command instead of continuing without Qt.

## [0.6.44] - 2026-07-25

### Fixed

- When GitHub limited update checks, GameHQ told you when it would retry but
  then asked again an hour later regardless, and restarting cleared the wait
  entirely. The retry time is now remembered and respected, including across
  restarts. A manual check during that wait tells you when it can try again
  instead of failing.

## [0.6.43] - 2026-07-25

### Changed

- Developer documentation now reports the real size and scope of the automated
  test suite instead of the figures from when it was first added.

## [0.6.42] - 2026-07-25

### Changed

- The build now re-runs the timing-sensitive tests three times each, so a race
  that only fails occasionally is caught before a release rather than after it.

## [0.6.41] - 2026-07-25

### Changed

- Every automated test now has a time limit, so a hung test fails quickly
  instead of consuming the whole build.
- The Playnite plugin records the .NET version it needs, so a build on an older
  toolchain fails clearly instead of producing a different result.
- Release evidence now records which tools produced the build (CMake, Ninja,
  Python, .NET, Qt, compiler, PowerShell and Windows build).

## [0.6.40] - 2026-07-25

### Fixed

- After a restart, the first update check could report "up to date" even when a
  newer release existed, because GameHQ remembered the server's cache tag but
  not the release it described.
- The update check only looked at the 20 most recent releases. Plugin releases
  published between app releases could push the app release out of view, so no
  update was ever found. It now reads more releases, in pages, with a limit.
- GitHub's second kind of rate limit was treated as an ordinary error, so
  GameHQ retried straight back into it instead of waiting.
- The "check at most once a day" timer restarted on every launch, so GameHQ
  checked more often than intended.

## [0.6.39] - 2026-07-25

### Fixed

- `gamehq.log` grew forever. It now rotates at 8 MB and keeps three previous
  files, so logs can never take more than about 32 MB.
- A log folder GameHQ could not write to made the whole session silent. It now
  reports the problem and writes diagnostics to the console instead, and the
  copied diagnostic summary says whether the log is being written.

## [0.6.38] - 2026-07-25

### Fixed

- A portable GameHQ stored a capture folder on another drive, or on a network
  share, as if it were inside the package. The path was then rebuilt against
  the package folder and the captures appeared to be missing. Such paths now
  stay absolute, and libraries already affected recover on their own.
- Importing a portable profile checked only a fixed list of files and tables to
  decide the destination was empty. Custom sounds, and anything a newer version
  of GameHQ stores, were treated as absent and overwritten. The import now
  refuses whenever the destination holds data it does not recognise.
- The import waited for the running GameHQ by process number alone. Windows
  reuses those, so it could wait on an unrelated program, and it treated "cannot
  check" as "already closed" and started replacing the data folder anyway. It
  now identifies the exact process, refuses when it cannot confirm the previous
  instance closed, and holds the single-instance lock for the whole import.

## [0.6.37] - 2026-07-25

### Fixed

- Uninstall checked only whether GameHQ was running, so it could remove an
  installation while an update was still working on it. It now applies the same
  update and recovery checks Setup does.
- A `maintenance.lock` left behind by a crash blocked Setup permanently. Setup
  and Uninstall now tell the difference between an update that is running, one
  that already finished, and one that never completed — the last of which asks
  you to start GameHQ once so it can recover. Neither ever deletes that
  evidence.
- Setup and Uninstall now also notice a GameHQ running in another Windows
  session, which the previous per-session check could not see.

## [0.6.36] - 2026-07-25

### Fixed

- Two screenshots finishing in the same second could be given the same file
  name, so one overwrote the other. A capture now claims its name by creating
  the file exclusively, which two encoder threads cannot both win.
- Screenshots were encoded straight to their final name, so an interrupted or
  failed write left a truncated image in the gallery. A capture is now written
  to a temporary file beside it and appears under its real name only once it is
  complete; leftovers from a crash are cleaned up at startup.
- Holding the capture button no longer grows memory without limit. Once eight
  screenshots or 256 MB of frames are waiting to be saved, further captures are
  refused with a message instead of being queued.

## [0.6.35] - 2026-07-25

### Fixed

- The package launcher built its paths and the command line it passed to
  `app\GameHQ.exe` in fixed buffers, so a long install path was silently
  truncated and a long argument list could overrun the buffer entirely. All of
  it is now dynamic; a command line Windows cannot carry, or an install path
  too deep for Windows to start, is reported instead.
- Arguments are forwarded exactly as typed. Quotes, embedded spaces and
  trailing backslashes previously survived by luck rather than by rule.
- `--post-update` is recognised only as a whole argument. Opening a file whose
  name merely contained that text made the launcher treat the start as part of
  an update and skip the "update in progress" guard.

## [0.6.34] - 2026-07-25

### Added

- Failure-injection tests for the startup library repair. Each durable step of
  the pass — the duplicate-game merge, the "repairs done" marker and the icon
  format marker — is now made to fail on purpose, and the test asserts the
  database comes back exactly as it was and that the next start finishes the
  repair it rolled back.

## [0.6.33] - 2026-07-25

### Fixed

- GameHQ no longer continues past a storage location it could not create. A
  missing data root or captures root now stops startup and names the failing
  path; a missing cache, thumbnail or log directory is reported and startup
  continues, because only diagnostics are affected.

## [0.6.32] - 2026-07-25

### Fixed

- An unreadable `config.json` was silently ignored and the next save then
  overwrote it, destroying the only copy of the user's settings. The file is
  now preserved as `config.json.corrupt-<timestamp>.json` before GameHQ falls
  back to defaults, with one non-blocking notice pointing at it. If it cannot
  be preserved, GameHQ refuses to start rather than discard it.
- Startup database repair could continue outside a transaction, and several of
  its writes ignored their result. A failure part-way through could leave the
  library half-repaired with the "repairs done" marker set, so it was never
  retried. Repair now requires a transaction, checks every statement and
  helper, rolls back on any failure, and records the marker last.
- A database written by a newer GameHQ is now refused instead of being modified
  against a schema this build cannot see.

## [0.6.31] - 2026-07-25

### Fixed

- Playnite state could vanish a few seconds after a reconnect. When Playnite
  reconnected before its previous connection reported the disconnect, the stale
  disconnect expired the source the live connection had just populated. Only
  the last remaining Playnite connection now starts that clock.
- A message handler that disconnects a client no longer leaves the reader
  walking a removed entry when several messages arrived in the same read.
- A reply that could not be queued in full left a truncated frame in the socket
  buffer, which would corrupt every later message on that connection. Such a
  connection is now closed instead.
- Broadcasting maintenance no longer iterates the client list while a failed
  send is removing an entry from it.

## [0.6.30] - 2026-07-25

### Security

- The updater now waits on the exact application process that authorised the
  update, identified by process id *and* process creation time. Windows reuses
  process ids, so a id-only wait could return immediately for an unrelated
  process while GameHQ was still running and holding files.
- The helper opens and verifies that process before it signals READY, so the
  app only exits once a meaningful handle is already held.
- Only an observed clean exit allows the helper to touch files. A timeout, an
  access failure, a reused process id, an abandoned wait or a failed wait all
  abort before any snapshot, extraction or swap.
- A handoff failure now releases maintenance mode, so a failure that changed no
  file can no longer leave Setup and the next launch blocked.

### Fixed

- Update transaction validation no longer reports a stale error message from a
  previous validation attempt.

## [0.6.29] - 2026-07-25

### Security

- The updater now installs only releases authorised by an Ed25519-signed
  release manifest. GameHQ downloads `gamehq-release.json` and
  `gamehq-release.sig` before the package, verifies the signature over the
  exact downloaded bytes with a key compiled into the binary, and accepts the
  archive only when its name, length and SHA-256 match the signed record.
- A manifest can no longer choose its own signing key, and release sequence,
  key activation, rollback and same-sequence equivocation are all enforced,
  with the accepted sequence stored atomically in the user data root.
- The `.sha256` asset is now a manual-verification convenience only. Replacing
  the update archive and its checksum together no longer produces an
  installable update.
- The updater helper repeats the whole verification from the manifest on disk
  before extraction, so a package swapped after the transaction was written is
  rejected before any file is touched.

## [0.6.28] - 2026-07-22

### Changed

- Confirmed that GameHQ remains under MIT and aligned product, package,
  contribution, trademark, and SignPath materials with that decision.
- Added dependency and asset provenance plus optional source-archive tooling.

### Security

- Release validation now rejects license drift, unclassified dependencies or
  assets, private identity data, secret files, credentials, and personal paths.

## [0.6.27] - 2026-07-22

### Added

- Added an installed-only portable-profile import flow under Advanced settings.
- Added transactional staging, strict path rebasing, source immutability checks,
  SQLite validation, evidence output and automatic rollback.

### Security

- Portable imports reject populated destinations, package escapes, unknown
  portable path fields, unsupported schemas and invalid referenced assets.

## [0.6.26] - 2026-07-22

### Added

- Added a clean-Windows unsigned-Beta CI gate that builds and tests GameHQ,
  then produces Setup, Portable, Update, manifest, signature and evidence.
- Added a strict pre-upload audit that recomputes every recorded size and
  SHA-256 value and rejects missing or unexpected release files.

### Security

- CI artifacts remain short-lived test-key evidence and are never published as
  a GitHub Release; publishable tags continue to reject test-key manifests.

## [0.6.25] - 2026-07-22

### Added

- Added a byte-exact Ed25519 release-manifest generator and verifier in an
  explicit test-key mode, covering Setup, Portable and Update artifacts.
- GameHQ, the static updater and the Playnite plugin now consume shared RFC
  8032 and GameHQ-specific verification vectors.
- Added strict signature encoding, trusted/current/next/revoked key handling,
  atomic anti-rollback state and tamper/equivocation tests.

### Security

- Pinned Monocypher 4.0.3 and BouncyCastle.Cryptography 2.6.2 with locked
  integrity metadata and redistributable license notices.
- Test-key manifests are rejected for tagged releases; production private key
  creation and activation remain outside the repository and ordinary CI.

## [0.6.24] - 2026-07-21

### Added

- A pinned, project-local Inno Setup toolchain now builds a full offline,
  per-user Windows installer with stable identity, metadata, shortcuts and
  installed-location discovery registration.
- Playnite 0.4.11 discovers GameHQ through the installer registry contract,
  Windows App Paths, autostart and the standard per-user location in a tested
  deterministic order.
- GameHQ holds an application-lifetime mutex so Setup and Uninstall can wait
  for capture, remux and database work to close normally.
- Security, privacy, download-verification, troubleshooting and code-signing
  policies now document Windows warnings and private vulnerability reporting.

### Changed

- Release packaging now creates one flag-free neutral payload. Portable staging
  alone adds `portable.flag`; Setup consumes the payload unchanged; update
  staging retains its strict program-file allowlist.
- Distribution identity, registry ownership and release artifact names are
  centralized in `packaging/distribution-identity.psd1`.
- Settings -> About now links to the concise Security & Privacy guidance.

## [0.6.23] - 2026-07-21

### Added

- The passive version label at the bottom of the main sidebar is now a full
  **About GameHQ** row. It opens a compact, controller-friendly About and
  What's New modal without leaving the current gallery or Settings page.
- Current-version release notes are bundled as validated structured data, so
  What's New remains available offline. A distinct sidebar mark indicates
  unread notes, while an available update changes the row and action explicitly.

### Changed

- The one-time post-update greeting now opens the same useful What's New modal
  after the desktop window becomes active. It never appears in the game overlay,
  and closing it records the installed version as read.
- Update controls and technical project links remain on Settings -> About; the
  compact modal only presents status, release highlights, and key actions.

## [0.6.22] - 2026-07-21

### Fixed

- The automated test suite could not be run unattended. Test executables are
  not deployed with the Qt runtime, so a plain `ctest` opened one modal
  "Qt6Core.dll was not found" dialog per test and then waited for a human.
  CTest now hands every test the project-local Qt binary directory.
- Two of the new icon tests crashed instead of reporting a result. They ran
  under `QTEST_GUILESS_MAIN`, while the icon extractor's fallback path uses
  `QFileIconProvider` — a QtWidgets class that needs a GUI application object,
  as the app itself always has. Both now run as `QApplication` tests.
- The metadata backfill test used a `games` table without the `last_seen_at`
  column the real database has, so every backfill update silently failed to
  prepare and the test never exercised what it claimed to.

## [0.6.21] - 2026-07-21

### Fixed

- Improving the icon extractor never reached games already in the library. An
  icon is extracted once and pinned in `games.icon_path`, and the only thing
  that rewrote it was detecting that game running again — so an Xbox title sat
  on the generic `.exe` glyph indefinitely, because there was no reason to
  relaunch it. The database now re-extracts icons for every game with a known
  executable whenever `GameIconCache`'s format version moves, recorded as
  `internal.icon_format`.

## [0.6.20] - 2026-07-21

### Fixed

- Xbox app game installs still showed the generic `.exe` glyph. The 0.6.18
  package-manifest lookup only searched for `AppxManifest.xml`, but GDK titles
  under `XboxGames\<title>\Content` ship `MicrosoftGame.config` instead, which
  declares the same artwork as attributes on its `<ShellVisuals>` element.
  `GameIconCache` now accepts either manifest and also reads GDK's
  `Square480x480Logo`.
- Icon extraction was silent about why it fell back to the shell icon. It now
  logs the manifest it found, the logo asset it used, and the specific failure
  (no manifest nearby, manifest declares no logo, or no declared asset present
  on disk).

### Changed

- The icon cache format version moved to `v3`, so executables cached as the
  generic glyph by 0.6.19 are re-extracted through the fixed manifest lookup.

## [0.6.19] - 2026-07-21

### Added

- The overlay capture strip's hover icons are now live for the mouse: the
  heart toggles the favourite, the folder icon reveals the capture on disk,
  and the trash icon opens a clickable "Delete capture?" confirmation. They
  previously only rendered — every click was swallowed, and deleting from the
  overlay needed the pad's action menu.

### Fixed

- Games without a Steam name (Xbox/MSIX, itch, standalone launchers) never got
  an icon. The historical-detection backfill only accepted the log line's
  `steam:` candidate, so a `steam: <none>` title was skipped and its
  `icon_path` stayed empty forever; it now matches on any logged candidate
  (window title, `ProductName`, `FileDescription` or executable name). The
  candidates are ranked and the whole log is scanned before anything is
  written, because a launcher shim is logged under the game's window title one
  line ahead of the real executable — first-match-wins would have bound the
  shim's path (`gamingservicesui.exe`) as the game.
- The icon cache key now carries an extractor format version. A cache hit
  short-circuits extraction, so executables already cached by the previous
  shell-icon-only extractor kept serving the generic `.exe` glyph and never
  reached the 0.6.18 package-manifest path.

## [0.6.18] - 2026-07-21

### Added

- The overlay sidebar now prints the running app version under the GameHQ
  logo, so the build in use is readable without opening Settings.

### Fixed

- MSIX/Appx games (Xbox app installs under `XboxGames\<title>\Content`) showed
  the generic Windows application icon in the sidebar. Their executables carry
  no icon resource — the artwork lives in `AppxManifest.xml` — so
  `GameIconCache` now resolves the manifest's logo (largest square tile first,
  falling back through `Square310x310Logo`, `Square44x44Logo` and
  `<Properties><Logo>`) and picks the highest-resolution qualified variant on
  disk (`Square150x150Logo.scale-200.png` and friends) before falling back to
  the shell icon.

## [0.6.17] - 2026-07-21

### Fixed

- `GameIconCache::iconPathForExecutable` failed silently on every error path
  (missing file, no embedded icon resource, PNG write failure), leaving no
  trace of why a detected game ended up with a blank icon. All three cases
  now log a `qWarning`.

## [0.6.16] - 2026-07-21

### Changed

- Replaced the experimental HDR tone-map curve's hard clip above SDR white
  with a real shoulder: identity below a 0.9 knee, then a smooth,
  monotonically compressing curve that never quite reaches full white.
  Highlights at 1.5x/2x/4x/8x/16x reference white now stay visually
  distinct instead of clipping to one flat value — a hard clip discarded
  all highlight detail, defeating the point of tone mapping. SDR white
  (1.0) is now intentionally slightly below 255 (headroom reserved for
  highlights) rather than pixel-identical to the untouched SDR path.
- `FramePumpService::createSession` now falls back to the BGRA8 SDR pool
  if FP16 pool creation itself fails after every earlier gate passed,
  instead of aborting the capture attempt outright.
- Extracted the FP16-attempt decision into `capture::hdr::shouldAttemptFp16Capture`
  (pure function, unit-tested) so `internal.capture.experimental_hdr`
  being off is verified, not just asserted by code review.

## [0.6.15] - 2026-07-20

### Added

- Experimental HDR capture stage for the replay pipeline (t24), hidden
  behind `internal.capture.experimental_hdr` (default off, not exposed in
  Settings). When enabled, the display is HDR-active, and the GPU reports
  FP16 texture/sample support, `FramePumpService` captures an FP16 scRGB
  frame pool and tone-maps each frame to BGRA8 on the GPU before handing it
  to the existing `SegmentRecorder` — the recorder itself is unchanged, and
  any failure at any gate falls back to the original BGRA8 SDR pool.
- `capture::hdr::HdrToneMapMath` (CPU reference) and `capture::hdr::GpuToneMapper`
  (GPU shader stage): identity in the SDR range [0,1], hard-clipped to white
  above it, then sRGB-encoded — see the code comments for why a perceptual
  curve is deferred. Covered by `tst_hdrtonemap` (pure logic) and
  `tst_hdrgputonemap` (opt-in GPU smoke test, self-skips without a D3D11
  device or FP16 support).

### Status

Code-complete and verified by this machine's own build/test/GPU smoke test,
but **not accepted** — nobody has run it against a real HDR-active display
or a live HDR game yet. Stays hidden until that hardware acceptance pass.

## [0.6.14] - 2026-07-20

### Fixed

- Update discovery no longer relies on `/releases/latest`, which could return
  the wrong release once the upcoming Playnite plugin starts publishing its
  own `playnite-v*` tags into this repo. `GitHubReleaseSource` now scans the
  release list and selects the highest-versioned entry that is non-draft,
  non-prerelease, has a tag matching the app's `vX.Y.Z` pattern, and carries
  the exact update ZIP + checksum assets — a plugin release can never be
  mistaken for an app update.

## [0.6.13] - 2026-07-20

### Added

- HDR detection and diagnostics: GameHQ now reports, per display, whether
  Windows HDR is currently active, along with bit depth and the display's
  luminance range, and probes whether an HEVC Main10 encoder is installed.
- Settings → Advanced shows a "Display HDR" row with the full report and a
  Refresh button; the report is also logged at startup, re-probed when the
  display topology changes, and included in the copied diagnostic summary.
- The replay buffer logs the HDR state of the display its capture target sits
  on when arming, making it obvious when HDR content is being captured through
  the SDR path.

Detection only: capture, screenshots and clips are unchanged and still SDR.

## [0.6.12] - 2026-07-20

### Added

- Install-and-restart controls: the update banner and About page now cover the
  full install flow (ready, preparing, installing) with state-specific actions,
  and a failed check offers "Check again" instead of a misleading retry.
- Updater READY handshake: the app only exits after the helper confirms it
  validated the transaction, and the helper waits for the old process to fully
  exit before touching any file.
- Persistent `.update/updater.log` recording every helper outcome, retained
  across the post-update cleanup for diagnostics.

### Fixed

- `UpdateService` double ownership: the service is no longer parented to the
  QML engine while also held by the application, removing a shutdown
  double-delete.
- Data-restore rollback now reverses only the operations it performed instead
  of deleting every known state file, so an aborted restore can no longer
  discard an untouched database.
- A release whose assets are still uploading is no longer offered, and its
  ETag is not cached (previously the check could stick on 304 and never see
  the finished assets).
- A failed health-token release from job supervision now rolls back instead of
  reporting success while the new process is killed on helper exit.
- Screenshot encoding now finishes before shutdown, preventing a crash when
  quitting during a background encode.
- Integration clients rejected for malformed frames free their connection slot
  immediately, and install-directory game matching no longer accepts
  executables from a different drive.
- Completed updates also clean the downloaded package and stale transaction
  file; the About page License link points at the real repository file.

## [0.6.11] - 2026-07-20

### Added

- Same-user `GameHQ.Local.v1` integration channel with bounded 64 KiB framing,
  strict UTF-8/JSON/type validation, handshake negotiation, lifecycle snapshots,
  structured replies, disconnect expiry, and hostile-input tests.
- Friendly second-instance forwarding for window activation and gallery opening,
  with short connection/reply bounds and the existing lock as final authority.
- Playnite identity hints for foreground detection. Exact or descendant process
  evidence can recognize windowed games; names and directory hints never weaken
  existing capture-safety gates.
- Durable update-maintenance suppression across the app, launcher, helper and
  local protocol, including terminal cleanup and five-minute stale recovery.

## [0.6.10] - 2026-07-20

### Added

- Safe update-package download and cancellation: HTTPS-only redirects, bounded
  streaming into install-local `.partial` files, atomic publication, stale
  partial cleanup, exact release-size enforcement, and visible progress.
- Strict `.sha256` parsing and local SHA-256 verification. Malformed checksum
  files, mismatched package names, truncated downloads, and corrupted packages
  are deleted before the update can enter `ReadyToInstall`.
- Focused updater tests covering accepted checksum formats and rejection of
  malformed, mismatched, and corrupted packages.
- Static `GameHQUpdater.exe` Stage 1 foundation with a strict transaction
  schema, canonical package-root path containment, a per-transaction mutex,
  and `--dry-run` output listing every planned operation without file writes.
- Transaction tests prove a valid dry run leaves staging and backup absent,
  while an escaped backup path is rejected before anything is created.
- Hardened Stage 2 ZIP staging with pinned miniz 3.1.2: the helper re-hashes
  the package, enforces file/count/size/method and positive-path allowlists,
  rejects traversal, links, user-data paths and invalid manifests/layouts,
  and removes staging after every rejection without touching live files.
- Update quiescence barrier: new screenshot/replay requests are blocked during
  preparation, in-flight writes and clip exports finish naturally, replay
  auto-arming stops, configuration is flushed, and a 30-second timeout cancels
  the update instead of cancelling or killing capture work.
- Helper-side data snapshot and restoration for config plus SQLite DB/WAL/SHM.
  The manifest preserves originally absent sidecars, restoration has its own
  rollback path, and automated mutation/restore coverage proves `Captures/`
  remains byte-identical.
- Allowlisted program-file swap with bounded Windows lock retries and reverse-
  order rollback. Tests cover a successful swap preserving `portable.flag`
  and a deliberately locked executable aborting with old and staged files intact.
- Healthy-start validation keeps capture hooks disarmed until the upgraded app
  survives seven seconds with its database, services, QML and event loop active;
  the helper accepts only the matching version token and times out safely.
- Durable update phase and swap journals make interrupted replacement
  deterministic. Failed health checks stop the supervised process tree, restore
  program and data state together, and restart the previous version.
- Updater helpers now advertise and enforce their protocol version. A packaged
  replacement is staged under a pending name, self-tested, and promoted by a
  later launcher only after the previous helper has exited.
- End-to-end transaction coverage exercises abandoned staging cleanup,
  extraction, snapshot, swap and healthy launch while proving settings,
  captures and `portable.flag` remain byte-identical.
- Automatic-update preflight now rejects unpackaged, unwritable, network,
  unsupported-filesystem, overlong, low-space and active-transaction targets;
  packaged autostart always uses the recovery-aware root launcher.
- A successful health-validated update records and shows a one-time greeting
  with a version-specific What's New link, deferred until the desktop window
  is visible when GameHQ starts minimized.
- Release packaging now emits separate portable and update-only ZIPs plus a
  SHA-256 file. A mandatory validator rejects missing, forbidden, mismatched or
  untested artifacts before publication.
- The final install action now revalidates the GitHub release, writes a helper-
  validated transaction, launches the updater, and exits only after successful
  handoff; withdrawn, superseded or changed releases are refused.

## [0.6.9] - 2026-07-20

### Added

- Automatic update-check policy and a non-modal update banner (Phase 1 of
  the updater plan, `docs/updater.md`). `App::init()` primes `UpdateService`
  from config, runs the first automatic check 15-30s after startup, and
  re-checks at most once every 24 hours via an hourly gate timer; manual
  checks always bypass that cache. New config keys: `updates.check_automatically`
  (default on), `updates.skipped_version`, and internal persistence keys
  `internal.updates.etag` / `internal.updates.last_check_utc` (survive
  "Restore all settings" like other `internal.*` keys).
- `UpdateBanner.qml`: shown only in the desktop gallery window (never over
  the pad overlay or a running game) when a newer stable release exists —
  version, publish date, size, and release notes rendered as plain text
  (no Markdown/HTML interpretation), with "View on GitHub" (the standalone
  fallback until download/install lands in Phase 2), "Skip this version",
  and "Not now".
- Settings → About "Updates" section now reflects real state (checking /
  up to date / update available / last-checked time) with a working
  "Check now" button and a "Check automatically" toggle, replacing the
  placeholder shipped in 0.6.6.

## [0.6.8] - 2026-07-20

### Added

- Release lookup and update-check service (`src/updates/`): `ReleaseInfo`,
  `GitHubReleaseSource` (queries the GitHub releases API with ETag caching,
  a bounded retry, and confirmed-rate-limit detection via
  `x-ratelimit-remaining`/`x-ratelimit-reset`), and `UpdateService`, a state
  machine (`Idle`/`Checking`/`UpToDate`/`UpdateAvailable`/…/`Failed`)
  exposed to QML as the `updates` context object. Only exact-named
  `GameHQ-<version>-win64-update.zip` (+ `.sha256`) assets are selected;
  drafts, prereleases, and releases not newer than the installed version are
  rejected. A failed or rate-limited check never regresses a known-good
  result. Download/install commands are declared but not yet implemented —
  that lands with the safe updater helper.

## [0.6.7] - 2026-07-20

### Added

- Linked the Qt Network module (needed by the upcoming update checker and
  the local integration channel).
- `VersionNumber` (`src/updates/`): strict `major.minor.patch` parsing and
  numeric comparison for release version strings, with an optional leading
  `v`/`V` and no prerelease/build-metadata suffix accepted. Versions are
  never compared as strings. Covered by `tst_versionnumber` (valid/invalid
  parsing, numeric ordering, `v`-prefix equivalence).

## [0.6.6] - 2026-07-20

### Added

- About settings page (`Settings → About`): application logo, name, version,
  storage mode, an Updates placeholder for the upcoming update checker, and
  project links (website, GitHub, releases, issues, license) plus a GitHub
  star call-to-action, all reading from `Brand.qml`. Version and Storage mode
  rows moved here from the Advanced page.

## [0.6.5] - 2026-07-20

### Changed

- Centralized project links (website, repository, releases, issues) in the
  `Brand.qml` singleton instead of hard-coding them per page. The GitHub
  link in the Help view now reads from `Brand.repositoryUrl`.

## [0.6.4] - 2026-07-20

### Added

- Design documentation for the upcoming update system and local integration
  channel: [`docs/updater.md`](docs/updater.md) (path ownership contract,
  the nine-stage helper flow, authenticity limits) and
  [`docs/integration-protocol.md`](docs/integration-protocol.md) (the
  `GameHQ.Local.v1` named-pipe protocol used by the future Playnite
  companion plugin).
- Reserved the canonical identifiers for that work (pipe name, release asset
  naming, plugin repo name, add-on identifiers) so none are invented ad hoc
  during implementation.
- Added the `0.6.x — Distribution & Integration Foundation` milestone to
  [`docs/roadmap.md`](docs/roadmap.md).

## [0.6.3] - 2026-07-18

### Added

- Hidden-controller detection: the app now cross-checks the Windows PnP
  device tree against Raw Input on every device change. A supported Sony/DS4
  pad that Windows sees but applications cannot (the signature of the
  HidHide filter driver installed alongside DSX / DS4Windows / reWASD) is
  reported in Settings → Input with a clear explanation instead of the app
  silently detecting nothing.
- One-click remedy for HidHide-hidden pads: a "Fix automatically" button in
  Settings → Input relaunches GameHQ elevated (single UAC prompt) and adds
  the app to HidHide's application allow-list through the driver's documented
  control interface — no third-party tools, nothing installed or removed;
  DSX setups keep working.

### Changed

- The overlay sidebar now centers the GameHQ brand lockup and uses the same
  larger icon, bright label, and semibold typography as the desktop sidebar.
- The original navy theme is now named **Blue** in Settings; its internal
  `dark` key remains supported so existing preferences keep working.

### Fixed

- Fresh installations now actually start with Obsidian. The QML fallback and
  Settings reset already selected Obsidian, but the C++ configuration defaults
  still returned `dark` and overrode both on first launch.
- Raw Input no longer tracks non-gamepad HID collections on supported
  hardware IDs. The PlayStation Link adapter (054C:0ECC) exposes four
  vendor-defined collections that were logged as four tracked "DualSense"
  devices which could never send input, masking real detection problems.

## [0.6.2] - 2026-07-18

### Fixed

- Overlay preview no longer reads a stale gallery record after a fresh
  capture. `OverlayPreview` resolved the displayed record with an imperative
  `galleryModel.get()` call that no model signal re-evaluated, so a
  just-saved capture — which is prepended at row 0 — left the binding
  pointing at the previous capture. Two visible symptoms, one cause: X
  refused to start playback on a clip recorded moments earlier (the stale
  record reported a screenshot, so `toggleVideoPlayback` bailed out), and a
  fresh screenshot painted a play badge over the stage (the stale record
  reported a video). The record binding now tracks the same
  `_modelRevision` counter the target-URL binding already used, and that
  counter also advances on row removal and moves.

## [0.6.1] - 2026-07-18

### Added

- Overlay auto-hides on any OS foreground-focus change — pressing the Windows
  key, Alt-Tab, opening the task switcher, or clicking another app now closes
  the overlay automatically, the same way Circle or click-outside does.
  Implemented generically via a `SetWinEventHook(EVENT_SYSTEM_FOREGROUND)`
  watcher in `OverlayManager` rather than special-casing individual key
  combos, so it catches every system focus-changing operation. Unlike a
  normal close, this auto-hide does not force focus back onto the
  previously-focused game, since that would fight whatever the user just
  opened (Start menu, task switcher).

## [0.6.0] - 2026-07-18

### Added

- Save a still frame from a recorded clip. While a clip is focused for playback
  in the overlay or the desktop lightbox, pressing **Share** (or **S** on the
  keyboard) now grabs the exact frame currently on screen — paused on a chosen
  frame or mid-playback — and saves it as a screenshot, instead of capturing the
  whole desktop. The frame is taken at the clip's native resolution from the
  video surface, attributed to the clip's game, and written through the normal
  screenshot pipeline (same folder, format/quality, shutter sound, toast and
  gallery row). New `playback.frame_grab` action (Playback scope), so Share only
  changes meaning while a clip is focused and stays the global screenshot
  everywhere else.

## [0.5.99] - 2026-07-17

### Changed

- `DesktopGalleryGrid` no longer reaches into the main window. It now takes
  what it displays as properties (`columns`, `zoomLevel`, `bulkMode`,
  `bulkIsChecked`) and emits signals for what it wants done
  (`keyboardActivity`, `bulkToggleRequested`, `bulkDeleteRequested`,
  `bulkSelectAllRequested`), matching the pattern its sibling
  `DesktopGalleryHeader` already used. Gallery behaviour, including the pad
  bulk-selection flow and nav-lock timing, is unchanged.

## [0.5.98] - 2026-07-17

### Changed

- The "left stick doubles as the D-pad" rule now lives in one place
  (`input/StickNav.h`) instead of being hand-rolled in the DualSense, XInput
  and WinMM backends. Each backend keeps its own tuned deadzone values and its
  existing hysteresis behaviour, so pad navigation feels exactly as before;
  only the shared structure — axis polarity, mutually exclusive directions and
  the optional hysteresis — moved.

## [0.5.97] - 2026-07-17

### Added

- Settings → General → Appearance gained an "Overlay dimming" slider
  (25–150 %, default 100 %) that scales how strongly the in-game overlay
  darkens the game behind it. 100 % keeps each theme's own dimming; lower
  values keep more of the game visible, higher values darken it further
  (capped just short of opaque). Stored as `theme.overlay_scrim_strength`
  and applied live via the new `Theme.overlayScrim` token.
- New reusable `SettingsSlider` control: an integer-valued slider bound to a
  config key that follows the drag live but writes config only on release.

## [0.5.96] - 2026-07-17

### Fixed

- The desktop Lightbox no longer blanks the stage when a clip is selected. It
  now decodes the clip's thumbnail onto the still layer — as the overlay preview
  already did — and the video surface above it covers that once the player has a
  frame. Stepping quickly between captures in a mixed gallery no longer flickers
  the item away before the next one appears.

### Changed

- Internal: with both surfaces now keeping a decodable still behind a clip, the
  `MediaStage.qml` `clearOnEmptyTarget` flag that existed purely to tell them
  apart was removed; an empty target now always clears the committed still.

## [0.5.95] - 2026-07-17

### Changed

- Internal: the desktop Lightbox and the overlay preview now share a single
  `components/MediaStage.qml` for their double-buffered still/clip stage, with
  no intended behavior change. The shared component owns the parts that were
  genuinely identical — the async decode-then-promote handoff that keeps the
  previous capture painted while the next one decodes, and the media
  player/end-of-media wiring. The two surfaces disagree about what sits behind
  a clip (the Lightbox paints nothing there, the overlay keeps the clip's
  thumbnail up until playback is focused), so those rules stay explicit at each
  call site instead of being duplicated.

## [0.5.94] - 2026-07-17

### Changed

- Internal: `DualSenseDevice::parseReport` was split into named stages with no
  intended behavior change — report-layout offset lookup, button decoding,
  stick-to-D-pad decoding (hysteresis untouched), and active-pad routing.
  The stick deadzone values and the USB/BT/DS4 offsets are byte-for-byte the
  same; pending a pad-in-hand re-verification pass. Deadzone unification
  across XInput/WinMM stays deferred.

## [0.5.93] - 2026-07-17

### Changed

- Internal: the four large replay-pipeline functions were split into named
  phase helpers with no intended behavior change — `startPump` (device/item/
  recorder/session bring-up), `saveReplayOnWorker` (guard/freeze/thumbnail/
  export stages), `remuxConcatImpl` (segment open, writer setup, and one
  shared sample-copy loop for video and audio), and `SegmentRecorder::
  buildWriter` (sink creation plus video and audio stream setup). Pending a
  live-game re-verification pass of the replay buffer and clip saving.

## [0.5.92] - 2026-07-17

### Fixed

- The replay buffer no longer arms on fullscreen windows that are not games.
  A game was detected as "covers the monitor and is not a known shell
  process", which also matches every overlay — the Snipping Tool's screen-clip
  layer armed the buffer and recorded the desktop. Overlay windows are now
  rejected by their extended styles (layered, click-through, tool window, or
  no-activate), none of which a game's render window can carry, and the
  Snipping Tool's processes joined the shell blocklist.

## [0.5.91] - 2026-07-17

### Fixed

- The highlighted row in a Settings dropdown no longer paints its square
  corners over the popup's rounded ones. The list's `clip` only ever clipped
  to a plain bounding box, so the top and bottom rows overflowed the corner
  radius; the list is now masked to the popup's rounded shape.

## [0.5.90] - 2026-07-17

### Fixed

- A Share-hold (or `Ctrl+Shift+E`) made while the replay buffer was off now
  arms the buffer instead of only reporting "Replay buffer is not running".
  The gesture itself always fired on time; with nothing recording there was
  no footage to save, so the hold looked ignored and invited ever longer
  presses. The first hold now starts recording and says so, the next one
  saves a real clip.

## [0.5.89] - 2026-07-17

### Removed

- The `Ctrl+Shift+R` replay-buffer toggle hotkey. Always-on recording is
  enabled by default and its only control is now the switch in
  Settings → Replay — a stray key press can no longer silently persist
  recording off, which made replay saves fail with "Replay buffer is not
  running" while screenshots kept working.

## [0.5.88] - 2026-07-17

### Fixed

- The GameHQ logo at the bottom of the overlay sidebar now lines up with the
  icons of the entries above it, instead of sitting slightly further left.

## [0.5.87] - 2026-07-17

### Added

- The tray menu now has monochrome icons — a grid for Open Gallery, a circular
  arrow for Rescan, a camera for Take Screenshot, a clip for Save Replay, and a
  power mark for Exit. They are drawn from the menu's own palette rather than
  bundled as images, so they follow the Windows light/dark setting, and the
  labels no longer float beside an empty icon column.

## [0.5.86] - 2026-07-16

### Added

- **Options** opens Settings from anywhere in the gallery.
- **L2 / R2** shrink and grow the gallery thumbnails, repeating while held.
  The triggers are new bindable controls (L2/R2 on PlayStation, LT/RT on Xbox,
  ZL/ZR on Nintendo) — they were not addressable before.
- **Hold Cross** for a second enters bulk selection; **Square → Bulk select**
  does the same from the action menu.

### Changed

- Cross now confirms on tap-release rather than on press, so that holding it
  can mean something else. A quick tap behaves as before.

## [0.5.85] - 2026-07-16

### Fixed

- A focused Settings dropdown showed no highlight, so pad users could not tell
  where focus was and the control read as unreachable. Its border only lit on
  press or while open — never on focus, unlike every other settings control.

### Changed

- The PS5 theme is now called **Obsidian** and is the default theme.

## [0.5.84] - 2026-07-16

### Changed

- Settings now reads as three panels on a controller — sidebar │ categories │
  options. Left/Right moves between the panels; Up/Down moves inside the
  focused one and can no longer wander out of it. Cross flips a toggle or opens
  a dropdown; inside a dropdown Up/Down moves the highlight, Cross commits, and
  Circle backs out without changing the setting. Circle unwinds one step at a
  time: dropdown → options → categories → sidebar → out of Settings.
  Previously Left/Right switched category and Up/Down walked the raw focus
  chain straight across panel boundaries.

## [0.5.83] - 2026-07-16

### Changed

- The PS5 theme was too blue: its surfaces read as navy where the console's are
  near-black and almost neutral, and its backdrop bloom was a saturated
  blue/violet wash rather than the faint cool glow the real dashboard uses.
  Surfaces are now near-black with a slight cool cast, the bloom drops from 0.5
  to 0.2, and the violet orb is replaced by a cool teal.

## [0.5.82] - 2026-07-16

### Added

- New **PS5** theme, modeled on the PlayStation 5 dashboard: a near-black cool
  navy lit by a soft blue-violet bloom, pure white type, and generous spacing.
  The accents are Sony's published brand colors — PlayStation Blue (#003791) and
  X Blue (#0070D1) for the accent ramp, Triangle Green for success, Circle Pink
  for danger (circle is cancel/back on PlayStation), and Square Purple tinting
  the backdrop bloom. Type is Segoe UI, the closest match on Windows to Sony's
  proprietary SST.

## [0.5.81] - 2026-07-16

### Fixed

- After using a Settings dropdown (for example the theme picker), the Up/Down
  arrow keys kept driving that dropdown even after clicking elsewhere — so the
  arrows went on changing the theme from anywhere on the page. Clicking any
  non-interactive part of a settings page now drops keyboard focus, and the
  arrows only drive a control while that control is focused.

## [0.5.80] - 2026-07-16

### Fixed

- The app still died on startup (blank white window, then an access-violation
  crash) on every theme, not just the textured ones. 0.5.79 stopped the texture
  tile from exporting itself in a loop, but the export still ran from inside the
  canvas paint handler, which released the render target mid-paint. The export
  now runs once the canvas is idle.
- Themes with no texture (Dark, Light, High contrast, Cobalt, Emerald) no longer
  export an empty texture tile on every start.

## [0.5.79] - 2026-07-16

### Fixed

- The app crashed on startup (blank white window, then gone) when the active
  theme uses a background texture (Dracula, Gruvbox, Nord, Midnight, Harbor,
  Synthwave, Carbon). Exporting the generated texture tile re-triggered its own
  paint handler in an endless loop; it now exports exactly once per repaint.

## [0.5.78] - 2026-07-16

### Added

- Themes can now lay a faint texture over the background: film grain
  (Dracula, Gruvbox, Nord, Midnight), diagonal hatching (Harbor), a blueprint
  grid under Synthwave's scanlines, and a carbon-fiber weave for Carbon. The
  patterns are generated by the app itself -- no images shipped -- so they stay
  crisp at any window size and take their color from the theme. They are kept
  deliberately subtle: atmosphere behind your captures, never a pattern
  competing with them. Dark, Light, High contrast, Emerald and Cobalt are
  untouched.

## [0.5.77] - 2026-07-16

### Added

- Nine more themes, and they now change more than color. A theme can also set
  the typeface, how round the corners are, how heavy the borders are, how fast
  the app responds, and what gets painted behind everything -- so they actually
  feel different rather than just looking recolored.
  - **Midnight** -- near-black with soft blue light pooling behind the content;
    slow and cinematic.
  - **Emerald** -- charcoal and green, quick and immediate.
  - **Harbor** -- flat blue-grey with squared-off edges; deliberately plain.
  - **Carbon** -- layered greys and one bright blue; the quietest of the set.
  - **Cobalt** -- soft greys and indigo, very round, the fastest to respond.
  - **Synthwave** -- neon magenta and cyan over a violet horizon, with
    scanlines and a glow on anything selected.
  - **Nord**, **Dracula**, **Gruvbox** -- three well-known palettes; Gruvbox is
    warm and monospaced where the others are cool.
- **Dark is still the default and is unchanged.** Text sizes and spacing stay
  identical in every theme -- a theme restyles the app, it does not rearrange
  it, so nothing shifts under you when you switch.

## [0.5.76] - 2026-07-16

### Added

- You can now pick a theme. Settings -> General -> Appearance offers **Dark**
  (unchanged, still the default), **Light**, and **High contrast**, and the app
  repaints as soon as you choose -- no restart. Your choice is remembered.
- Only colors change. Text sizes, spacing and the video player's timings stay
  exactly as they were, because those are layout, not palette -- a theme should
  recolor the app, not rearrange it. A few things stay dark in every theme on
  purpose: the dimmed backdrop behind an opened capture, and the play badge and
  buttons drawn on top of video thumbnails. Those sit over your captures rather
  than over the app, and they have to stay readable whatever the frame shows.

## [0.5.75] - 2026-07-16

### Changed

- Developer documentation now matches the last two changes, and records a
  conclusion rather than a plan: the long-standing idea of merging the two video
  players into one shared piece was examined and dropped. They looked like
  duplicates but behave oppositely -- the full-screen viewer hides its still
  image behind the video, the overlay keeps showing the clip's thumbnail until
  you focus it -- so merging them would have meant one component full of
  switches standing in for about thirty genuinely shared lines. Writing down why
  it is not worth doing saves the next person from rediscovering it.

## [0.5.74] - 2026-07-16

### Changed

- The logic behind selecting several captures at once -- which ones are ticked,
  where a shift-extend measures from, how "select all" flips to "deselect all"
  -- now lives in a file of its own instead of being spread through the main
  window. It was about eighty lines tangled up with window code it had nothing
  to do with, which made the one genuinely fiddly part (dragging a shift-range
  back over itself to undo it) hard to follow. Nothing about selecting works
  differently; the sounds, the controller buttons and the delete confirmation
  are all untouched.

## [0.5.73] - 2026-07-16

### Changed

- Working out which game you are playing no longer re-reads the disk every 1.5
  seconds. The app checks the foreground window on a timer, and each check was
  scanning your Steam library folder and reading the game executable's embedded
  metadata again from scratch -- work that returns the same answer every time,
  because it describes a file that is not changing. That answer is now
  remembered for as long as the same game stays in front, and looked up again
  the moment a different one takes over. The game's own window caption is still
  read fresh each time, so a title that appears late still shows up.

## [0.5.72] - 2026-07-16

### Changed

- The technical audit document now describes the code as it is today. It was
  written a week ago, before this round of cleanup, so it still listed problems
  that have since been fixed and pointed at code that has since moved. It now
  records what this round changed, and -- more usefully -- keeps an honest list
  of the work that is deliberately not done: five finished changes still waiting
  on a hands-on check, and five larger rewrites left alone because the recording
  and controller code they touch was tuned against real games and real hardware
  and cannot be re-checked without them.

## [0.5.71] - 2026-07-16

### Added

- The project has its first automated tests: 51 checks covering game-name
  handling, the settings file (defaults, overrides, resetting, and preserving
  keys written by a future version), and the rule that picks the better of two
  duplicate game names. They are opt-in (`-DGAMEHQ_BUILD_TESTS=ON`), cover pure
  logic only -- no recording, no controller, no database -- and run in under a
  second. The command is documented in `docs/dev-setup.md`.

### Changed

- Working out which game a capture belongs to from its folder now lives next to
  the rest of the game-name logic, and uses the same "Unknown Game" fallback as
  everything else instead of its own copy of the text.

## [0.5.70] - 2026-07-16

### Changed

- The two identical read-only folder boxes on the Capture settings page (the
  screenshots root and the clips root) are now one shared `SettingsPathField`
  component rather than the same twenty lines written twice. The page looks and
  behaves exactly as before.

## [0.5.69] - 2026-07-16

### Changed

- Picking a sidebar category now runs through one shared rule instead of three
  separate copies of it (desktop mouse click, desktop controller navigation, and
  the overlay). The `Game` and `Game Favourites` rows are the only special
  cases, and they are now described in exactly one place, so the three sidebars
  can no longer disagree about what a row means. Every row filters exactly as
  before.

## [0.5.68] - 2026-07-16

### Changed

- The play marker shown on video thumbnails is now one shared `VideoBadge`
  component instead of three separate copies of the same drawing in the capture
  tile, the overlay preview and the toast. The three only ever differed in size,
  which each caller still sets, so the badge looks exactly as before -- but it
  can no longer end up looking different in one place after an edit.

## [0.5.67] - 2026-07-16

### Changed

- The visual values that were still written directly into individual QML
  components -- the video play badge, the thumbnail icon buttons, the player's
  play/pause pulse, the tinted Delete/Done buttons, row hover, two font sizes
  and the all-caps letter spacing -- now come from named `Theme` tokens like
  every other value. Each literal was mapped to a token of exactly the same
  value, so nothing changes visually; the point is that these values can now be
  changed in one place.

## [0.5.66] - 2026-07-16

### Changed

- Controller and keyboard actions are now looked up in one dispatch table
  instead of a 28-branch chain of string comparisons, keeping the list of
  actions next to the `ActionCatalog` it mirrors. An action that is ever left
  out of the table now logs a warning instead of failing silently. No shortcut,
  binding, or hold-to-repeat behavior changes.

## [0.5.65] - 2026-07-16

### Changed

- The compatibility shims that adopt data from the app's former names (SavePlay,
  PlayHQ) now live in one place, a new `LegacyMigration` helper, instead of being
  spread between `Paths` and an inline block in `App::init`. The database
  hand-over is reached through the new `Paths::databasePath()`. Migration
  behavior is unchanged: legacy folders and a legacy database are still adopted
  on first start, and capture media is still never moved.

## [0.5.64] - 2026-07-16

### Changed

- The special-cased settings keys (the startup toggle and the screenshot/clip
  root pickers) moved out of `AppController::setConfig`/`resetConfig` into a
  new internal `SettingsRouter` helper, matching the existing
  `CaptureLibraryService` delegation pattern. Those two methods are now a plain
  read of the router's outcome. Behavior and the QML API are unchanged.

## [0.5.63] - 2026-07-16

### Changed

- Every `config.json` key is now spelled once, in the new
  `src/config/ConfigKeys.h` registry, and referenced from C++ through a
  constant instead of a repeated string literal. A mistyped key is now a
  compile error rather than a silent fall back to the default value. QML keeps
  using string literals, since it cannot see the constants.
- The settings "Restore defaults" taxonomy (which config groups each page
  owns) moved out of `AppController` into `src/config/SettingsCategories.h`,
  next to the key registry.

## [0.5.62] - 2026-07-16

### Changed

- Library scans now read the capture index once instead of querying the
  database twice per file on disk. A scan of a library with N media files
  previously issued up to 2N queries during the directory walk; it now issues
  a single up-front `SELECT` and diffs in memory. Registered captures, their
  game assignment, and thumbnail backfill are unchanged.

## [0.5.61] - 2026-07-16

### Changed

- The two heavy one-time startup repairs — the O(n²) duplicate display-name
  collapse and the full `gamehq.log` metadata rescan — are now gated behind a
  completion sentinel (`internal.repairs_v1_done` in the `settings` table).
  They run once after an upgrade and are skipped on every subsequent launch,
  cutting startup cost. The sentinel is written inside the same repair
  transaction, so it only sticks if the repairs commit successfully.

## [0.5.60] - 2026-07-16

### Fixed

- The database startup repair pass (legacy brand paths, duplicate captures,
  moved-path renormalization, game metadata) now runs inside a single
  transaction, so an interrupted launch can no longer leave the library
  half-repaired.

## [0.5.59] - 2026-07-16

### Fixed

- Replay audio timestamps now use an overflow-safe clock conversion shared
  with the video pipeline (the previous audio-only formula could overflow
  after about a day of system uptime and skew A/V sync).
- The replay capture bring-up no longer leaks a d3d11.dll module reference on
  every buffer arm.
- Oversized system-audio packets that cannot fit the capture buffer are now
  logged instead of being dropped silently.

### Changed

- Capture subsystem helpers (HRESULT logging, clock conversion, stale-cache
  threshold, collision-safe file naming) consolidated into one shared header.

## [0.5.58] - 2026-07-16

### Removed

- Unused input-layer code: the superseded `TapHoldDetector` class and the
  legacy integer controller signals that had no remaining consumers. No
  behavior change; controller dispatch is unaffected.

## [0.5.57] - 2026-07-16

### Documentation

- Completed a full-codebase technical audit and recorded the Refactor Wave 2
  execution plan in the internal planning docs (dead-code removal, startup-cost
  fixes, config key centralization, theme-token cleanup, first automated test
  target). No code changes.

## [0.5.56] - 2026-07-15

### Changed

- Settings → General: the "Start minimized" toggle is now labeled
  "Launch minimized" with a clearer description.

## [0.5.55] - 2026-07-13

First public release.

### Added

- Native Windows capture gallery with game grouping, filters, favorites,
  thumbnails, bulk selection, lightbox image viewing, and video playback.
- Screenshot capture in PNG or JPEG with configurable quality, folders,
  notifications, and sounds.
- Rolling H.264 replay buffer with configurable duration, quality, frame rate,
  resolution, system audio, and MP4 export.
- Controller-driven in-game overlay with focus restoration and capture actions.
- Configurable controller, keyboard, and extra mouse-button bindings with two
  slots per action, gesture support, conflict handling, profiles, and resets.
- Separate managed screenshot and clip locations plus read-only watched folders.
- Portable mode, tray behavior, startup options, diagnostics, and scoped restore
  controls.

### Security and privacy

- No account, telemetry, cloud dependency, game-process injection, or background
  Windows service.
- User data remains local in portable storage or the current Windows profile.

[0.5.55]: https://github.com/underfusion/GameHQ/releases/tag/v0.5.55
