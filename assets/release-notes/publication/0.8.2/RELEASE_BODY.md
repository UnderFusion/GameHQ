# GameHQ 0.8.2 (2026-10-30)

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

<!-- gamehq:locale-index -->

## Release notes in other languages

These release notes are published in English by default. [Release notes in 16 languages](https://github.com/underfusion/GameHQ/tree/v0.8.2/assets/release-notes/publication/0.8.2) are kept in the repository; they are not attached to this release as separate downloads.

| Language | Locale | Content |
| --- | --- | --- |
| English | `en-US` | English source |
| 简体中文 | `zh-Hans` | English fallback (no reviewed translation) |
| Русский | `ru-RU` | English fallback (no reviewed translation) |
| Español | `es-ES` | English fallback (no reviewed translation) |
| Português (Brasil) | `pt-BR` | English fallback (no reviewed translation) |
| Deutsch | `de-DE` | English fallback (no reviewed translation) |
| 日本語 | `ja-JP` | English fallback (no reviewed translation) |
| Français | `fr-FR` | English fallback (no reviewed translation) |
| Polski | `pl-PL` | English fallback (no reviewed translation) |
| 한국어 | `ko-KR` | English fallback (no reviewed translation) |
| 繁體中文 | `zh-Hant` | English fallback (no reviewed translation) |
| Türkçe | `tr-TR` | English fallback (no reviewed translation) |
| ไทย | `th-TH` | English fallback (no reviewed translation) |
| Español (Latinoamérica) | `es-419` | English fallback (no reviewed translation) |
| Українська | `uk-UA` | English fallback (no reviewed translation) |
| Italiano | `it-IT` | English fallback (no reviewed translation) |
