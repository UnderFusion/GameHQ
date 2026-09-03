# Localization surface inventory

Baseline: GameHQ `dev` at `02a092158b19afc2435c6287227b19af1f61cc35` (`0.7.6`).

Machine-readable companion: `i18n/surface-inventory.json`.

## Baseline finding

GameHQ is English-only. The repository has no `.ts`/`.qm` catalogs, `QTranslator`,
`lupdate`/`lrelease` target, `QLocale::setDefault`, or language-change handler. QML
copy is not wrapped in an ID-based lookup. A few C++ strings use `tr()`, but most
user-facing C++ text is stored in `QStringLiteral` values and no catalog extracts it.

The migration target is the revision-5 ID-based contract: `qsTrId()` in QML and
`qtTrId()` / context-localized ID extraction in C++, with a complete embedded
`en-US` catalog and no raw `gamehq.*` message exposure. No source-ID (`#% `.ts`
format) catalog exists yet.

## Checked surfaces

Each row maps a runtime surface to its owner, extraction and migration route, and
later acceptance coverage. The JSON companion contains the full source lists and
risk flags.

| ID | Class | Runtime owner and source | Extraction and migration | Acceptance coverage |
|---|---|---|---|---|
| `qml-shell` | Application UI | `src/ui/qml/*.qml`, `components/**`, `settings/**`, `helpers/SidebarCategories.js` | Wrap visible literals and accessibility names with ID-based `qsTrId()`; replace cached JS label objects with retranslate-aware lookup keyed by message ID. | Extraction allowlist plus desktop, overlay, settings, dialog, toast, and accessibility language-switch tests. |
| `qml-theme-labels` | Application UI | `src/ui/qml/themes/*.qml`, `Theme.qml` | Translate skin names and blurbs; keep font families, style IDs, color values, and texture IDs stable. | Every visible skin label changes while its persisted ID remains unchanged. |
| `native-shell` | Application UI | `src/app/App.cpp`, `src/tray/TrayIcon.cpp`, `src/notify/**` | Migrate visible literals to ID-based `qtTrId()`; rebuild cached tray actions and regenerate notification timestamps after a language change. | Tray, toast, and settings-quarantine notifications switch language without restart. |
| `input-editor` | Application UI | `src/input/ActionCatalog.*`, `BindingEditorModel.*`, `BindingPattern.*`, `BindingRelation.*`, `ControlId.*`, `ExtraButtonCatalog.*`, `HotkeyManager.*`, `InputEngine.*` | Translate display labels, prompts, validation, connection state, and gesture text via ID-based lookup; never translate action IDs, control codes, or device IDs. Avoid resolving translations of static data before the translator is installed. | Catalog coverage, placeholder preservation, controller-family label checks, and live binding-editor refresh. |
| `capture-gallery` | Application UI | `src/ui/AppController.*`, `GalleryModel.*`, `CaptureLibraryService.*`, `src/capture/**` | Translate messages that reach QML/toasts; keep internal capture diagnostics English; format dates and numbers through the active locale. | Gallery dates, HDR/status summaries, capture failures, and fallback labels render in the selected locale. |
| `update-client` | Application UI | `src/updates/UpdateService.*`, `UpdateDownloader.*`, `UpdateInstaller.*`, `UpdatePreflight.*`, `GitHubReleaseSource.*` | Translate status/error chrome at the UI boundary and preserve technical details and identifiers; use the active locale for dates. | Update states and representative download, trust, preflight, and helper-handoff failures are localized. |
| `startup-launcher` | Application UI | `src/main.cpp`, `src/launcher/LauncherMain.cpp`, `src/launcher/LauncherLocalization.*`, `src/launcher/LauncherStrings.rc` | Load the Qt app locale before constructing visible surfaces; independently resolve the static Win32 launcher locale from persisted `ui.language`, canonical aliases, or Windows UI preferences. | Portable-import failures use Qt catalogs; all five launcher dialogs use sixteen embedded `STRINGTABLE` blocks with complete en-US fallback. |
| `installer` | Installer | `packaging/GameHQ.iss`, `packaging/generated/InnoLanguages.iss`, `packaging/inno/languages/**` | The canonical registry now generates sixteen pinned `[Languages]` mappings; `p5-2` through `p5-4` own custom messages, locale handoff, and acceptance without Qt coupling. | Mapping/provenance checks pass now; later clean install, upgrade, failure, uninstall, and silent scenarios pass for every installer locale. |
| `release-notes-offline` | Release note | `assets/release-notes.json`, `src/app/ReleaseNotes.*` | Ship schema-identical locale assets with explicit fallback; localize section titles and items without translating versions or URLs. | Schema/parity checks and offline fallback render the intended locale. |
| `release-notes-online` | Release note | GitHub release payload via `GitHubReleaseSource`/`UpdateService` and `AboutWhatsNewDialog.qml` | Select locale-authored notes when available; sanitize formatted content and fall back explicitly. | Online/offline parity, missing-locale fallback, and styled-text escaping tests. |
| `playnite-ui` | Application UI | `integrations/playnite/src/GameHQ.Playnite/Settings/**`, `Localization/**`, `GameHQPlugin.cs`, `extension.yaml` | `localized now`: Playnite-native dictionaries drive XAML, view-model states, dialogs, validation, menu, and diagnostics for all 16 production launch locales. | Resource parity, host lookup, package-local English fallback, protected tokens, hardcoded-text scan, and `.pext` inclusion are automated. |
| `updater-helper` | Log-only diagnostic | `src/updater/**`, transaction/recovery helpers | Explicitly `developer/log-only`: keep detached-helper console/log protocol English and stable; translate only mapped errors at the main-app boundary. | Helper output and exit semantics remain byte-stable; mapped app-facing errors are localized. |
| `packaging-ci` | Developer-only text | `packaging/*.ps1`, `integrations/playnite/packaging/*.ps1`, `.github/workflows/**`, `tools/**`, tests | Explicitly `developer/log-only`: keep command output, thrown validation errors, CI labels, standalone probes/tools, and fixtures English. | The auxiliary audit discovers these paths and extraction rejects them. |
| `packaged-docs` | Generated artifact | `CHANGELOG.md`, `packaging/RELEASE_TEMPLATE.md`, `packaging/README-dist.txt`, Playnite release files | Keep maintainer and distribution documentation English unless a separate documentation-localization scope is approved. | Packaging checks confirm these files are not catalog inputs. |
| `user-content` | User content | game names, executable/file paths, capture names, device names, remote error details | Preserve verbatim and interpolate only into translated templates with numbered placeholders. | Tests verify user text and paths are unchanged in every locale. |
| `stable-tokens` | Developer-only text | `Brand.h.in`, `Brand.qml`, config keys, action/control IDs, protocol keys, URLs, registry paths | Add extraction exclusions/comments; translate nearby labels only. | Protected-token checks fail if a locale changes a stable value. |
| `locale-formatting` | Application UI | `GalleryModel.cpp`, `App.cpp`, `ReleaseNotes.cpp`, `UpdateService.cpp`, QML date/count labels | Route date, time, number, and future plural formatting through the selected app locale. | Fixed-clock locale tests cover gallery, toast, release, and update timestamps. |
| `logs-and-telemetry` | Log-only diagnostic | `src/diagnostics/**`, `capture/**`, `input/**`, `gameinput/**`, `updates/**`, `updater/**`, `core/**` | Exclude logging calls and persisted diagnostics; translate only separate presentation-layer templates. | Catalog checks exclude logs while explicitly classified dual-use errors remain covered. |

## Special-case findings

- Static/global data: `ActionCatalog` and related catalogs cache display strings;
  translation must happen after translator installation or on demand.
- Cached native UI: `TrayIcon` constructs `QMenu`/`QAction` labels once and needs an
  explicit rebuild/retranslate path.
- Constant QML/JavaScript: `SidebarCategories.js` object labels and theme properties
  do not automatically re-evaluate when the language changes.
- Plurals: no `%n`/numerus use exists today; future count text must use numerus
  entries rather than concatenation.
- Placeholders: `%1`/`%2`, Inno `[name/ver]`/`%n`, and WPF `{0}` occur and must be
  preserved and validated.
- Rich text: `AboutWhatsNewDialog.qml` emits escaped `Text.StyledText`; translated
  content must retain escaping and supported markup boundaries.
- Mnemonics: no current Qt/Inno mnemonic contract exists. The Playnite label
  `Convert & add` contains a literal ampersand and must be handled deliberately.
- URLs and brand terms: `GameHQ`, `underfusion`, GitHub URLs, licenses, executable
  names, registry paths, and protocol identifiers are protected tokens.
- User content: game titles, device names, file paths, process names, capture names,
  and server-provided technical detail are never translated.

## Boundary decisions deferred to later plan items

This audit does not select the language portfolio or final fallback policy. It only
records that installer language, first launch, live app language, offline notes,
online notes, and the Playnite plugin require explicit, independently testable
ownership. Production catalogs and copy changes are outside `p1-1`.
