# GameHQ 0.8.2 (2026-10-30)

> This document is the original **English (en-US)** release note. No reviewed Thai translation exists for this version, so the complete English text is published unchanged.

## Beta 1

- GameHQ 0.8.2 Beta 1 is a pre-release for testing native controller shortcuts and clearer update feedback. GameHQ 0.8.1 remains Latest Stable.

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

- Automated checks cover simulated input and actual action dispatch. DualSense Bluetooth with DS4Windows and HidHide still needs a real-device test without the Share keyboard macro; issue #32 remains open.
- Keep Use Back/View as Share off for the virtual Xbox source. Check native Share tap for a screenshot, a two-second hold for replay, double tap for overlay, and both Share and touchpad/Back after reconnecting.
