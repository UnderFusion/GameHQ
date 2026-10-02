<div align="center">

<img src="docs/assets/gamehq-wordmark.svg" width="190" alt="GameHQ">

<h2>Capture moments after they happen</h2>

<p>GameHQ brings console-style capture controls to Windows. Tap Share for an
instant screenshot, hold it after something memorable happens to save the
previous configurable minutes as a video clip, and press PS or Guide to browse
everything in a controller-friendly overlay. The entire app - gallery,
overlay, and Settings - can be driven from the controller alone.</p>

<p><img src="docs/assets/readme-separator.svg" width="1000" height="1" alt=""></p>

<!-- public-downloads:start -->
<p align="center">
  <a href="https://github.com/underfusion/GameHQ/releases/latest"><img src="docs/assets/download-windows.svg" width="230" alt="Download GameHQ for Windows"></a>
  &nbsp;
  <a href="https://github.com/underfusion/GameHQ/releases/latest"><img src="docs/assets/download-portable.svg" width="230" alt="Download GameHQ Portable ZIP"></a>
</p>
<p align="center"><sub>Windows 10+ &middot; Latest stable &middot; GPL-3.0 &middot; No telemetry</sub></p>
<p align="center"><sub>Using Playnite? <a href="https://github.com/underfusion/GameHQ/releases/download/playnite-v0.4.12/GameHQ_Playnite_Integration_0_4_12.pext">Get the GameHQ Integration &rarr;</a></sub></p>
<!-- public-downloads:end -->

<p><strong>Available in 16 languages · Controller-first · No telemetry · Open source</strong></p>

<p>⭐ Enjoying GameHQ? Star the repository - it helps more players discover the project.</p>

<p>❤️ <a href="https://ko-fi.com/underfusion"><strong>Coffee fund for late-night GameHQ coding ☕</strong></a></p>

</div>

![GameHQ gallery](docs/assets/gamehq-gallery.png)

## Highlights

- **Save recent gameplay after it happens** - the rolling buffer turns the
  previous configurable minutes into a normal MP4 with system audio.
- **Instant screenshots and frame capture** - save PNG or JPEG images, including
  the exact displayed frame from a recorded clip. JPEG at 90% quality is the
  default for smaller files; PNG remains available.
- **Share captures from anywhere** - share a screenshot or clip from the
  gallery, full-screen viewer, or overlay. Open Telegram Desktop's chat picker,
  copy a capture and open Discord to paste it into a chat, upload directly to a
  Discord channel through a configured webhook, or copy to the clipboard.
  Telegram and Discord Desktop let you finish sending in the app; confirmed
  webhook uploads can report *Sent*. No account sign-in through GameHQ.
- **In-game overlay over borderless games** - browse, play, favorite, reveal,
  and delete captures without leaving your game. The overlay stays above a
  visible borderless game, and on supported GameInput paths overlay navigation
  doesn't also control the game underneath.
- **Shape the overlay to fit your game** - adjust margins, spacing, thumbnail
  size, interface size, and control hints live. Expand, collapse, resize, or
  use Auto mode for the sidebar; your layout is remembered.
- **Full-screen viewing over your game** - open screenshots full screen,
  browse screenshots and clips with L1/R1, and mark favorites from the viewer.
- **Controller-first everywhere** - the whole interface is pad-navigable:
  gallery, overlay, dialogs, and Settings, with follow-scroll, right-stick
  scrolling, and visible scrollbars. Keyboard and mouse work everywhere too.
- **Modern controller support** - app-local GameInput, true Share/Guide, extra
  buttons, and safe legacy fallback ([getting started](docs/getting-started.md),
  [compatibility guide](docs/controller-compatibility.md)).
- **Dependable Sony controller input** - DualSense support over USB and
  Bluetooth, with reconnecting and switching between them without a restart.
  Supported DualSense and DualShock 4 controllers hidden by HidHide remain
  usable once GameHQ is allowed through HidHide. *Fix automatically* only adds
  GameHQ to its allowed applications; it does not change which pads are hidden.
- **Mapping presets** - save controller layouts as named presets, then assign
  them per controller and per game; the preset for the game you're running is
  picked automatically.
- **Flexible binding editor** - two slots per action with taps, holds, double
  and triple taps, button combinations, and conflict-aware validation.
- **Summon GameHQ with the pad** - hold PS for two seconds to bring the window
  over your game; hold again to send it back and return focus to the game.
- **One organized library** - GameHQ captures alongside watched Steam, Game Bar,
  NVIDIA, and OBS folders. The gallery follows files copied, deleted, or
  restored in Explorer live, and pinned games stay at the top of the desktop
  and overlay sidebars.
- **Speaks your language** - 16 interface languages across the app, overlay,
  Settings, tray, installer, and release notes. GameHQ follows your Windows
  language on first start, and switching language applies immediately without a
  restart.
- **Picks up where you left off** - GameHQ reopens on your last page, Settings
  category, and gallery filter, and the overlay returns to the capture you had
  selected.
- **Private and portable** - no account, telemetry, background service, or
  game-process injection.
- Configurable replay duration, quality, frame rate, and resolution.
- Thirteen themes, live theme switching, textured backdrops, and adjustable
  overlay dimming.
- Thumbnail zoom and controller-friendly bulk selection in the capture library.
- Clip saves never overwrite earlier clips, and a replay can be saved right
  after recording starts, using the footage captured so far.
- Immediate gallery refresh after new captures, with instant saved or failed
  feedback.
- Separate switches for capture-request, Steam Input conflict, and
  controller-hidden notices, plus early dismissal with the mouse.

See [what's new in 0.7.9](https://github.com/underfusion/GameHQ/releases/tag/v0.7.9)
for the full release notes and links to all 16 reviewed translations.

## Languages

GameHQ is localized across the desktop interface, the in-game overlay, Settings,
the tray menu, the installer, and the release notes. The language is picked from
Windows on first start and can be changed at any time in **Settings > General**;
the change applies immediately, without restarting.

English · 简体中文 · 繁體中文 · Русский · Español (España) · Español (Latinoamérica) ·
Português (Brasil) · Deutsch · 日本語 · Français · Polski · 한국어 · Türkçe · ไทย ·
Українська · Italiano

## Quick controls

| Input | Action |
|---|---|
| Share / Capture - tap | Take a screenshot |
| Share / Capture - hold | Save recent gameplay |
| PS / Guide | Open or close the in-game overlay |
| PS / Guide - hold | Show or hide the GameHQ window |

<details>
<summary><strong>Full controls</strong></summary>

| Input | Action |
|---|---|
| Share / Capture - tap | Take a screenshot |
| Share / Capture - hold | Save recent gameplay |
| PS / Guide | Open or close the in-game overlay |
| PS / Guide - hold | Show or hide the GameHQ window |
| D-pad / left stick | Navigate |
| Cross / south button | Confirm or open |
| Circle / east button | Back |
| L2 / R2 | Decrease or increase gallery thumbnail size |
| Cross / south button - hold | Enter bulk selection in the desktop gallery |
| Share / `S` while playing a clip | Save the displayed video frame as a screenshot |
| `Ctrl+Shift+S` | Take a screenshot |
| `Ctrl+Shift+E` | Save a replay clip |
| `Ctrl+Shift+G` | Toggle the overlay |

Bindings, mapping presets, and gesture timing can be changed from
**Settings > Input**.

</details>

## Installation

**Setup** is recommended for normal use. **Portable ZIP** stores its profile
beside the application and can run from any folder.

Using Playnite? Install the
[GameHQ Integration](https://github.com/underfusion/GameHQ/releases/download/playnite-v0.4.12/GameHQ_Playnite_Integration_0_4_12.pext)
by opening the downloaded `.pext` file with Playnite.

> **Unsigned Windows binaries:** Windows may show Unknown Publisher or
> SmartScreen warnings. Download GameHQ only from this repository, verify the
> published hashes, and do not disable Windows Security.

<details>
<summary><strong>Portable, installed, and uninstall behavior</strong></summary>

Portable keeps its profile and default captures beside the extracted app.
Installed mode uses the current user's AppData and `Videos\GameHQ`. On a fresh,
empty installed profile, **Settings > Advanced > Portable profile** can import
an existing portable configuration and library. The importer leaves the
portable source and all capture media in place; populated installed libraries
are not merged.

Uninstall removes only installed program files, shortcuts, and registry values
still owned by that installation. It does not remove AppData, captures, watched
folders, portable copies, or media.

</details>

<details>
<summary><strong>Download verification and code signing</strong></summary>

Verify the official source and published release hash before running an
artifact. A specific Defender malware or PUA detection is different from an
Unknown publisher warning: do not bypass it or disable Windows Security. See
[Download verification](docs/download-verification.md) and
[Security & privacy](docs/security-and-privacy.md).

Current releases include a production Ed25519 release manifest and signature,
SHA-256 checksums, and corresponding source. The manifest authenticates update
artifacts and their hashes; Windows binaries are currently not
Authenticode-signed. See the [Code signing policy](docs/code-signing-policy.md).

</details>

<details>
<summary><strong>Building from source</strong></summary>

GameHQ uses C++20, Qt 6.8.3/QML, CMake, Ninja, SQLite, Windows Graphics Capture,
Media Foundation, WASAPI, and Windows input APIs.

See [Development Setup](docs/dev-setup.md) for the toolchain and
[Packaging & Distribution](docs/packaging.md) for the portable package layout.

```powershell
tools/cmake/bin/cmake.exe -S . -B out -G Ninja
tools/cmake/bin/cmake.exe --build out
powershell -ExecutionPolicy Bypass -File packaging/make-dist.ps1
```

</details>

## Requirements

- Windows 10 version 1903 or newer, 64-bit.
- A GPU and driver supporting Windows Graphics Capture and H.264 encoding.
- Enough free disk space for the configured rolling buffer and saved captures.

## Support the project

GameHQ is developed independently and stays open source, with no account, no
telemetry, and no paid tier. If you would like to support continued development,
bug fixing, and new features, you can support the creator at
[ko-fi.com/underfusion](https://ko-fi.com/underfusion).

## Documentation

Architecture and subsystem documentation lives in [docs/](docs/README.md).
Changes are summarized in the [changelog](CHANGELOG.md).
Vulnerabilities can be reported privately through the repository's enabled
[security advisory form](https://github.com/underfusion/GameHQ/security/advisories/new).

## License

GameHQ core source code is available under the [GNU GPL version 3](LICENSE).
The separately packaged Playnite integration and public integration protocol
remain under MIT. See
[Licensing](docs/licensing.md) for the project boundary and
[Third-Party Notices](THIRD_PARTY_NOTICES.md) for redistributed Qt, FFmpeg,
compiler-runtime, and other components under their respective licenses.

GameHQ is an independent project and is not affiliated with or endorsed by
Sony Interactive Entertainment, Microsoft, Valve, NVIDIA, or OBS Project.
