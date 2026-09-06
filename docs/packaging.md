# Packaging & Distribution

How GameHQ separates compiler output from the clean portable folders used for
local runs and downloads, and how the application icon is produced.

## Build layers

GameHQ uses three separate trees so generated development files never appear in
the package a user opens:

| Path | Purpose | User-facing |
|---|---|---|
| `out/` | CMake/Ninja cache, generated sources, object files, and freshly linked executables | No |
| `build/` | Clean local portable package launched by `start.bat`; runtime data is preserved when refreshed | Yes, local |
| `dist/GameHQ/` | Clean release package rebuilt from scratch for sharing or archiving | Yes, release |

This follows Qt's Windows deployment guidance: `windeployqt` creates a runtime
tree containing the required libraries, QML imports, plugins, and translations.
Qt resolves plugins relative to the real executable, so the runtime belongs
together under `app/`; it must not be scattered through the package root.

Reference: [Qt for Windows - Deployment](https://doc.qt.io/qt-6/windows-deployment.html).

The static update helper uses miniz 3.1.2, fetched from its tagged upstream
archive with a pinned SHA-256 during configuration. It accepts only stored or
deflated ZIP entries and applies GameHQ's own path, size, layout and manifest
validation before staged files can be considered installable. Its MIT license
is included at `licenses/miniz.txt`.

`packaging/prepare-source.ps1` generates the mandatory revision-bound
corresponding-source ZIP, checksum, and package-visible source notice.
`packaging/validate-source.ps1` checks its layout, hash, and clean build. Every
GPL binary package includes `licenses/SOURCE_OFFER.txt` bound to the exact
release. Keeping the notice under the established `licenses/` package root also
preserves update compatibility with earlier updater helpers.

`packaging/minimum-updater-version.txt` is the single compatibility floor used
by both the embedded update metadata and the signed release manifest. Release
validation requires those values to match so a target version cannot
accidentally require its own not-yet-installed updater.

Clean-source validation resolves Ninja from an explicit parameter, `NINJA_EXE`,
`PATH`, or the optional local developer fallback. CI bootstraps and passes the
checksum-pinned Ninja version explicitly; source archives never contain it.

`packaging/public-app-version.txt` pins README and website buttons to the newest
published app release, so candidate commits never expose unavailable downloads.

## Distribution identity contract

[`packaging/distribution-identity.psd1`](../packaging/distribution-identity.psd1)
is the machine-readable source for packaging and installer identity. Its
`InnoAppId` is permanent: `{1CB27009-5809-408F-8510-1C4F19605565}`. GameHQ is
a per-user product with product ID `underfusion.gamehq`, publisher
`underfusion`, and default installed root
`%LocalAppData%\Programs\GameHQ`. Installed packages omit `portable.flag`.

Setup owns these current-user values:

```text
HKCU\Software\underfusion\GameHQ
  InstallLocation = <absolute installation root>
  Version = <GameHQ VERSION>

HKCU\Software\Microsoft\Windows\CurrentVersion\App Paths\GameHQ.exe
  (Default) = <absolute installation root>\GameHQ.exe
  Path = <absolute installation root>
```

The App Paths entry is part of the installer contract. Autostart remains a
user setting at `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` value
`GameHQ`; installing GameHQ does not enable it. Setup may update only the
values above. Uninstall removes a value only when it still points to the same
installation, and removes the `GameHQ` autostart value only when it is owned by
that installation. It never removes unrelated values or user data.

Discovery never scans disks. A consumer checks an established
`GameHQ.Local.v1` connection, a manually selected launcher, `InstallLocation`,
App Paths, the current-user Run value, then the default installed root. A valid
candidate contains root `GameHQ.exe` and `app\GameHQ.exe`. The global
`%TEMP%\gamehq.lock` continues to prevent simultaneous portable and installed
copies, while the updater helper always runs from the application root.

Version-one uninstall removes installed program files, shortcuts, its uninstall
entry, and only integration values that still point to that exact installation.
It removes an owned `GameHQ` autostart value to avoid a dead launch entry, but
preserves changed or unrelated registry values. It never presents a data-purge
option and never removes AppData, `Videos\GameHQ`, historical capture roots,
portable folders, or media. A future data purge must be a separate in-app tool
with its own inventory and explicit confirmation.

Release artifacts are named from the same data file:

```text
GameHQ-<version>-win64-setup.exe
GameHQ-<version>-win64-portable.zip
GameHQ-<version>-win64-update.zip
GameHQ-<version>-win64-update.zip.sha256
GameHQ-<version>-source.zip
GameHQ-<version>-source.zip.sha256
gamehq-release.json
gamehq-release.sig
```

The installer compiler is pinned separately in
`packaging/inno-toolchain.psd1`. `bootstrap-inno.ps1` downloads the immutable
official Inno Setup 7.1.0 x64 release, verifies both its SHA-256 and expected
Authenticode signer, and installs it in portable mode below `tools/`.
`build-setup.ps1` compiles the full offline Setup exclusively from the neutral
payload. No web bootstrapper, service, scheduled task, Defender exclusion, or
automatic HidHide elevation is part of Setup.

`test-installer-regression.ps1` compiles the same script with isolated AppId,
registry, and mutex identifiers, then exercises fresh install, upgrade,
maintenance refusal, silent exits, uninstall, and critical locale selection
without modifying an existing GameHQ installation.

The installer-to-application language handoff is generated from
`i18n/locales.json`. On a genuinely fresh installed profile, Setup writes the
semantic locale to the per-user `BootstrapLanguage` registry value and records
`BootstrapLanguageOffered`. The application validates and persists the value
once, then removes only `BootstrapLanguage`; the marker prevents later upgrades
or reinstalls from recreating it. Existing profiles and explicit `system`
preferences win, while portable targets never receive bootstrap state. Setup
never edits `config.json`. `test-installer-bootstrap.ps1` exercises all sixteen
mappings and the one-shot precedence rules using isolated registry/profile data.

Setup targets x64-compatible Windows 10 1903 or newer and installs per-user
without elevation. It publishes the complete version, publisher, support,
updates and uninstall-icon metadata. It never force-closes GameHQ or restarts
it through Restart Manager. Silent automation treats all nonzero Inno codes as
failure. Silent Setup reserves code `20` for a running application and `21` for
an active update/recovery transaction. Inno's cloned Uninstall process exposes
only success (`0`) versus a nonzero refusal/failure; callers must not infer a
specific reason from its nonzero value. Trust and payload gates are added by
their owning release task. The accepted GameHQ process holds
`Local\GameHQApplicationActive` for its whole lifetime. Setup and Uninstall use
the matching Inno `AppMutex`, never ask Restart Manager to terminate the
process, and require a normal user-initiated shutdown before replacing files.

## Neutral payload and package layout

`assemble-package.ps1 -Mode Neutral` creates `dist/.program-payload/` as the
single flag-free program tree. Portable staging copies it and adds
`portable.flag`; installer staging consumes it unchanged; update staging copies
only its allowlisted program files and renames the updater to its pending name.
The neutral payload is validated to contain no capture or configuration data.

`build/` and `dist/GameHQ/` use this root layout. `portable.flag` appears only
in a portable package; an installed layout omits it, and update-only packages
must never contain or modify it:

```txt
GameHQ/
  GameHQ.exe      tiny static Win32 launcher; starts app\GameHQ.exe
  README.txt      user-facing readme with the current version
  portable.flag   keeps settings, captures, and logs inside this folder
  app/            real exe + Qt/FFmpeg/MinGW DLLs + plugins + QML runtime
  Captures/       created at runtime - screenshots and clips by game
  gamehq-data/    created at runtime - config, DB, thumbnails, and logs
```

Windows requires load-time DLLs next to the executable that loads them. The
launcher keeps those files one level down while leaving one obvious executable
at the root. `Paths.cpp` accepts `portable.flag` beside the real exe or one level
above it, so both package folders store user data at their clean root.

The launcher forwards its command line to `app\GameHQ.exe` verbatim: it replaces
only the program name and never re-quotes the arguments, so quotes, embedded
spaces and trailing backslashes reach the app exactly as typed
(`src/launcher/LauncherCommandLine.cpp`, covered by `tst_launchercommandline`
against a real child process). Nothing in that path assumes `MAX_PATH`; a
command line Windows could not carry, or an install path too deep for
`CreateProcessW`, is reported and refused rather than truncated. Switches such
as `--post-update` are matched as whole arguments, so a capture path containing
that text is not mistaken for the switch.

The launcher remains static and Qt-free. Its five failure dialogs use embedded
Win32 string resources for all sixteen production locales. It reads the existing
`ui.language` setting, applies canonical aliases, falls back through Windows UI
preferences, and finally uses complete embedded en-US resources; it never stores
a translated display string or depends on desktop-app localization files.

## Configure, build, and package

Configure once into the developer-only `out/` tree (full toolchain command is in
[dev-setup.md](dev-setup.md)), then build and assemble either package:

```txt
tools/cmake/bin/cmake.exe --build out
powershell -ExecutionPolicy Bypass -File packaging/assemble-package.ps1 -BuildDirectory out -Destination build -PreserveUserData
powershell -ExecutionPolicy Bypass -File packaging/make-dist.ps1
```

A release build must select its trust state explicitly, for example:

```powershell
$env:RELEASE_TRUST_MODE = 'unsigned-beta'
powershell -ExecutionPolicy Bypass -File packaging/make-dist.ps1
```

The command builds Setup, Portable and Update together and writes
`release-evidence.json` with final hashes, sizes, trust state, Authenticode
observations, the pinned Inno version/checksum and a `buildTools` block naming
what produced the bytes — CMake, Ninja, Python, the .NET SDK, Qt, the compiler,
PowerShell and the OS build. A tool that cannot be queried is recorded as
`unavailable` rather than omitted, so the record is never silently incomplete.
`unsigned-beta` requires
honest Beta wording and consistently unsigned GameHQ-built binaries; `signed`
rejects a missing/invalid signature or inconsistent publisher.
`-ManifestMode test` exercises the reviewed Ed25519 generator and verification
consumers with an explicitly public test key. `-ManifestMode production` loads
the production seed from Windows Credential Manager for a local release, or
from the protected `production-release` GitHub environment, and emits only the
detached manifest signature. The checked-in `packaging/release-trust.json`
contains the corresponding public key and key ID only.

### Production release-manifest signing

Every public GameHQ release must use the existing production key and production
manifest mode:

```powershell
powershell -ExecutionPolicy Bypass -File packaging/make-dist.ps1 `
    -BuildDirectory out-production `
    -TrustMode unsigned-beta `
    -ManifestMode production
```

This produces `gamehq-release.json` and `gamehq-release.sig`, verifies them
against the checked-in public key, and records the key ID and public-key
fingerprint in `release-evidence.json`. The command must stop if the protected
credential is unavailable; it must never fall back to a test key or silently
create a replacement.

`gamehq-prod-2026-01` is reused for each release. Do **not** generate a new key
for each version. Provision another key only during an explicitly reviewed
rotation or compromise-recovery operation, then ship its public trust record
before using it to sign releases. The provisioning command refuses to
overwrite either existing protected copy.

The `Unsigned Beta release gate` workflow repeats that exact path on a clean
Windows runner for pull requests and pushes to `dev` or `main`. Before upload,
`assert-beta-artifact-set.ps1` requires the exact seven-file CI artifact set and
recomputes every size and SHA-256 value recorded in `release-evidence.json`.
The uploaded bundle is short-lived CI evidence, not a GitHub Release. A
separate negative step proves that a publishable tag cannot validate while the
public test key is selected. Production signing remains isolated from ordinary
CI; Authenticode is a separate release gate.

Installed builds expose **Settings > Advanced > Portable profile**. Import is
available only when the installed library is empty. GameHQ closes the live
database, restarts into a dedicated importer, rebases audited `portable:/`
paths, clears derived thumbnail/icon references, validates SQLite, then swaps
the staged profile into place. Capture media and the portable source remain
untouched; any failure restores the original empty installed profile.

The importer uses a persistent transaction journal so restart recovery can
finish rollback after interruption. It never merges two populated libraries,
moves source media, rewrites unknown `portable:/` fields, or deletes the source.
Derived thumbnails and game icons are rebuilt in installed storage. The UI
shows the source and destination before confirmation and reports the evidence
or recovery location when the operation cannot complete.

Uninstall changes neither data ownership nor license rights. AppData, capture
media, watched folders, and portable profiles remain user-controlled, and the
The applicable license grant continues to apply to every downloaded GameHQ version distributed
under it.

`start.bat` automates the first two commands. It incrementally builds in `out/`,
stops the running app, refreshes only generated content in `build/`, preserves
`build/Captures/` and `build/gamehq-data/`, then launches the root executable.

`make-dist.ps1` first recreates the neutral payload, derives `dist/GameHQ/`,
then creates `dist/releases/GameHQ-<version>-win64-portable.zip` plus the
program-only `GameHQ-<version>-win64-update.zip` and checksum, then runs
`validate-release.ps1`. The gate checks filenames, manifest/version agreement,
checksum, neutral/ZIP allowlists, packaged binary version and updater tests.

### Build-directory identity

The build that is packaged is the build that is validated. `make-dist.ps1`
passes its `-BuildDirectory` selection to `validate-release.ps1`, which resolves
it through `resolve-build-directory.ps1` before doing anything else and never
falls back to `out/`. A missing, empty or unconfigured selection fails with the
selected path in the message, and `-SkipTests` cannot bypass that resolution.
The native candidate tests (`ctest --test-dir`) and the packaged-localization
work directory both follow the selected build, so packaging `out-production/`
can no longer be validated by tests running in `out/`.

In `unsigned-beta` mode the three packaged GameHQ binaries must be byte-identical
to their sources in the selected build directory; the assembler only copies them,
so a mismatch means the package and the tests describe different builds. Signing
rewrites those files, so `signed` mode records the same hashes without requiring
equality. `release-evidence.json` gains a `build` block with the source commit,
the selected build directory, its `CMakeCache.txt` hash, whether the native
candidate tests ran, and the built/packaged hash pair for every binary.
`packaging/test-build-directory-identity.ps1` proves this on a fixture whose
default `out/` directory does not exist at all.
The assembler validates its destination, copies the real executable
into `app/`, runs `windeployqt --qmldir src/ui/qml --compiler-runtime`, adds the
FFmpeg DLLs that Qt Multimedia loads dynamically, then writes the launcher,
readme, and portable marker at the root.

## App icon

- Source of truth: `assets/icons/gamehq.svg`.
- `assets/icons/gamehq.ico` carries true per-size renders (16-256 px); its
  regeneration recipe is in `assets/icons/icongen.cpp`.
- `assets/icons/gamehq.rc` embeds the ICO into both executables for Explorer,
  taskbar, tray, and shortcut use.
- At runtime `main.cpp` and `TrayIcon.cpp` use `:/icons/gamehq.ico`; QML sidebars
  keep the SVG for crisp scaling.
- The top-level CMake file locates MinGW's `windres.exe` beside the configured
  C++ compiler before enabling resource compilation.
