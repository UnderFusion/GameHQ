# GameHQ 0.7.6 (2026-08-31)

## Fixed

- Fixed an input path that could cause system-wide mouse stuttering or brief freezes while GameHQ was running. Global Mouse Back/Forward/Middle monitoring is now installed only while a mouse binding exists or mouse-binding capture requires it.
- When global mouse monitoring is required, the Windows low-level mouse hook now runs on a dedicated worker thread instead of the interface thread. Hook start/stop and rapid-restart handling were also hardened so held buttons are released cleanly and stale queued input cannot survive across hook lifetimes.
- WinMM joystick discovery no longer performs potentially slow device scans on the interface thread, preventing periodic GameHQ interface stalls on systems where discovery is slow.

## Diagnostics / Reliability

- Added lightweight, rate-limited performance diagnostics for slow XInput and WinMM operations and interface event-loop stalls. Any remaining hitch should now leave useful Perf: evidence in gamehq.log.

<!-- gamehq:locale-index -->

## Release notes in other languages

These release notes are published in English by default. Every production language is attached to this release as a separate Markdown asset.

| Language | Locale | Asset | Content |
| --- | --- | --- | --- |
| English | `en-US` | [release-notes.en-US.md](release-notes.en-US.md) | English source |
| 简体中文 | `zh-Hans` | [release-notes.zh-Hans.md](release-notes.zh-Hans.md) | English fallback (no reviewed translation) |
| Русский | `ru-RU` | [release-notes.ru-RU.md](release-notes.ru-RU.md) | English fallback (no reviewed translation) |
| Español | `es-ES` | [release-notes.es-ES.md](release-notes.es-ES.md) | English fallback (no reviewed translation) |
| Português (Brasil) | `pt-BR` | [release-notes.pt-BR.md](release-notes.pt-BR.md) | English fallback (no reviewed translation) |
| Deutsch | `de-DE` | [release-notes.de-DE.md](release-notes.de-DE.md) | English fallback (no reviewed translation) |
| 日本語 | `ja-JP` | [release-notes.ja-JP.md](release-notes.ja-JP.md) | English fallback (no reviewed translation) |
| Français | `fr-FR` | [release-notes.fr-FR.md](release-notes.fr-FR.md) | English fallback (no reviewed translation) |
| Polski | `pl-PL` | [release-notes.pl-PL.md](release-notes.pl-PL.md) | English fallback (no reviewed translation) |
| 한국어 | `ko-KR` | [release-notes.ko-KR.md](release-notes.ko-KR.md) | English fallback (no reviewed translation) |
| 繁體中文 | `zh-Hant` | [release-notes.zh-Hant.md](release-notes.zh-Hant.md) | English fallback (no reviewed translation) |
| Türkçe | `tr-TR` | [release-notes.tr-TR.md](release-notes.tr-TR.md) | English fallback (no reviewed translation) |
| ไทย | `th-TH` | [release-notes.th-TH.md](release-notes.th-TH.md) | English fallback (no reviewed translation) |
| Español (Latinoamérica) | `es-419` | [release-notes.es-419.md](release-notes.es-419.md) | English fallback (no reviewed translation) |
| Українська | `uk-UA` | [release-notes.uk-UA.md](release-notes.uk-UA.md) | English fallback (no reviewed translation) |
| Italiano | `it-IT` | [release-notes.it-IT.md](release-notes.it-IT.md) | English fallback (no reviewed translation) |
