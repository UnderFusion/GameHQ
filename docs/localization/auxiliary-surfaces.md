# Auxiliary localization boundaries

Plan item `p4-7 / LANG-04G` audited every independently packaged runtime,
installer, build helper, release tool, and standalone script. The structured
source of truth is `i18n/surface-inventory.json`; this document explains the
decisions. Qt catalogs remain exclusive to the main desktop application.

The only permitted classifications are `localized now`, `developer/log-only`,
`English-by-policy`, and `deferred follow-up`.

## Verified component inventory

| ID | Runtime and package boundary | User visibility and locale source | Native mechanism and fallback | Classification |
|---|---|---|---|---|
| `playnite-plugin-runtime` | .NET Framework 4.6.2/WPF, built by MSBuild and shipped as `GameHQ.Playnite.dll` plus `extension.yaml` in a `.pext` | Settings, menu, dialogs, validation, states, and diagnostics consume Playnite's active UI locale; static manifest name remains a protected product name | Twelve Playnite XAML dictionaries, `DynamicResource`, and `ResourceProvider.GetString`; package-local `en_US.xaml` is complete fallback | `localized now` |
| `win32-launcher` | Static C++/Win32 `GameHQLauncher`, shipped as package-root `GameHQ.exe` | Five `MessageBoxW` failures are visible; no locale reaches them today, and `t2` will read the persisted app locale then Windows user default | Win32 `STRINGTABLE` plus `LoadStringW`; complete en-US resources are final fallback | `deferred follow-up` |
| `updater-helper-process` | Static C++ console `GameHQUpdater`, shipped as `app/GameHQUpdater.exe` | Normal launches are detached; stdout/stderr, usage, trace, and log text are diagnostic only, with no locale input | No localization mechanism; stable English output remains the fallback while the Qt app translates mapped failures | `developer/log-only` |
| `inno-installer` | Inno Setup script, shipped as standalone Setup and Uninstall executables | Wizard, task, error, maintenance, and uninstall text are visible; Inno owns language selection and `p5-3` owns first-launch handoff | Official `.isl` files plus `CustomMessages`; English remains complete fallback | `deferred follow-up` |
| `gameinput-runtime-probe` | Standalone C++ console probe, never shipped as an app dependency | Developer/validation console output only; no locale input | None; stable English diagnostics | `developer/log-only` |
| `release-manifest-tool` | .NET 8 console signing/verification tool for maintainers | Developer and release-automation output only; no locale input | None; stable English diagnostics | `developer/log-only` |
| `automation-scripts` | PowerShell, Python, and GitHub Actions under `packaging/`, `integrations/playnite/packaging/`, `tools/`, and `.github/workflows/` | Developer, CI, packaging, i18n, media-inspection, and manual-validation output only; no locale input | None; stable English command output and errors | `developer/log-only` |
| `packaged-documentation` | Independently authored Markdown/YAML READMEs, changelogs, checklists, licenses, and marketplace metadata | Read directly by users or maintainers; no runtime locale reaches these files | Separately authored locale documents if later approved; English source document is fallback | `English-by-policy` |

## Deferred owners and limits

### `win32-launcher`

- Owner: `t2 / LANG-04I`.
- Scope: localize only the five launcher failure dialogs and locale resolution;
  do not add Qt or change launcher/update behavior.
- Acceptance: all five dialogs select the persisted GameHQ locale or Windows
  user default, use Win32 string resources, and fall back completely to en-US.

### `inno-installer`

- Owner: existing `p5-1` through `p5-4 / LANG-05` items.
- Scope: qualify the Inno toolchain, add language entries and custom messages,
  hand off first-launch locale, and automate installer acceptance.
- Acceptance: Tier 1 clean-install, upgrade, error, uninstall, and silent-mode
  scenarios pass without changing the app's established language on upgrade.

## Runtime boundary policy

The shared `i18n/locales.json` registry and terminology policy may inform locale
tags and wording, but each process uses its native resource loader. Playnite
never reads Qt catalogs, the launcher never links Qt, Inno owns installer text,
and the updater's English protocol/log output stays byte-stable. User-facing
updater failures are translated only after the main app maps helper state to a
stable application message ID.

## Machine-checkable coverage

`tools/i18n/test_auxiliary_surfaces.py`, registered as
`tst_auxiliarysurfaceaudit`, validates the four-way classification and required
boundary fields. It discovers every non-Qt executable target, Playnite/tool
`.csproj`, packaging/release/i18n/manual-validation script, and CI workflow,
then fails if a discovered component is not covered. It also pins the current
five launcher dialogs, Playnite user-facing markers, updater log-only contract,
and installer's still-deferred language state so implementation changes must
update the inventory deliberately.
