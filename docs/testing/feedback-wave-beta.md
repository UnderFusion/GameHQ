# Feedback-wave hardware beta (cpo-h01 + cpo-h02)

This is a **local hardware beta**, not a stable release and not a published
download. It exists to collect exactly two physical evidence checklists:

- `FEEDBACK-WAVE-DUALSENSE-CHECKLIST.md` - wired DualSense stability and
  mapping presets (cpo-h01)
- `FEEDBACK-WAVE-OVERLAY-CHECKLIST.md` - overlay above borderless games,
  including the reported title, and the controller-isolation behaviour between
  them (cpo-h02)

Extract the portable ZIP beside these instructions into a new folder and run
`GameHQ.exe` from there. Do not replace or update an installed GameHQ copy, and
do not delete `portable.flag`: the beta keeps settings, captures and logs
inside its own folder.

## Package identity

The authoritative identity is `beta-manifest.json` and
`GameHQ-0.7.43-feedback-wave-beta-portable.zip.sha256` in this folder. Values
recorded when this build was packaged:

| Field | Value |
|---|---|
| Version | 0.7.43 |
| Source commit | 75ddf417ea114bb89803a43fc0a52c8df4c6759a (dev) |
| Product tree at packaging | clean - compiled and packaged inputs match the commit |
| Package | GameHQ-0.7.43-feedback-wave-beta-portable.zip |
| Package SHA-256 | `eb79c915bbb15725ce353b8213d3eb780e0c461d6f5664263c5ce62ff613654f` |
| `GameHQ.exe` (root launcher) SHA-256 | `577f2cee4d536e57dde24d6516361fcdc4481961814163e60b9c84600fd7f695` |
| `app\GameHQ.exe` SHA-256 | `3fe9d1d35af5f33798279e2807f08503cd7f71698517113b7c0ff0465c8d6141` |
| Build | Release - Ninja - MinGW-w64 GCC 13.1.0 - Qt 6.8.3 mingw_64 |

The launcher hash is unchanged from the previous beta on purpose: the launcher
is a stable stub that starts `app\GameHQ.exe` and carries no version of its own,
so its bytes are expected to stay identical across versions. Everything that
identifies this build is in `app\GameHQ.exe` and `beta-manifest.json`.

The archive was re-created at the commit above after the earlier package (source
`2bf126d`, SHA-256 `101fe08cb82f7763279f017ab1a622eb986ab829a306d57a818853f85bd8f244`)
had been recorded; the difference between the two commits is documentation only,
and the payload is byte-identical - all 1421 files compared one by one, with
`app\GameHQ.exe` unchanged. Archive bytes are not reproducible run to run (the
same payload re-zipped gives a different SHA-256), so the value above, together
with `beta-manifest.json`, identifies exactly this delivered file. These
instructions and the two checklists are delivered beside the ZIP, never inside
it, and are updated in place after the archive is built.

Verify the ZIP hash before testing:

```
certutil -hashfile GameHQ-0.7.43-feedback-wave-beta-portable.zip SHA256
```

## Before sending results

Record for each checklist: Windows version, controller model + firmware + USB
port (DualSense cases), GPU and driver version (overlay cases), the games used,
the display mode, and - new in this package - **which remapper or virtual-pad
software was running** (Steam Input on/off, DSX, JoyXoff, ...). The overlay
checklist asks for that because the exclusive controller policy binds GameInput
clients: it cannot say anything about a path that a translator tool rewrote
before GameHQ or the game ever saw it. Attach `gamehq-data\logs\gamehq.log` only
after a failure. Review every attachment before sending: the diagnostic exports
are sanitized, but screenshots are not.

## What this package changes over the previous beta

The previous feedback-wave beta was built before the whole controller-isolation
work. Between them the overlay learned to take the foreground without
minimising the game (`cpo-o06b`), to ask GameInput for exclusive input while it
is interactive (`cpo-o06c`), to hand the controller back only once it is
physically at rest (`cpo-o06e`), and to state per open exactly what it may claim
about isolation (`cpo-o06f`). A witness to that last point: the open record in
the diagnostics export now ends with

```
isolation=scoped evidence=external-gameinput-clients real-game=unconfirmed
```

or `isolation=unavailable ...` when no exclusive policy was in force. Nothing in
the package asserts more than the documented evidence; `docs/overlay.md`
("Controller isolation: what is verified, and what is not") lists the measured
case and the paths that are explicitly *not* measured.

## What is already verified automatically

The frozen source bytes of this package passed the integrated gate recorded for
`cpo-o06f`: 113 automated checks at `-j 6`, 109 passing with four 120 s
parallel wall-clock timeouts (`tst_mappingpresetstorage` 35.1 s,
`tst_mappingpresetmodel` 33.0 s, `tst_presetswitch` 60.9 s,
`tst_gamesessionpresets` 26.3 s), each re-run serially and passing, with no
assertion failure anywhere.

On 2026-09-22 the packaged app additionally passed, from a fresh extraction of
this ZIP: `--assert-version 0.7.43` (exit 0), `--release-trust-self-test`
(exit 0), the packaged localization self-tests for `en` and `de` (exit 0 each)
and `--smoke-test` (exit 0, `gamehq-data\logs\gamehq.log` written into the
package folder, session marker removed on exit).

Physical behaviour - the purpose of this beta - is **not** covered by any of
those checks and must never be inferred from them.

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

Suggested line to open a report: `GameHQ 0.7.43 feedback-wave beta, package
SHA-256 <value>, <Windows version>, <controller / game+mode>`.
