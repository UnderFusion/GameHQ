# Localization architecture and contract

This document freezes the GameHQ localization contract (plan item `p1-3`). It defines the
stable message-ID system, the locale manifest, source-language policy, fallback rules, the
glossary, translation states, and the release-quality thresholds. After this contract is
adopted, agents add strings and languages against it without source-text key churn or
feature-specific language logic.

Companion artifacts:

- `i18n/locales.json` — the single locale registry (approved portfolio, plan item `p1-2`).
- `i18n/schema/locale.schema.json` — schema that validates the registry at build time.
- `i18n/schema/message-state.schema.json` — schema for the per-message-ID state manifest.
- `docs/localization/surface-inventory.md` and `i18n/surface-inventory.json` — the `p1-1`
  audit of every translatable and locale-sensitive surface; this contract is its direct
  resolution.

Scope: this item fixes identifiers, schemas, fallback semantics, and quality gates. Bulk
string migration and production translations are later items.

## 1. Message-ID system

GameHQ uses **ID-based Qt translation everywhere user-facing text is produced**.

- QML: `qsTrId("gamehq.settings.general.language.label")`
- C++: `qtTrId("gamehq.settings.general.language.label")`
- C++ context-native extraction (translation-time, not runtime lookup): `QT_TRID_NOOP(...)`

Every public message ID uses the reserved `gamehq.` prefix. A failed `qsTrId()`/`qtTrId()`
lookup returns the raw ID, so the prefix makes any leak immediately recognizable and greppable.

**Naming convention** — hierarchical, lowercase, dot-separated, feature to leaf:

```txt
gamehq.<area>.<subsection>.<element>.<part>
```

- `area` is a top-level surface: `settings`, `overlay`, `capture`, `input`, `gallery`,
  `update`, `notify`, `tray`, `launcher`, `about`, `release`, `installer`, `error`.
- `part` ends in a semantic suffix marking responsibility: `.label` (short noun/verb),
  `.title` (heading), `.description` (longer copy), `.tooltip`, `.placeholder`,
  `.prompt`, `.message`, `.error`, `.warning`, `.confirm`, `.action`, `.state`, `.unit`.

Examples:

- `gamehq.settings.general.language.label`
- `gamehq.capture.gallery.empty.title`
- `gamehq.update.preflight.trust.error`
- `gamehq.input.binding.record.prompt`

Rules:

- IDs—not their English text—are the stable identifier. A source change that rewords a
  message **reuses the same ID** and bumps the source hash; it never invents a new ID, and it
  never renames an ID just because the wording changed.
- A new, genuinely distinct meaning gets a **new ID**. A wording tweak does not.
- Every public ID carries a translator comment (`//:`) with a short English context or
  placeholder explanation. Placeholders are numbered and positional unless a named
  substitution is unavoidable (Inno/WPF differ; see the surface inventory).
- Plurals use Qt numerus (`%n`), never `n == 1 ? a : b` concatenation.
- Rich text keeps `Text.StyledText` escaping and markup boundaries; translated strings must
  preserve supported markup and escaping.

## 2. Locale manifest

`i18n/locales.json` is the **only** locale registry. No locale metadata is duplicated in
code; runtime and build paths both read from this file.

Each entry carries: BCP-47 `tag`, native and English `names`, `aliases`, `tier`, `state`,
`direction` (ltr/rtl), `fallback`, `qt_catalog` name, `inno_language`/`inno_message_file`
mapping, and a `completeness_policy`. The `aliases` map resolves any detected system tag to
its canonical locale.

Tiers (owner-approved, `p1-2`):

- **Tier 1** (launch, 12): `en-US`, `zh-Hans`, `ru-RU`, `es-ES`, `pt-BR`, `de-DE`, `ja-JP`,
  `fr-FR`, `pl-PL`, `ko-KR`, `zh-Hant`, `tr-TR`.
- **Tier 2** (follow-up, 4): `th-TH`, `es-419`, `uk-UA`, `it-IT`.
- **Reserve**: `cs-CZ` is the first reserve; further additions are community-demand driven,
  never global-population-only (GameHQ is a Windows PC-gaming utility).

The portfolio is a Steam-hardware-survey proxy, not measured GameHQ usage, so tiers are
data-driven and revision requires no code branching—editing the registry is enough.

**RTL**: Arabic and Hebrew are not ordinary add-on locales. Build direction metadata and the
`ar-XB` RTL pseudo-locale now; enable either language only after a dedicated RTL production
acceptance phase. The pseudo-locales themselves are **not** part of this registry — they live
in a generated development registry only (see section 8).

## 3. Source language and fallback

- **`en-US` is the canonical source language and the final fallback.** Every active public
  `gamehq.*` ID must have a complete, embedded `en-US` entry. This is release-fail-closed.
- Aliases: `zh-CN`/`zh-SG`/`zh` → `zh-Hans`; `zh-TW`/`zh-HK` → `zh-Hant`; `es-MX`/`es-AR`/
  `es-CL` → `es-419`; generic `en` → `en-US`. Unknown or empty tags resolve to `en-US`.
- **Fallback is whole-message or whole-document, never field-level mixing.** A message is
  either fully present in the active locale or fully inherited from its fallback chain; a
  partially-translated message is treated as missing and inherits entirely.
- Fallback order per message: active locale → its `fallback` chain → `en-US`.
- Locale-sensitive formatting (dates, numbers) uses the **active** selected locale even when
  the text itself falls back, and vice-versa is not permitted to drift: formatting follows
  the locale that owns the displayed string.

## 4. Glossary and do-not-translate policy

Brand, identifiers, and technical protocol names are **never translated**. The do-not-translate
set includes, at minimum:

- `GameHQ` and other product/brand names (`underfusion`).
- Game titles and user-provided game names.
- Executable and file/registry paths; port and protocol identifiers.
- Command-line switches, registry keys, config keys, action/control IDs, device IDs, and
  chord syntax.
- Version strings, URLs, and license text.
- Any stable token listed as `stable-tokens` in the surface inventory.

Translators must copy these verbatim; validation fails any locale that alters a protected
token.

## 5. Translation states and message-state manifest

`i18n/state/translations.json` (validated by `message-state.schema.json`) keys each locale
and active message ID to source and translation hashes, domain, status, provenance, and
last translation time. TS catalogs remain the translation payload; the sidecar records
whether that payload is still trusted for the current English source and context.

Statuses:

- `missing` — no translation payload exists.
- `machine_translated` — machine output exists but has not been promoted by the verification workflow.
- `machine_verified` — machine output passed deterministic structural verification.
- `human_reviewed` — a human reviewed and accepted the current payload.
- `stale` — the English source, context, or translation payload differs from recorded hashes.
- `intentionally_inherited` — a non-release locale explicitly uses its registered fallback.

The `source_hash` proves whether the English source changed since a translation was made; a
finished `.ts`/`.qm` entry alone is never trusted to prove the source is unchanged.

## 6. Release-quality thresholds

Release validation fails the build when any of these is true:

- `en-US` catalog is missing any active public `gamehq.*` ID (raw-ID escape gate).
- Any enabled production locale (Tier 1, and Tier 2 once enabled) is below **100% non-obsolete,
  non-stale** coverage of active IDs.
- Any entry breaks placeholder parity, plural-form validity, markup parity, or UTF-8 validity.
- Any protected/do-not-translate token was altered.
- Any raw `gamehq.*` ID can reach a user-facing surface at runtime.

Extraction rejects developer-only text, log-only diagnostics, and stable tokens per the
surface inventory's boundary decisions.

## 7. Ownership boundaries

The surface inventory (`p1-1`) assigns each runtime surface an owner and a migration route.
This contract does not re-litigate those boundaries; it fixes the identifiers, the registry,
fallback, glossary, states, and gates that every later migration item (`p2`+) depends on.

Auxiliary runtimes keep native resource mechanisms, as verified by `p4-7` and documented in
`auxiliary-surfaces.md`. The Playnite plugin consumes Playnite locale resources, the static
launcher uses Win32 string resources, and Inno Setup owns installer resources; none may load
Qt `.qm` catalogs. The detached updater, standalone probes, release tools, and automation keep
stable English diagnostics because they do not render application UI. A helper failure becomes
translatable only after the main Qt process maps its state or exit semantics to a stable message
ID. The shared locale registry and glossary coordinate tags and terminology, not resource loading.

## 8. Development pseudo-locales

Two pseudo-locales exist to expose expansion, direction, and leakage defects before real
translations arrive (plan item `p4-6`). They are **development-only** and are generated into
the build tree; neither the production registry, the release resources, the installer, nor
the public language selector can ever see them.

| Tag | Direction | Purpose |
| --- | --- | --- |
| `en-XA` | ltr | Accented, delimited, ~35–40 % expansion — finds clipping and truncation. |
| `ar-XB` | rtl | Mirrored delimiters, reversed accented words inside an RTL isolate — finds direction and bidi assumptions. |

**Generation.** `tools/i18n/generate_pseudo.py` derives both catalogs deterministically from
`i18n/app/gamehq_en_US.ts` plus the protected-token metadata in `i18n/extracted/messages.json`,
and writes a development copy of the locale registry next to them. Output goes to
`<build>/pseudo-i18n/` only; nothing is written back into `i18n/`. Placeholders (`%1`, `%n`,
`%L1`), markup, URLs, filesystem paths, keyboard shortcuts, version numbers, technical acronyms,
and every glossary/do-not-translate token are copied through untouched. Plural messages keep
valid form counts (2 for `en-XA`, 6 for `ar-XB`).

**Boundary.** The `GAMEHQ_ENABLE_PSEUDO_LOCALES` CMake option defaults to `OFF`. Only with it
`ON` are the pseudo catalogs added to the `GameHQ` target and the generated development registry
substituted for `i18n/locales.json` in the embedded resources. `start.bat` sets it `ON`;
CI beta builds, `packaging/validate-source.ps1`, and every release configuration leave it `OFF`.
A test build (`GAMEHQ_BUILD_TESTS=ON`) generates the catalogs for the test targets but still does
not embed them into the application.

**Checks.** `tools/i18n/test_pseudo.py` (CTest `tst_i18npseudo`) proves determinism, catalog
completeness, token/placeholder/plural invariance, the measured expansion band, and that the
production manifest and every non-developer build configuration exclude the pseudo tags.
`tests/tst_pseudolocales.cpp` (CTest `tst_pseudolocales`) proves the production/development
registry split, runtime selection and layout direction, that dense representative surfaces
(settings, cards, dialogs, update and release-note text, tray labels, error messages) neither
truncate nor overflow under expansion and mirroring, that no raw `gamehq.*` ID reaches a label,
and that semantic chevrons/arrows mirror while physical glyphs such as the `<<` / `>>` media
controls do not.

**Directional icons.** Semantic direction (back/forward chevrons, "next" arrows) is mirrored
through `languageManager.layoutDirection`; physical or brand glyphs (media transport, controller
button icons) are left alone. Window roots opt into `LayoutMirroring.enabled` with
`childrenInherit: true`, and `QGuiApplication::setLayoutDirection` follows the active locale.
