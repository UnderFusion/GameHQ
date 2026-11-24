# GameHQ 0.8.2 (2026-11-24)

> This document is the original **English (en-US)** release note. No reviewed Korean translation exists for this version, so the complete English text is published unchanged.

## Beta 2

- GameHQ 0.8.2 Beta 2 is a pre-release for testing fixes reported by the community. It also includes everything from Beta 1. GameHQ 0.8.1 remains Latest Stable.

## Community report fixes

- Exit from the tray menu closes GameHQ completely, even while a recording is being prepared or a frame is still being processed (#34).
- Keyboard shortcuts can be changed, removed and saved again, and they survive a restart. If another app already uses the new key, your previous shortcut keeps working (#36).
- Games launched from Playnite are recognized even when Playnite reports no process ID. This needs GameHQ Integration 0.4.14 for Playnite (#37).
- The list of allowed games decides which games can be captured, and removing a game stops its capture immediately (#37).
- A double press of View/Back runs only the action assigned to it, so it no longer also triggers the single-press action (#33).
- Extra controller buttons that Windows reports separately can be assigned in the Input editor (#33).
- Recording continues while a game is paused by an overlay such as Playnite Console Overlay. Screenshots and replays work again after the game resumes.
- If Windows shrinks the GameHQ window after a monitor sleeps or the display layout changes, GameHQ restores its normal size.

## Controller fixes

- Native Share and Guide tap, hold and double-tap gestures continue when ordinary controller input switches between Sony Raw Input and XInput. Releases still reach the controller that started the gesture.
- Duplicate-input protection, controller and game presets, the Back/View capture fallback setting, and cancellation when focus, mappings or device connections change remain in place.
- The Input page clears the previous action when a new button is pressed, so an earlier Screenshot result cannot look like confirmation of a new Share press.

## Update feedback

- Install and restart is the first action on the update banner. It uses the existing download, verification, installation and restart flow.
- The About popup keeps update controls, download percentage and progress above the scrolling release notes, visible beside the action that started the download.
- Opening or closing About ends navigation repeats from the previous view. Gallery hover selection and navigation sounds stop behind a modal, and About navigation stops silently at either end.
- Bundled release notes display formatted links instead of raw Markdown. After an update, a short notification confirms the installed version without opening About automatically.

## Hardware verification needed

- DualSense Bluetooth with DS4Windows and HidHide still needs a real-device test without the Share keyboard macro.
- Please test the fix for your report and tell us in its GitHub issue whether it works. 8BitDo extra buttons and Playnite Console Overlay have only been tested with simulated input so far.
