# Installer CustomMessages

GameHQ-owned installer text is separate from the stock Inno messages supplied by the
pinned language files. `packaging/i18n/custom-messages.json` is the source of truth;
`tools/i18n/generate_inno_custom_messages.py` creates the native
`packaging/generated/InnoCustomMessages.iss` include.

## Owned presentation inventory

| Key | Surface | Runtime consumer |
| --- | --- | --- |
| `GameHQWelcomeTitle` | Welcome heading | `[Messages].WelcomeLabel1` via `{cm:...}` |
| `GameHQWelcomeBody` | Welcome explanation | `[Messages].WelcomeLabel2` via `{cm:...}` |
| `GameHQDesktopShortcut` | Optional task | `[Tasks].Description` via `{cm:...}` |
| `GameHQAdditionalShortcuts` | Task group | `[Tasks].GroupDescription` via `{cm:...}` |
| `GameHQLaunch` | Final-page action | `[Run].Description` via `{cm:...}` |
| `GameHQStageDetail` | Transaction detail | Pascal `FmtMessage(CustomMessage(...))` |
| `GameHQUpdateActive` | Active-update error | Pascal `FmtMessage(CustomMessage(...))` |
| `GameHQUpdateStale` | Stale-update recovery | Pascal `FmtMessage(CustomMessage(...))` |
| `GameHQNothingRemoved` | Recovery reassurance | Pascal `CustomMessage(...)` after a preserved CRLF |
| `GameHQSetupAppRunning` | Setup refusal | Pascal `CustomMessage(...)` |
| `GameHQUninstallAppRunning` | Uninstall refusal | Pascal `CustomMessage(...)` |

All eleven keys have contextual translations for the sixteen production locales.
Unqualified `en-US` values provide complete fallback, while each other canonical locale is
emitted under its manifest-owned Inno language name. This includes dedicated GameHQ copy
for `es-419`; only the surrounding stock Inno UI intentionally uses the approved Spanish
source fallback recorded by `p5-1`.

`GameHQ`, `underfusion`, executable names, paths, `Windows`, `[name/ver]`, `%n`, and `%1`
retain their protected or structural meaning. The Polish messages are the reference pass
against the contextual policy and `pl-PL` style guide.

## Intentionally nonlocalized identity

Product, publisher, executable, shortcut, registry, and URL identifiers are protected
identity rather than English copy. `VersionInfoDescription=GameHQ for Windows Setup` is
English-by-policy because one locale-neutral executable owns one Windows version-resource
description before Setup chooses a language. The machine-readable manifest records this
exception and its rationale.

The focused audit rejects missing or duplicate keys, pseudo-locales, placeholder or
protected-token drift, physical line breaks, stale generated output, hardcoded owned copy,
and missing runtime consumers. `packaging/build-setup.ps1` runs that audit before compiling
with the pinned Inno toolchain.
