# GameHQ 0.7.6 (2026-08-31)

## Fixed

- Fixed an input path that could cause system-wide mouse stuttering or brief freezes while GameHQ was running. Global Mouse Back/Forward/Middle monitoring is now installed only while a mouse binding exists or mouse-binding capture requires it.
- When global mouse monitoring is required, the Windows low-level mouse hook now runs on a dedicated worker thread instead of the interface thread. Hook start/stop and rapid-restart handling were also hardened so held buttons are released cleanly and stale queued input cannot survive across hook lifetimes.
- WinMM joystick discovery no longer performs potentially slow device scans on the interface thread, preventing periodic GameHQ interface stalls on systems where discovery is slow.

## Diagnostics / Reliability

- Added lightweight, rate-limited performance diagnostics for slow XInput and WinMM operations and interface event-loop stalls. Any remaining hitch should now leave useful Perf: evidence in gamehq.log.

<!-- gamehq:locale-index -->

## Release notes in other languages

These release notes are published in English by default. [Release notes in 16 languages](https://github.com/underfusion/GameHQ/tree/v0.7.6/assets/release-notes/publication/0.7.6) are kept in the repository; they are not attached to this release as separate downloads.

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
