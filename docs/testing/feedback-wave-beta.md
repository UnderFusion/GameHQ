# Feedback-wave hardware beta (cpo-h01 + cpo-h02)

This is a **local hardware beta**, not a stable release and not a published
download. It exists to collect exactly two physical evidence checklists:

- `FEEDBACK-WAVE-DUALSENSE-CHECKLIST.md` - wired DualSense stability and
  mapping presets (cpo-h01)
- `FEEDBACK-WAVE-OVERLAY-CHECKLIST.md` - overlay above borderless games,
  including the reported title (cpo-h02)

Extract the portable ZIP beside these instructions into a new folder and run
`GameHQ.exe` from there. Do not replace or update an installed GameHQ copy, and
do not delete `portable.flag`: the beta keeps settings, captures and logs
inside its own folder.

## Package identity

The authoritative identity is `beta-manifest.json` and
`GameHQ-0.7.35-feedback-wave-beta-portable.zip.sha256` in this folder. Values
recorded when this build was packaged:

| Field | Value |
|---|---|
| Version | 0.7.35 |
| Source commit | bfbb31c2fb4c5fcc465125285b7fe4abe92cecc4 (dev) |
| Product tree at packaging | clean - compiled and packaged inputs match the commit |
| Package | GameHQ-0.7.35-feedback-wave-beta-portable.zip |
| Package SHA-256 | `969d23b4ef6d4ec30ecdfe9903204ea16b41d39a0ac037b42c3eae7c4bf9a288` |
| `GameHQ.exe` (root launcher) SHA-256 | `577f2cee4d536e57dde24d6516361fcdc4481961814163e60b9c84600fd7f695` |
| `app\GameHQ.exe` SHA-256 | `b51fec820bcf21cd4d5c6077a8debe67bdd1f8e555c2aef30fdbfc30a5444781` |
| Build | Release - Ninja - MinGW-w64 GCC 13.1.0 - Qt 6.8.3 mingw_64 |

Verify the ZIP hash before testing:

```
certutil -hashfile GameHQ-0.7.35-feedback-wave-beta-portable.zip SHA256
```

## Before sending results

Record for each checklist: Windows version, controller model + firmware + USB
port (DualSense cases), GPU and driver version (overlay cases), the games used,
the display mode, and the evidence the checklist asks for. Attach
`gamehq-data\logs\gamehq.log` only after a failure. Review every attachment
before sending: the diagnostic exports are sanitized, but screenshots are not.

## What is already verified automatically

The same frozen source bytes passed the integrated gate recorded for
`cpo-x03`: 107/107 automated checks, 0 failures, 0 skips on one Release build.
On 2026-09-21 the packaged app additionally passed, from an extraction of this
ZIP: `--assert-version 0.7.35` (exit 0), `--smoke-test` (exit 0, log written
into the package folder, session marker removed on exit) and the packaged
localization self-tests for `en` and `de` (exit 0 each). Physical behaviour -
the purpose of this beta - is NOT covered by any of those checks and must
never be inferred from them.

Composition note: `app\dxcompiler.dll` and `app\dxil.dll` (present in the
0.7.7 release package) are intentionally not shipped here; see `deviations`
in `beta-manifest.json`. They belong to a graphics backend the app never
selects.

## How to report

Return both completed checklists with the diagnostic exports they request.
Mark a case **PASS** only after directly observing it, **FAIL** with what
happened instead, and **UNRUN** with a reason otherwise - never leave a case
implicitly assumed. Do not edit or re-zip the package before testing; if a
package problem is suspected, report it and stop.

Suggested line to open a report: `GameHQ 0.7.35 feedback-wave beta, package
SHA-256 <value>, <Windows version>, <controller / game+mode>`.
