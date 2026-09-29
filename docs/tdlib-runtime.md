# TDLib runtime for Telegram Integrated

Telegram Integrated (Share to Telegram without leaving GameHQ) uses TDLib, Telegram's
official client library. GameHQ does **not** bundle a third-party binary: the runtime is
built from the official source at a pinned revision and shipped as an *optional* file.

Telegram Desktop sharing (hand a file to the installed Telegram app) never uses TDLib and
works whether or not this runtime exists.

## Pin

| Field | Value |
|---|---|
| Source | https://github.com/tdlib/td |
| Version | 1.8.67 |
| Commit | `bc9c263e2bfee06aaab41e82db51a103376030bc` ("Update version to 1.8.67.", 2026-08-24) |
| License | Boost Software License 1.0 (`licenses/TDLib-BSL-1.0.txt`) |
| Installed at | `runtime/tdlib/tdjson.dll` next to `GameHQ.exe` |

The pin lives in two places that must change together: `third_party/tdlib/tdlib-pin.json`
(for the build script and packaging) and `src/telegram/TdRuntimePin.h` (compiled into the app).

## Building the runtime

`tools/tdlib/build-tdjson.ps1` clones the source, checks out the pinned commit and refuses to
continue if `HEAD` differs, installs `gperf`, `openssl` and `zlib` through vcpkg with the
static-CRT triplet `x64-windows-static`, and builds only the `tdjson` target. The result needs no
Visual C++ redistributable and no other DLL. Requirements are listed in the script header
(Visual Studio Build Tools, CMake, git, a vcpkg checkout in `GAMEHQ_VCPKG_ROOT`).

The script prints the SHA-256 of the DLL. Record it in `kRuntimeSha256` and in
`tdlib-pin.json`, then commit. Until then GameHQ reports the runtime as **Unpinned** and loads
nothing.

> Status: the script follows TDLib's documented Windows build but has not been run yet on the
> maintainer machine. No real `tdjson.dll` is recorded, so Telegram Integrated cannot log in yet.

## Loading rules (`telegram::TdRuntime`)

Loaded lazily, only when Telegram Integrated is enabled and asked to connect. In order:

1. No pinned hash recorded -> `unpinned`. Nothing is loaded.
2. File missing -> `not_installed` (the normal state of a package built without the runtime).
3. SHA-256 differs from the pin -> `hash_mismatch`. The DLL is **not** loaded, so none of its code runs.
4. The OS cannot load it -> `load_failed`.
5. A required export (`td_create_client_id`, `td_send`, `td_receive`, `td_execute`) is missing or
   `getOption version` differs from the pin -> `incompatible`; the library is unloaded.
6. Otherwise `ready`. TDLib's own log verbosity is set to 0 immediately, because its log can name
   chats and files.

A failure is remembered for the session and not retried silently. The status code is what the UI
and the log show; paths, hashes and account data are never logged.

## Packaging

`packaging/assemble-package.ps1` copies `tools/tdlib/<version>/tdjson.dll` to
`app/runtime/tdlib/` only when it exists, and prints which case applied. `licenses/` (including the
TDLib license) ships in every package.

## What this is not

TDLib is a full Telegram *account* session library. GameHQ uses it for Share only: no inbox, no
incoming-message UI, no Telegram notifications. See `docs/share-platform.md`.
