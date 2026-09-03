# Contributing a locale

One command drives the whole locale lifecycle. It composes the existing
localization tools and the canonical contracts; it never introduces parallel
metadata, never contacts a network service, and never marks anything reviewed
just because generation succeeded.

```powershell
.\tools\i18n\locale.ps1 status
python tools/i18n/locale_lifecycle.py status
```

Both forms are equivalent. The PowerShell wrapper only resolves the project
root and Python for you.

## The portfolio you are joining

- **16 production locales** are the launch baseline: `en-US`, `zh-Hans`,
  `ru-RU`, `es-ES`, `pt-BR`, `de-DE`, `ja-JP`, `fr-FR`, `pl-PL`, `ko-KR`,
  `zh-Hant`, `tr-TR`, `th-TH`, `es-419`, `uk-UA`, `it-IT`.
- **`cs-CZ` is the first reserve locale.** It is registered but not enabled.
- **`en-XA` and `ar-XB` are development-only pseudo-locales.** They live in the
  build tree only, never in `i18n/locales.json`, and the lifecycle command
  refuses to register or promote them.

Every command refuses to run if the portfolio would drop below 16 production
locales, if `cs-CZ` stops being the first reserve, or if a pseudo-locale
appears in the registry.

## Lifecycle

| Command | What it does |
| --- | --- |
| `add` | Registers a new locale as a **disabled reserve** locale and scaffolds its style guide. |
| `update` | Reconciles translation state and refreshes every generated surface. |
| `review` | Reports translation status, provenance, and what promotion still needs. |
| `disable` | Stops offering a locale while keeping its catalog, style guide, and history. |
| `retire` | Parks a locale so it can never be packaged, keeping all reviewed work. |
| `restore` | Returns a disabled or retired locale to reserve, never straight to enabled. |
| `verify` | Runs the deterministic offline localization checks. |
| `package` | Reports release and packaging eligibility per locale. |

`add`, `update`, `disable`, `retire`, and `restore` accept `-Check` (PowerShell)
or `--check` (Python) to print the plan without writing anything.

### 1. Add the locale

```powershell
.\tools\i18n\locale.ps1 add fr-CA -EnglishName 'French (Canada)' -NativeName 'Français (Canada)' -Alias fr-CA -Check
.\tools\i18n\locale.ps1 add fr-CA -EnglishName 'French (Canada)' -NativeName 'Français (Canada)'
```

Both `-EnglishName` and `-NativeName` are required; the command fails clearly
without them. It also fails on a duplicate tag, a duplicate Qt catalog name, an
alias that is already a locale, and an alias already mapped elsewhere.

The new locale is written to `i18n/locales.json` as `tier 0`, `state reserve`,
`completeness_policy complete_at_release`, with `inno_language` and
`inno_message_file` left `null`. **It is not release-eligible and cannot be
packaged.** Running `add` again for the same tag changes nothing.

The command also scaffolds `i18n/style/<tag>.json`. Every guidance field starts
with `UNREVIEWED: `. That marker is how `review`, `verify`, and `package` know a
speaker has not authored the style guide yet, so a generated file is never
mistaken for reviewed content.

### 2. Translate

Translation itself runs through the existing agent protocol, not through this
command:

```powershell
.\tools\i18n\sync.ps1                 # extract IDs and synchronize catalogs
.\tools\i18n\protocol.ps1 -Queue ... -Response ...
.\tools\i18n\apply-response.ps1 ...
.\tools\i18n\verify.ps1 -UpdateState
```

See [agent-translation-protocol.md](agent-translation-protocol.md) and
[contextual-translation-quality.md](contextual-translation-quality.md).

### 3. Review

```powershell
.\tools\i18n\locale.ps1 review fr-CA
```

`review` prints message counts by status and by provenance kind, whether the
style guide is authored, and the human-reviewed coverage. Only a `human_reviewed`
provenance counts as reviewed; successful generation never does.

### 4. Promote

Promotion to an enabled production locale is an **owner release decision**, not
a contributor action, and it is deliberately not a command. `add` prints the
full checklist; `package` re-checks it. See *What a new locale touches* below.

### 5. Disable, retire, restore

```powershell
.\tools\i18n\locale.ps1 disable fr-CA
.\tools\i18n\locale.ps1 retire fr-CA
.\tools\i18n\locale.ps1 restore fr-CA
```

- `disable` sets `state: disabled` and `tier: 0`. The Qt catalog, style guide,
  and translation provenance are untouched.
- `retire` additionally sets `completeness_policy: never_embed`, so packaging
  can never pick the locale up. Reviewed history is kept, never deleted.
- `restore` returns the locale to `state: reserve` with
  `completeness_policy: complete_at_release`. It never re-enables a locale
  directly — re-promotion goes through the same owner decision as a first
  promotion, so a previously reviewed locale is re-verified before it ships.

Disabling or retiring one of the 16 launch locales is refused: it would drop the
portfolio below the release baseline. Withdrawing a launch locale is an owner
release decision that must also update the release-note manifest, the installer
language table, and the Playnite map in the same change.

## What a new locale touches

Adding a language must be manifest and catalog work, never new branches in
application code. These are the surfaces a locale propagates through.

**Qt catalogs and stable IDs.** Source strings are addressed by stable
`gamehq.*` IDs, never by English text. `tools/i18n/sync.py` extracts them into
`i18n/extracted/messages.json` and synchronizes `i18n/app/gamehq_<locale>.ts`.
Renaming an ID is a source change, not a translation change.

**Translation queue, state, and provenance.** `i18n/state/translations.json`
records, per message, the source hash, translation hash, status, and provenance.
`tools/i18n/verify.py` reconciles it; `tools/i18n/protocol.py` validates queue
and response documents. Provenance is how the project distinguishes machine
output from reviewed work.

**Glossary and locale style guide.** `i18n/glossary/glossary.json` holds
protected and do-not-translate terminology; `i18n/style/<locale>.json` holds
per-locale tone, capitalization, button-label form, punctuation, units, and
contextual terminology guidance.

**Playnite localization.**
`integrations/playnite/src/GameHQ.Playnite/Localization/locale-map.json` maps a
GameHQ tag to Playnite's underscore resource name plus its XAML dictionary.
Playnite owns the active language; the plugin has no language setting.

**Win32 launcher resources.** `src/launcher/LauncherStrings.rc` carries one
`STRINGTABLE` per Win32 `LANGUAGE`/`SUBLANG` pair. A new production locale needs
its own block; English remains the fallback table.

**Inno Setup.** `i18n/locales.json` carries `inno_language`,
`inno_message_file`, `inno_language_name`, `inno_language_id`,
`inno_app_locale`, `inno_order`, `inno_source_kind`, `inno_source_locale`, and
`inno_fallback_locale`. `tools/i18n/generate_inno_languages.py` renders the
language and bootstrap sections; `tools/i18n/generate_inno_custom_messages.py`
renders GameHQ-owned `CustomMessages`. See
[installer-language-map.md](installer-language-map.md) and
[installer-custom-messages.md](installer-custom-messages.md).

**Release notes.** `assets/release-notes/manifest.json` pins the production
locale list and per-version integrity;
`assets/release-notes/versions/<version>/<locale>.json` holds either a complete
localized document or an explicit whole-document English fallback.
`tools/i18n/generate_release_notes.py` builds the offline bundles and
`tools/i18n/generate_release_publication.py` builds the publication artifacts.

**Registry semantics.** Aliases resolve detected system tags to a canonical
locale. Native names appear in the language selector. `direction` drives layout
mirroring. `fallback` selects the locale used when a message is absent.
`completeness_policy` and `tier`/`state` decide packaging eligibility.

## Verifying and packaging

```powershell
.\tools\i18n\locale.ps1 verify
.\tools\i18n\locale.ps1 update fr-CA -Check
.\tools\i18n\locale.ps1 package
```

`verify` confirms the portfolio invariants, that the translation state is
byte-current, that the installer language table, installer custom messages,
release-note bundles, and release publication artifacts are all freshly
generated, and that every production locale has an authored style guide. It runs
entirely offline and is deterministic: the same tree always produces the same
result.

`package` reports, per locale, whether it is release-eligible and what is
missing, then runs the release completeness gate. A reserve, disabled, or
retired locale is reported as not release-eligible rather than silently skipped.

`verify`, `update`, and `package` inspect generated repository surfaces, so they
run against this checkout and reject a custom `--root`.

## Troubleshooting

**A raw `gamehq.*` ID appears in the interface.** The ID is missing from the
catalog, or the catalog failed to load. Run `sync`, then
`locale.ps1 review <tag>`; a `missing` status for that ID confirms it.

**`translation state requires update`.** A source string or a translation
changed and `i18n/state/translations.json` is behind. Run
`.\tools\i18n\verify.ps1 -UpdateState`, then review the diff — messages that
became `stale` need re-translation, not just a state refresh.

**Stale hashes after editing an English string.** Changing English source text
changes the source hash, so every locale's entry for that ID becomes `stale` by
design. That is the signal to re-translate, not a bug.

**Placeholder or markup mismatch.** `%1`, `%L1`, `%n`, and inline markup must
appear in the translation exactly as in the source. `verify` rejects a mismatch.
Reordering placeholders is allowed; dropping or inventing one is not.

**Protected terminology was translated.** Glossary tokens, product names,
executable names, command-line flags, and URLs must survive verbatim. Check
`i18n/glossary/glossary.json` and the protected tokens in
`i18n/extracted/messages.json`.

**Missing glyphs or wrong font.** A locale whose script the bundled font does
not cover renders as boxes. Confirm the script is supported before promoting,
and treat font coverage as a promotion blocker.

**Clipped or overflowing text.** Build with `-DGAMEHQ_ENABLE_PSEUDO_LOCALES=ON`
and switch to `en-XA` for ~35–40% expansion or `ar-XB` for mirroring. These
pseudo-locales exist for exactly this check and are never packaged. See
[architecture.md](architecture.md) section 8.

**Installer shows the wrong language.** The `inno_app_locale` must equal the
locale tag, and `inno_language_id`/`inno_order` must be unique. Re-run
`generate_inno_languages.py --check`.

**Playnite shows English.** Playnite selects the language, not the plugin.
Confirm the tag is in `locale-map.json`, that its XAML dictionary exists, and
that Playnite exposes that host locale at all — some do not, and then the
packaged `en_US` dictionary is the correct result.

**Unexpected English at runtime.** Check the registry `fallback` chain, then
whether the locale is `enabled`, then whether its catalog is present.
`review <tag>` shows `catalog present` and per-status counts.

**Release notes appear in English for a translated locale.** The version's
locale document is an explicit whole-document fallback, or the manifest policy
for that release is `fallback-allowed`. That is intentional for releases made
before the locale existed.

**Publication assets disagree with the source.** Regenerate with
`generate_release_publication.py`; `--check` fails when a committed artifact,
its size, or its SHA-256 no longer matches the structured source.
