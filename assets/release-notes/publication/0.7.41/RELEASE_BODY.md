# GameHQ 0.7.41 (2026-09-22)

## Added

- A developer-only tool now watches the controller from outside GameHQ. It is a separate program that only observes - no injection, no hooks, no drivers, no virtual devices - and it is never part of an installed copy. It records, phase by phase, whether the pad still reaches another program through the three Windows input paths, and it says "not measurable" rather than claiming isolation whenever it had nothing to compare against.

## Changed

- The exclusive input mode the overlay asks for while it is active is now measured from outside GameHQ instead of being described as unverified. On the test machine, with a controller streaming in the background, another program's controller input stopped while the overlay had the exclusive mode and came back as soon as it was released - twice, in two independent runs. Games that read the controller through other Windows interfaces are unaffected by that mode, which is documented as a limit rather than hidden; the diagnostics keep saying the effect is not verified from inside GameHQ, because that is what they can honestly know.

<!-- gamehq:locale-index -->

## Release notes in other languages

These release notes are published in English by default. [Release notes in 16 languages](https://github.com/underfusion/GameHQ/tree/v0.7.41/assets/release-notes/publication/0.7.41) are kept in the repository; they are not attached to this release as separate downloads.

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
