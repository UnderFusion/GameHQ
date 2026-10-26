# GameHQ 0.8.0 (2026-10-26)

## Special Thanks

- A big thank you to [@galcn10](https://github.com/galcn10), creator of [Console Overlay for Playnite](https://github.com/galcn10/ConsoleOverlay), for testing GameHQ, sharing valuable feedback, and building a great Playnite integration. Your support helped make GameHQ 0.8.0 better!

## Highlights

- Clips can record your game's own sound. With the new Game audio source, GameHQ records the game on whatever device it plays through (headphones, Bluetooth, HDMI or speakers) and leaves out other apps such as Discord or Spotify.
- Audio recording problems are now detected and reported. If the selected audio source cannot be recorded, the replay buffer stops and tells you why, and which source to choose instead, rather than quietly saving clips without sound. GameHQ never switches the audio source on its own.
- Sound and picture stay in sync. Clips saved after a loading screen, a pause or a long stall keep correct timing, and the replay buffer restarts on its own when a game changes resolution.
- HDR screenshots no longer get stuck. A screenshot of a paused game or a static menu in HDR is saved right away. If no frame arrives at all, the attempt fails cleanly after a few seconds and the next press works.
- Safer deleting with a controller. Cross in a delete confirmation now presses the highlighted button and Circle cancels. Previously, Cross could delete the file even when Cancel was highlighted.

## What's New

- Settings › Replay › Audio source offers Game audio, Desktop audio and Off. Game audio is the default on Windows 11. Desktop audio records everything on the default Windows output, including other apps, and is the default on older Windows versions.
- Game audio uses Windows' own per-app audio capture and never touches the game process. If you turned system audio off on purpose in an earlier version, it stays off.
- Optional beta updates: a new Include beta versions switch in Settings › General and in About lets the updater offer beta releases. It is off by default, so stable installations only receive stable updates.
- When GameHQ crashes, it tries to save a small crash dump in its logs folder to help diagnose the problem. Nothing is uploaded. Some crashes, such as ones Windows ends immediately, can still close GameHQ without a dump.
- The log records more detail about saved screenshots and start-up failures, so a single log file is more often enough to understand a problem report.

## Fixes and Improvements

- Clips no longer come out silent because of an old internal setting that kept desktop audio off by default.
- Audio in any Windows playback format is converted correctly instead of the audio track being lost.
- Quiet moments are recorded as silence, so sound after a quiet stretch stays aligned with the picture.
- Game audio recording restarts more reliably when a game restarts or you switch games.
- Parts of the replay buffer that failed to save or cannot be read back are left out, so they cannot break a saved clip.
- GameHQ's own notifications, such as the notice that a screenshot was saved, no longer appear in your screenshots or in whole-screen recordings and streams.
- A screenshot replaced by another image with the same file name gets a fresh thumbnail on the next library scan.
- Capture folders are scanned in the background, so GameHQ stays responsive with large collections.
- Browsers, editors and other windows that are not games no longer appear as games in your library.
- During a bulk delete, the selection stays fixed while the confirmation is open, so background controller actions cannot change which files are deleted.
- The overlay's delete dialog takes the controller's input while it is open and closes when the overlay hides.
- The overlay skips windows that have stopped responding instead of waiting on them.
- Settings no longer jumps back to the top when you scroll with the mouse after switching back to GameHQ.
- Choosing Built-in defaults or removing a preset in the controller mapping editor no longer freezes the preset selection.

## Controllers and Playnite

- Controller shortcuts keep working in games launched outside Steam, such as a local game started from Playnite. When Windows stops delivering a connected DualSense to GameHQ while the game has focus, GameHQ reads the controller directly, read-only, and returns to normal input once Windows delivers it again.
- Shortcuts and held-button repeats always end on the release of the same press, including releases reported through GameInput and controllers unplugged while a button is held.
- Controllers that were disconnected for good are cleaned up, the HidHide check no longer slows down the interface, and GameInput devices are only checked again when something changes.
- Playnite games stay recognized even when Playnite cannot identify the game's process.
- Windows Xbox full screen experience: when the Xbox shell is in front, GameHQ uses the game started from Playnite for screenshots and clips. GameHQ Playnite Integration 0.4.13, available separately as a pre-release, also starts GameHQ when Xbox mode delays it at sign-in.

## Stability and Reliability

- Cleanup of unfinished recordings only removes temporary files that GameHQ itself created.
- Game capture folders always stay inside your capture folder.
- Start-up no longer fails when the library contains duplicate folder paths; the entries are merged instead.
- If GameHQ cannot start, it explains why in your language and keeps the reason in the log.
- Importing a portable profile into an installed GameHQ now works when the installed copy has already been started.
- Hardened security: safer DLL loading, stricter checks on local integration connections and safer handling of update files.
- The updater copes better with a slow first start and keeps its records intact if it is interrupted.
- Telegram sharing shuts down more safely.

## Installation and Updating

- Updating from 0.7.9: the built-in updater offers 0.8.0, or you can run the installer over your current installation. Your settings, library and captures are kept.
- Installer: installs for the current user and replaces an installed 0.7.9 or an earlier beta.
- Portable: extract the ZIP to a new folder and run GameHQ.exe.
- If you tested 0.8.0 Beta 2, 3 or 4, install 0.8.0 with the installer or a fresh portable ZIP, because those betas were not offered through the updater.

## Known Limitations

- Game audio needs Windows 11. On older Windows versions GameHQ uses Desktop audio, which also records other apps playing on the default output.
- A game that takes exclusive control of a controller may still prevent background shortcuts.
- Support for the Windows Xbox full screen experience has not yet been confirmed by testers on a PC running it.
- Windows binaries are not Authenticode-signed; Windows SmartScreen may show an Unknown Publisher warning.

<!-- gamehq:locale-index -->

## Release notes in other languages

These release notes are published in English by default. [Release notes in 16 languages](https://github.com/underfusion/GameHQ/tree/v0.8.0/assets/release-notes/publication/0.8.0) are kept in the repository; they are not attached to this release as separate downloads.

| Language | Locale | Content |
| --- | --- | --- |
| English | `en-US` | English source |
| 简体中文 | `zh-Hans` | Reviewed translation |
| Русский | `ru-RU` | Reviewed translation |
| Español | `es-ES` | Reviewed translation |
| Português (Brasil) | `pt-BR` | Reviewed translation |
| Deutsch | `de-DE` | Reviewed translation |
| 日本語 | `ja-JP` | Reviewed translation |
| Français | `fr-FR` | Reviewed translation |
| Polski | `pl-PL` | Reviewed translation |
| 한국어 | `ko-KR` | Reviewed translation |
| 繁體中文 | `zh-Hant` | Reviewed translation |
| Türkçe | `tr-TR` | Reviewed translation |
| ไทย | `th-TH` | Reviewed translation |
| Español (Latinoamérica) | `es-419` | Reviewed translation |
| Українська | `uk-UA` | Reviewed translation |
| Italiano | `it-IT` | Reviewed translation |
