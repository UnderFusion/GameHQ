# GameHQ 0.7.4 (2026-08-07)

## What's new

- Fixed HDR screenshots on HDR-enabled displays. HDR tone mapping was already implemented, but an internal feature gate remained disabled by default in public builds, causing screenshots to fall back to the SDR capture path and appear overexposed.
- Fixed occasional double controller navigation caused by the same physical press being mirrored through multiple Windows controller APIs.
- Improved non-exclusive background controller input delivery while games have focus, including Guide/Share delivery where the controller and firmware expose those buttons.
- Improved diagnostics for controller buttons exposed as keyboard macros or disabled by firmware.

## Notes

- Share/Capture support remains hardware-dependent and is still unverified on GameSir G7 Pro. No controller-model compatibility claim is made by this release.

<!-- gamehq:locale-index -->

## Release notes in other languages

These release notes are published in English by default. [Release notes in 16 languages](https://github.com/underfusion/GameHQ/tree/v0.7.4/assets/release-notes/publication/0.7.4) are kept in the repository; they are not attached to this release as separate downloads.

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
