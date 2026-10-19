# GameHQ 0.8.0 (2026-10-19)

> This document is the original **English (en-US)** release note. No reviewed Chinese (Simplified) translation exists for this version, so the complete English text is published unchanged.

## Clip Audio Repair

- Desktop audio recording starts by default. An old internal workaround left System audio off by default, so clips could contain video only. The setting remains available in Settings > Replay, and an explicitly saved disabled setting remains respected.
- Windows playback formats are converted to a standard stereo format supported by the recording encoder instead of losing the audio track.
- If System audio is on but Windows audio cannot be captured, GameHQ retries and then reports why no clip was saved, instead of saving a clip without sound. Turning System audio off still records video-only clips on purpose.
- Quiet moments are recorded as silence, so the audio track stays complete and sound after a quiet stretch stays in sync with the picture.
- Desktop audio includes sound playing through the default Windows output device, which can include other apps. It does not record the microphone. Audio missing from an older clip cannot be recovered.

## Changes and Improvements

- Optional beta updates: turn on Include beta versions in Settings > General or About to receive beta releases. It is off by default. Each beta has its own update identity so the updater can offer the next one.
- Controller shortcuts work more reliably in games launched outside Steam, including games started from Playnite, when Windows stops delivering a connected DualSense through its normal input path. GameHQ temporarily reads the controller directly and returns to normal delivery when it recovers.
- Windows Xbox mode with Playnite can use the verified game session for screenshots and clips when the Xbox shell holds the foreground. Use GameHQ Playnite Integration 0.4.13; no new plugin build is required.

## Other Bug Fixes

- Settings mouse scrolling no longer jumps back to the focused control when the window or a popup returns focus.
- Controller navigation repeats stop when GameInput reports the release or the controller is unplugged while a direction is held.
- Playnite games remain recognized when Playnite cannot identify the game process.
- GameHQ notifications stay visible to you but are excluded from screenshots and whole-screen recordings. Notifications from other apps may still appear.

## Known Limitations

- System audio records the default Windows output device. A game assigned to a different output in Windows sound settings is not included in clips.
- A game that takes exclusive control of a controller may still prevent background shortcuts.
- Windows binaries are not Authenticode-signed; Windows SmartScreen may show an Unknown Publisher warning.
- New release-note text uses complete English fallback where a reviewed translation is not yet available. Existing interface translations are unchanged.
