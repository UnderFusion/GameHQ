# GameHQ 0.7.42 (2026-09-22)

## Added

- Closing the overlay while a button, a trigger or a stick direction is still held no longer passes that held state straight to the game. GameHQ now waits - briefly, never longer than about half a second - until the controller is physically back at rest before it hands control back, and it does so without pressing or releasing anything on the game's behalf. When the controller is already at rest, closing is as immediate as it always was.

## Changed

- Every overlay close now records how that hand-over went: whether the controller came back to rest in time, whether the short wait ran out and the close finished anyway, whether something else had already given the controller back, or whether the close never needed to wait - for example when the overlay steps aside for the GameHQ window, or when the game it covered is gone. A close that ran out its wait says so, instead of being reported as a clean hand-over.
- If the controller is disconnected while the overlay is closing, the close finishes straight away: a controller that is no longer there cannot pass anything on to the game, so there is nothing to wait for.

<!-- gamehq:locale-index -->

## Release notes in other languages

These release notes are published in English by default. [Release notes in 16 languages](https://github.com/underfusion/GameHQ/tree/v0.7.42/assets/release-notes/publication/0.7.42) are kept in the repository; they are not attached to this release as separate downloads.

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
