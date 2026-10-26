# Changelog

All notable changes to the GameHQ Integration Playnite plugin are documented
here. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the plugin uses [Semantic Versioning](https://semver.org/). This plugin
is versioned and released independently of the main GameHQ application (see
`../../VERSION` for that) — releases are tagged `playnite-vX.Y.Z` in this
same repository.

## [0.4.13] - 2026-10-08

Published first as a GitHub pre-release for Windows Xbox mode testing; the
public installer manifest keeps offering 0.4.12 until it is confirmed.

### Changed

- Windows Xbox mode (full screen experience): Windows holds back startup apps
  until you first switch to the desktop, so GameHQ could be missing after
  booting straight into Playnite. The plugin now checks again 15, 45 and 90
  seconds after Playnite starts and when a game starts, starts GameHQ if it is
  not running, and never launches a second copy when GameHQ is already running.
- The Playnite log says whether GameHQ is not running or running but not
  connected.

## [0.4.12] - 2026-07-22

### Added

- Connects Playnite to GameHQ over a private local connection.
- Starts GameHQ automatically when needed.
- Synchronizes game start, stop and reconnect state.
- Adds connection, version and path diagnostics.

## [0.4.11] - 2026-07-21

### Changed

- Expanded GameHQ discovery for Setup installations. After the configured
  path, the plugin now checks the installer-owned `InstallLocation`, the
  current-user Windows App Path, the existing autostart entry, and finally the
  standard per-user install folder. Candidate precedence and invalid fallback
  behavior are covered by linked-source tests.

## [0.4.10] - 2026-07-21

### Added

- The settings page now opens with a branded header: the GameHQ logo next to
  "GameHQ Integration / Playnite companion plugin", separated from the rest by a
  rule. `icon.png` is embedded in the assembly as a WPF `Resource` and loaded
  through a pack URI, so it renders regardless of where Playnite unpacks the
  extension.

## [0.4.9] - 2026-07-21

### Fixed

- The settings page was clipped at the bottom in Playnite's add-on window (the
  *About this integration* paragraph was cut off). The whole page is now wrapped
  in a `ScrollViewer` with an automatic vertical scrollbar.

## [0.4.8] - 2026-07-21

### Changed

- Reworked the settings page for readability: sections are separated by
  horizontal rules, headings are larger, the resolved GameHQ path now sits in a
  rounded read-only "bubble" text box that can be selected and copied, and the
  connection status fields plus the *Test connection* button are grouped into
  one card.
- Removed the descriptive captions under the two Startup checkboxes; the
  checkbox labels themselves now carry the meaning.
- Trimmed the Connection and Support blurbs, and hints are now smaller and
  dimmer (`#B7BDC6`) than the labels they describe, so the page no longer reads
  as one undifferentiated wall of same-sized text.

## [0.4.7] - 2026-07-21

### Fixed

- The 0.4.6 fix didn't hold: `BasedOn="{StaticResource {x:Type ...}}"` relied on
  Playnite exposing implicit type styles to plugin-hosted controls, which it
  does not, so text stayed on WPF's default near-black foreground. Replaced
  with an explicit `Foreground="White"` on the `Hint` style and
  `TextElement.Foreground="White"` on the root panel, so every label inherits
  a visible color regardless of the host theme's resource wiring.

## [0.4.6] - 2026-07-21

### Fixed

- Hint text and the "Download GameHQ" button were invisible on dark Playnite
  themes: the local `Hint` and `MissingOnly` styles had no `BasedOn`, so they
  replaced Playnite's implicit theme style instead of extending it, leaving
  those elements with WPF's default black foreground. Both styles now chain
  `BasedOn="{StaticResource {x:Type ...}}"` onto the correct target type.

## [0.4.5] - 2026-07-21

### Added

- An "Open Playnite log" button under Support, with a line explaining that the
  integration writes into Playnite's main log rather than keeping one of its
  own. It opens `playnite.log` in Playnite's configuration folder, or the
  folder itself if the file does not exist yet.

## [0.4.4] - 2026-07-21

### Added

- The settings page now explains itself. Two lines under "Connection" say what
  the plugin does and that GameHQ is a separate application the plugin never
  downloads or installs, both startup checkboxes carry a one-line description,
  and an "About this integration" section states what is shared, that no
  gameplay is recorded or uploaded, and that GameHQ works without Playnite.
- A "Download GameHQ" button, shown only while no installation is found, opens
  the releases page.
- An "Open folder" button reveals the resolved installation.

### Fixed

- The connection path was an empty, unexplained text box. It was bound to the
  manual override, so a successful auto-detection left it blank and the page
  looked broken. The page now shows the resolved `GameHQ.exe`, whether it was
  detected automatically or selected manually, and "GameHQ was not found on
  this PC" when there is nothing to show.
- The "Select..." button now reads "Locate GameHQ.exe..." when no install is
  found and "Change location..." when one is.
- The connection-test result outlived the connection it described: after
  restarting GameHQ on a newer build the page showed the live version in one
  row and an older version in the stale test line. The result is now cleared
  whenever the connection state moves, and no longer repeats the version.

## [0.4.3] - 2026-07-21

### Fixed

- "Test connection" still gave no visible feedback: a local pipe reconnect
  completes in a few milliseconds, so the Status field's Connecting flicker
  was too fast to actually see. The button now shows "Testing..." right
  away and, once the reconnect settles, a persistent "Test succeeded" /
  "Test failed: <reason>" result line under the button.
- The settings page never showed the plugin's own version, only GameHQ's —
  it now has a "Plugin version:" row so it's obvious whether Playnite
  picked up a newly installed build.

## [0.4.2] - 2026-07-21

### Fixed

- The plugin never appeared under Playnite's Add-ons → Extensions settings
  tree (though it showed as installed/enabled) because the constructor
  didn't set `Properties.HasSettings = true`, the flag Playnite's SDK
  requires to list a settings entry regardless of `GetSettings`/
  `GetSettingsView` being overridden.
- `extension.yaml` never declared an `Icon`, so Playnite showed the plugin
  with a generic default icon instead of the packaged `icon.png`.
- The handshake always failed with "Timeouts are not supported on this
  stream" because `PipeStream.ReadTimeout` is unconditionally unsupported
  in .NET — Test Connection could never succeed even with GameHQ running.
  The handshake deadline is now enforced by racing the read against a
  timer instead of setting `ReadTimeout`.
- "Test connection" did nothing when already connected — it only woke the
  loop's backoff wait, which isn't reached while a connection is active,
  so clicking it gave no visible feedback. It now also drops the active
  pipe to force an immediate real reconnect, which is visible in the
  Status field as it cycles through Connecting.
- Even after the reconnect fix above, Test connection still appeared to
  do nothing: `StateChanged` fires from the background reconnect thread,
  and WPF doesn't reliably apply property-change notifications raised
  off the UI thread. The settings page's `StateChanged` handler now
  marshals onto the UI dispatcher before refreshing the bound fields.

## [0.4.0] - 2026-07-20

### Added

- Settings page (Connection / Startup / Support sections): configurable
  GameHQ path with validation, live connection status, detected GameHQ
  version and protocol, a "Test connection" action, the two startup
  preferences, "Open GameHQ", "Website" and "Copy diagnostic summary".
- Main-menu command "Open GameHQ" (focuses a running instance or launches
  it) — intentionally the only main-menu command; no screenshot/replay
  commands, since opening a menu defocuses the game.
- `IntegrationClient` now tracks the remote app version, negotiated
  protocol and last error for the settings page, and exposes
  `TriggerReconnect()` for the "Test connection" action.

### Known gaps

- "Open plugin log" from the Support section is not implemented — Playnite
  doesn't expose a log path via the SDK and guessing at one risked being
  wrong; deferred to a follow-up once a real Playnite install is available
  to confirm the right target.

## [0.3.0] - 2026-07-20

### Added

- `GameLifecycleForwarder`: tracks active Playnite game sessions and sends
  `playnite.game.starting/started/stopped/startup_cancelled` and
  `playnite.application.started/stopping` over the pipe client.
- `playnite.state.sync` is sent with the current session snapshot after
  every successful connection, including reconnects, so a missed
  disconnect never leaves a phantom "game running" state on the app side.
- Playnite exiting never closes GameHQ — it may be tray-resident or used
  standalone.

## [0.2.0] - 2026-07-20

### Added

- `GameHQ.Local.v1` pipe client (framing, handshake, bounded outgoing queue,
  reconnect with exponential backoff, `app.maintenance` awareness) under
  `Protocol/`.
- `GameHQLocator` (configured path, then the current-user Run registry
  entry) and `GameHQProcessLauncher` (launches the root `GameHQ.exe`).
- The plugin now starts the client on load and makes one best-effort launch
  attempt on `OnApplicationStarted` if GameHQ isn't already reachable.
  Game lifecycle forwarding still lands in a follow-up release (p5-3).

## [0.1.0] - 2026-07-20

### Added

- Initial subproject scaffold: buildable `GenericPlugin` skeleton targeting
  the Playnite 10 SDK, its own `.csproj`, packaging scripts and installer
  manifest. No user-facing behavior yet — connecting to GameHQ, forwarding
  game lifecycle events and the settings page land in follow-up releases.
