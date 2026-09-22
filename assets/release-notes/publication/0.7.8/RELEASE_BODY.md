# GameHQ 0.7.8 (2026-09-22)

## Added

- Screenshot and replay requests now receive prompt visual feedback, with clear completion or failure messages and capture sound controls.
- Controller mapping presets support per-device and per-game assignments, safer editing and clearer assignment controls.

## Changed

- The overlay can take foreground above a visible game, request GameInput exclusive input, and wait briefly for controller neutral state before handing input back. Owner testing confirms native wired DualSense behavior; DSX switching remains partial and needs verification.
- The application restores the last page, settings category, gallery filter and reachable window position, including monitors with negative coordinates.
- Bottom control hints have a compact background following overlay dimming, and the running game receives the normal sidebar selection highlight.
- Provider lifecycle diagnostics record sanitized controller identities, runtime failures, overlay state and held-control counts to investigate DSX switching without changing input behavior.

## Fixed

- An unavailable GameInput Guide/Share callback no longer disables ordinary readings or exclusive-focus policy; existing system-button providers remain eligible.
- Replay exports reserve collision-safe filenames and retain their source segments across game changes and shutdown; failed exports preserve existing clips.
- Replay buffer readiness and manual/HDR ownership now follow explicit lifecycle states, with bounded idle expiry.
- Windows capture-border controls report system support and permission outcomes honestly while preserving recording when hiding is unavailable.
- Controller identity, provider arbitration, gesture lifetime, preset changes and disconnect cleanup have focused regression coverage.

<!-- gamehq:locale-index -->

## Release notes in other languages

These release notes are published in English by default. [Release notes in 16 languages](https://github.com/underfusion/GameHQ/tree/v0.7.8/assets/release-notes/publication/0.7.8) are kept in the repository; they are not attached to this release as separate downloads.

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
