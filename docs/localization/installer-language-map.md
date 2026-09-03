# Installer language map

GameHQ compiles Setup with the signed, pinned Inno Setup 7.1.0 x64 toolchain. The
archive URL, SHA-256, expected publisher, compiler hash, bundled-resource
hashes, and license path are recorded in
`packaging/inno-toolchain.psd1` and `packaging/inno/languages/provenance.json`.
Builds never fetch mutable language files.

`i18n/locales.json` is the source of truth. The generator writes
`packaging/generated/InnoLanguages.iss`; the build rejects a stale generated
file or a compiler whose bundled language resources differ from the qualified
7.1.0 inventory. Every language chain begins with `compiler:Default.isl`, so a
missing translated key falls back to English without changing the selected
application locale.

| Locale | Inno name | UI source | LanguageID | App locale | Selection/fallback |
|---|---|---|---|---|---|
| `en-US` | `english` | `Default.isl` | `$0409` | `en-US` | Final fallback and first entry |
| `zh-Hans` | `chinesesimp` | Bundled `ChineseSimplified.isl` | `$0804` | `zh-Hans` | Separate Simplified Chinese resource; English key fallback |
| `ru-RU` | `russian` | Bundled `Russian.isl` | `$0419` | `ru-RU` | Native; English key fallback |
| `es-419` | `spanishlatinamerica` | Bundled `Spanish.isl` | `$080A` | `es-419` | Approved Spanish source fallback; precedes Spain for non-exact Latin American variants |
| `es-ES` | `spanish` | Bundled `Spanish.isl` | `$0C0A` | `es-ES` | Exact Spain match; English key fallback |
| `pt-BR` | `brazilianportuguese` | Bundled `BrazilianPortuguese.isl` | `$0416` | `pt-BR` | Never collapses to Portugal Portuguese |
| `de-DE` | `german` | Bundled `German.isl` | `$0407` | `de-DE` | Native; English key fallback |
| `ja-JP` | `japanese` | Bundled `Japanese.isl` | `$0411` | `ja-JP` | Native; English key fallback |
| `fr-FR` | `french` | Bundled `French.isl` | `$040C` | `fr-FR` | Native; English key fallback |
| `pl-PL` | `polish` | Bundled `Polish.isl` | `$0415` | `pl-PL` | Native; English key fallback |
| `ko-KR` | `korean` | Bundled `Korean.isl` | `$0412` | `ko-KR` | Native; English key fallback |
| `zh-Hant` | `chinesetrad` | Bundled `ChineseTraditional.isl` | `$0404` | `zh-Hant` | Separate Traditional Chinese resource; English key fallback |
| `tr-TR` | `turkish` | Bundled `Turkish.isl` | `$041F` | `tr-TR` | Native; English key fallback |
| `th-TH` | `thai` | Bundled `Thai.isl` | `$041E` | `th-TH` | Native; English key fallback |
| `uk-UA` | `ukrainian` | Bundled `Ukrainian.isl` | `$0422` | `uk-UA` | Native; English key fallback |
| `it-IT` | `italian` | Bundled `Italian.isl` | `$0410` | `it-IT` | Native; English key fallback |

Aliases remain on their canonical registry entries. The generated mapping
records them for audit, while Inno uses the explicit `LanguageID` values for
automatic selection. Simplified and Traditional Chinese use distinct resource
files and code-page families. `es-419` uses Mexico's `$080A` identifier and is
placed before Spain only for the primary-language fallback pass; Spain's exact
`$0C0A` match still wins on a Spanish (Spain) system.

Inno Setup 7.1.0 promotes both Chinese resources into its signed x64 toolchain,
so GameHQ does not vendor or download separate translations. The toolchain
archive hash and every consumed bundled `.isl` hash are pinned and validated.
