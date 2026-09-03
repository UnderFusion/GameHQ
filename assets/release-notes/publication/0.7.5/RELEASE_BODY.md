# GameHQ 0.7.5 (2026-08-07)

## Added

- The full About release-notes view now lists recent version links across the top, with the newest selected by default (0.7.5). The current-version summary stays compact, while users who skipped 0.7.3 or 0.7.4 can read those changes offline without leaving GameHQ.
- The desktop update banner is now a compact notification that opens the update-aware release dialog (0.7.5). Available updates open directly on the complete changelog with Update, Remind me later and Skip version actions at the top, plus version links for the new, current and recent releases.

## Fixed

- The compact About view now shows Skip version for a real available update without evaluating a removed preview-only helper (0.7.5), preventing a QML runtime warning and restoring the intended skip action.
- Closing a gallery video or another desktop dialog no longer leaves controller navigation without a visible focus indicator (0.7.5). Focus returns through GameHQ's verified native foreground path, waits until the main window is active, and targets the active Settings panel instead of the hidden gallery grid.
- Custom Global controller gestures remain active when a Desktop or Overlay binding uses the same button (0.7.5). Primary-scope trigger ownership now suppresses only contextual fallbacks, never Global tap, hold or multi-tap actions when the engine represents the fallback scope as Global.
- Pressing Cross during video playback now toggles Play/Pause exactly once (0.7.5). Playback now owns the button for its entire press cycle, so the Desktop Confirm tap can no longer resume the clip on release and holding the button cannot enter Bulk Select behind the lightbox.
- DualSense Share and PS buttons no longer stop responding after the first press (0.7.5). When Sony Raw Input delivered a press and the preferred GameInput mirror arrived while the button was held, the capability router silently transferred ownership of the held control to GameInput; the Sony release was then rejected, the control stayed logically held forever, and every later press â€” including standard buttons routed the same way â€” was swallowed as a duplicate. The router now uses press-cycle ownership: the first accepted press owns the cycle, mirrored presses only join it as participants (never a second action, never an ownership transfer), any participant's release closes the cycle exactly once, and a release with no open cycle is explicitly ignored. Dead-owner safe releases and lifecycle resets are preserved. Regression tests cover repeated Share/PS handoff cycles for both Capture and Guide at router and provider-integration level.

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
