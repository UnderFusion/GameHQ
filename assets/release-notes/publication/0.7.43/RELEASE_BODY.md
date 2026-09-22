# GameHQ 0.7.43 (2026-09-22)

## Added

- GameHQ now says plainly what it can and cannot promise about keeping your controller input away from the game while the overlay is open. Every overlay open records one of three levels in its log and in the diagnostics summary behind Settings, Advanced, Copy diagnostic summary: no isolation for that open, scoped (input is held back from other GameInput clients, which is the case that has actually been measured), or full native (measured and confirmed for that specific game, controller and setup). The strongest level is never claimed on the strength of an internal call alone.

## Changed

- The controller-isolation documentation now lists the measured case and, separately, everything that is not measured: games reading the controller through XInput, Raw Input, Steam Input or a virtual-pad tool such as DSX, the PlayStation or Guide press that opens the overlay, and how a real game reacts to the moment the overlay closes while a control is still held. Anything that was not measured is named as not measured instead of being implied to be covered.
- The same documentation states the footprint of the normal overlay path in plain terms: nothing is loaded into the game, nothing of it is hooked or modified, no virtual controller or driver is installed, and no input is ever synthesized. It also records what happens when GameHQ closes or is killed: the setting belongs to the running GameHQ process, nothing is left behind on the machine, and the controller keeps working.
- The tester checklist for real borderless games now covers the behaviour as it works today: the overlay taking the foreground without minimising the game, navigation not acting in the game, closing with a control held, the foreground coming back afterwards, changes you make to the foreground never being stolen back, and the controller still working after GameHQ exits.

<!-- gamehq:locale-index -->

## Release notes in other languages

These release notes are published in English by default. [Release notes in 16 languages](https://github.com/underfusion/GameHQ/tree/v0.7.43/assets/release-notes/publication/0.7.43) are kept in the repository; they are not attached to this release as separate downloads.

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
