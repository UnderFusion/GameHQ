# GameHQ 0.7.40 (2026-09-22)

## Changed

- While the in-game overlay is the active window, GameHQ now also asks Windows' GameInput runtime for exclusive foreground input, so other GameInput clients are meant to stop receiving the pad while the overlay is open. The policy has exactly one owner that applies it only after the overlay has really taken the foreground, and releases it again on every way out: closing the overlay, another app taking the focus, the game going away, or GameInput being switched off. Guide and Share handling is unchanged. Whether another program actually stops seeing the pad is not something GameHQ can confirm from inside itself, so the diagnostics report the policy it requested and explicitly do not claim isolation.

<!-- gamehq:locale-index -->

## Release notes in other languages

These release notes are published in English by default. [Release notes in 16 languages](https://github.com/underfusion/GameHQ/tree/v0.7.40/assets/release-notes/publication/0.7.40) are kept in the repository; they are not attached to this release as separate downloads.

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
