# GameInput spike — go/no-go report (2026-08-01)

Plan item p10 of the 2026-08-01 input-hardening wave. Everything below was
verified **on this machine** (Windows 11 build 26200, project MinGW 13.1
toolchain) with a runtime probe and a real compile test — not from
documentation alone. Spike artifacts live in `tools/spikes/` (gitignored, per
"no spike code in shipping backends"): `gameinput_probe.cpp` (runtime DLL
probe) and `gameinput-nuget/` (the extracted `Microsoft.GameInput` 3.5.262
package plus `tu_test.cpp`, the MinGW compile test).

## Decision: **GO — route 1 (official header + dynamic loading), no bridge DLL**

The blocking question was MinGW viability, and it is settled: the official
`GameInput.h` from the `Microsoft.GameInput` NuGet package (v3 API surface,
`GameInput::v3` namespace) **compiles cleanly under the project's MinGW 13.1**
with zero patches — interfaces, callback typedefs, `GameInputCreate`
signature, `APP_LOCAL_DEVICE_ID`, the lot (`tu_test.cpp` produces a valid
object file). The header only depends on `<unknwn.h>`-level COM declarations
that mingw-w64 ships. No MSVC-built C-ABI bridge DLL is needed, and
hand-authored vtables are ruled out entirely.

### The three routes, in the required order

1. **Official headers + dynamic `LoadLibrary` — VIABLE, recommended.**
   Vendor `GameInput.h` (subject to the package's Microsoft Software License
   Terms — the package is explicitly a redistributable; legal review of
   vendoring the header vs. restoring it at build time via NuGet download is
   the one open item), then resolve `GameInputCreate` at runtime with
   `LoadLibraryW` + `GetProcAddress`. Never link statically; fail soft to the
   existing Raw Input/XInput/WinMM stack when the DLL is absent.
2. **MSVC C-ABI bridge DLL — NOT NEEDED.** Its only justification was "MinGW
   cannot consume the header", which the compile test disproved. It would add
   a second toolchain and installer complexity for nothing.
3. **Hand-authored local vtable declarations — REJECTED.** The ABI is a
   moving target: `IGameInputDevice::GetDeviceInfo` changed from
   struct-return to HRESULT-plus-out-parameter between versions, and the API
   namespace advanced v0 → v1 → v2 → v3. A hand-copied vtable would have
   broken silently at least twice.

## Runtime availability on this machine

`tools/spikes/gameinput_probe.exe` output:

| DLL | Loaded from | `GameInputCreate` | File version |
|---|---|---|---|
| `gameinput.dll` | `C:\WINDOWS\SYSTEM32` | present | 0.2309.26100.8894 (in-box v0 line) |
| `gameinputredist.dll` | `C:\WINDOWS\SYSTEM32` | present | 3.3.221.0 (NuGet redistributable line) |

Two coexisting DLLs confirm the versioning model: Windows 11 ships a v0-era
`gameinput.dll`; the v1+ API arrives via the redistributable
(`GameInputRedist.msi`, included in the NuGet package, supported back to
Windows 10 19H1). **Shipping implication:** GameHQ must bundle/install the
redist (or detect-and-degrade) — the in-box DLL alone does not provide the
`GameInput::v3` surface the header targets. Load `gameinputredist.dll` first,
fall back to `gameinput.dll` only if the chosen API version tolerates it, and
degrade to the existing backends when neither resolves.

## Spike questions, answered from the v3 header + docs

- **Share/Guide visibility:** `IGameInput::RegisterSystemButtonCallback`
  filters on `GameInputSystemButtonGuide | GameInputSystemButtonShare` and
  reports current/previous state per device; per-device support is
  discoverable via `GameInputDeviceInfo::supportedSystemButtons`. This is the
  designed replacement for our XInput ordinal-100 Guide hack. Whether a given
  pad (DualSense Create, GameSir screenshot key) actually surfaces as
  `Share` is a hardware question — pending the maintainer smoke test.
- **Extra-button enumeration (the GameSir case):**
  `GameInputKindControllerButton/Axis/Switch` expose the *raw* controller
  surface; `GameInputDeviceInfo.controllerButtonCount` +
  `controllerButtonLabels` enumerate every physical button, including ones
  XInput's fixed 14-button gamepad view drops. This is the API-level fix for
  "screenshot button acts as Menu".
- **Stable device identity (feeds p12):** `GameInputDeviceInfo.deviceId` is a
  32-byte `APP_LOCAL_DEVICE_ID`, documented stable across app restarts *and*
  system reboots, alongside `vendorId`/`productId`. This is exactly the
  identity `xinput.slotN` fingerprints lack. Caveat: it is *app-local* —
  fine for our per-install binding profiles, useless for cross-machine sync
  (acceptable).
- **Deduplication vs XInput/WinMM:** correlate on `vendorId:productId` plus
  connection timing, the same arbitration contract p7 already enforces
  (`ControllerArbitration::backendMayTakeOver` — a GameInput backend would
  enter as another prioritized backend, its VID:PID fingerprint comparable
  with Sony Raw Input's and WinMM's). No new mechanism required.
- **`GameInputExclusiveForegroundInput`:** exists (`GameInputEnumerationKind`
  flags include `GameInputExclusiveForegroundInput`, plus separate
  Guide/Share exclusivity flags). It is an **additional best-effort overlay
  layer only**: exclusivity binds other *GameInput clients*; games reading
  XInput/DirectInput/Raw Input directly are unaffected. It does not replace
  the Exclusive Controller Mode design (`exclusive-controller-mode.md`) and
  must never be advertised as input isolation.
  cpo-o06c put this to work in exactly that spirit: while the in-game overlay
  holds the verified interactive foreground, GameHQ requests
  `ExclusiveForegroundInput` (plus the unchanged background Guide/Share flags)
  and releases it on every exit path, through the single owner
  `GameInputFocusController`. Games with host-side controller handoff (2K/…
  family) may swallow XInput regardless — that stays a documented compatibility
  limit, measured externally in cpo-o06d, not "fixed" in-process.
  cpo-o06d then measured it from the outside with `tools/input-receiver/`
  (developer-only observer process, GameInput + XInput + Raw Input
  `RIDEV_INPUTSINK`): two independent runs on a DualSense measured another
  process's GameInput delivery dropping from ~209/s to **0/s** while the
  exclusive policy was in force and returning to ~249/s after release, while
  XInput and Raw Input stayed `not-measurable` on that hardware. The flag is
  therefore real for other GameInput clients and unchanged for everything else —
  exactly the boundary this paragraph describes.

## Recommended integration shape (for p12 and the future backend)

- New optional backend module, loaded dynamically, OFF when the DLL is
  missing — no hard dependency, no startup cost when absent.
- First production use is **identity only** (p12): resolve the
  `APP_LOCAL_DEVICE_ID` for connected pads and hand it to the binding-profile
  layer. No input routing changes, no GameSir mappings (p15 stays gated on
  probe data).
- Input routing through GameInput (replacing XInput polling, gaining
  Share/Guide callbacks and raw extra buttons) is a separate, later decision
  with its own pad-in-hand verification.

## Addendum (2026-08-04, Phase 5 t17): licensing resolved, vendoring landed

The open legal item is closed: package 3.5 changed the public header and the
loader source/static library to the **MIT license** (in-file banners verified
in the 3.5.262 payload). Both MIT files are now vendored at
`third_party/gameinput/` with pinned hashes and provenance; the runtime
redistributable stays under the Microsoft Software License Terms and is
recorded in `THIRD_PARTY_NOTICES.md`. `GameInputRuntimeProbe.exe`
(`src/gameinput/GameInputRuntimeProbe.cpp`) proved all three resolution
outcomes on this machine: official loader against the installed runtime,
app-local (side-by-side) `GameInputRedist.dll` next to the executable —
which Microsoft's loader then prefers over the older System32 copy — and a
clean fail-soft report with exit code 2 when no app-local runtime exists.
Production backend work continues in the Phase 5 plan items.

Sources: [GameInput API versioning](https://learn.microsoft.com/en-us/gaming/gdk/docs/features/common/input/overviews/input-versioning?view=gdk-2510) · [GameInput for PC with NuGet](https://learn.microsoft.com/en-us/gaming/gdk/docs/features/common/input/overviews/input-nuget?view=gdk-2510) · [Microsoft.GameInput package](https://www.nuget.org/packages/Microsoft.GameInput) · [RegisterSystemButtonCallback](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/input/gameinput/interfaces/igameinput/methods/igameinput_registersystembuttoncallback) · [GameInput devices (app-local device ID)](https://learn.microsoft.com/en-us/gaming/gdk/docs/features/common/input/overviews/input-devices?view=gdk-2510)
