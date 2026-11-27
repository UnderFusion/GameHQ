# Regression and release evidence

The Windows CI workflow uses the same test receipt runner as a local checkout.
It keeps the full CTest suite, repeats concurrency-sensitive tests three times
as separate invocations, and uploads receipts even when a test fails. Playnite
keeps its locked restore and test step and contributes its TRX report.
Localization gates, package validation and release trust checks run separately.
Development CI accepts the existing Playnite prerelease verification mode for
an unlisted newer candidate. Main, release branches and pull requests targeting
those branches keep stable manifest verification.

## Desktop-safe checkpoint

Use a clean checkout and a fresh build directory. Install or select the
project's pinned Qt 6.8.3 and MinGW 13.1 toolchain. This example uses the
project-local paths from the developer setup. In a separate worktree, point
`$toolchain` at the same installed toolchain. Dependencies are resolved by
CMake from the pinned sources during a new configure.

```powershell
$toolchain = "$PWD/tools"
$qt = "$toolchain/Qt/6.8.3/mingw_64"
$mingw = "$toolchain/Qt/Tools/mingw1310_64/bin"
$cmake = "$toolchain/cmake/bin/cmake.exe"
$ninja = "$toolchain/Ninja/1.13.2/ninja.exe"
$env:PATH = "$mingw;$qt/bin;$env:PATH"
& $cmake -S . -B out-evidence -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DGAMEHQ_BUILD_TESTS=ON -DGAMEHQ_RELEASE_TRUST_TEST_KEYS=OFF `
  "-DCMAKE_PREFIX_PATH=$qt" "-DCMAKE_C_COMPILER=$mingw/gcc.exe" `
  "-DCMAKE_CXX_COMPILER=$mingw/g++.exe" "-DCMAKE_MAKE_PROGRAM=$ninja"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$tests = @(
  'configmanager','gameidentity','inputpattern','logperf','logrotation',
  'settingsapplypolicy','navigationstate','shutdownwait','captureacknowledgement',
  'capturefilesync','controllerstatelifetime','gameinputdelivery','galleryscale',
  'integrationclient','integrationprotocol','ipcbounds','portableprofileimporter',
  'releasemanifest','releasetrust','thumbnailscheduler','updatediscovery',
  'updatertransaction','gdireadback','rawinputflood','segmentrecorder'
)
$targets = $tests | ForEach-Object { "tst_$_" }
& $cmake --build out-evidence --parallel 4 --target GameHQ GameHQLauncher GameHQUpdater @targets
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

python tools/testing/run_regression.py --build-dir out-evidence `
  --output-dir out-evidence/receipt --profile headless --jobs 2
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
```

The receipt directory must be new. Run one CTest command at a time per build
directory so generated results stay attributable. The headless profile uses
offscreen Qt, a disposable temp directory and synthetic replay inputs. It
exercises the updater with temporary packages and fixture processes. It does
not start GameHQ, run an installer, play audio or send controller input to
other applications.

The `ci` profile is for an isolated Windows runner. It includes the full
registered CTest suite, including native window cases. Use the headless
profile on a shared desktop.

The `concurrency` profile runs the same three focused rounds as CI in
offscreen mode. In addition to the targets above, build
`tst_integrationservice`, `tst_capturepublisher`,
`tst_capturedatabaserepair` and `tst_processidentity`. Use
`--profile concurrency --output-dir out-evidence/concurrency-receipt`.

## What the receipts prove

| Run type | Evidence |
| --- | --- |
| Unit | Settings and layout policy, input recognition, identity, log rotation and bounded wait behavior. |
| Integration | Disposable library and updater transactions, production trust rejection of test signatures, local IPC, capture handoff and controller delivery. |
| Mock feed | The tracked, curated GitHub-shaped fixture at `tests/fixtures/public-releases-stable.json` is served over loopback. It exercises stable selection and old and current client states without contacting GitHub. |
| Synthetic | Generated video and PCM, offscreen overlay layout, D3D/Win32 fixture checks, and raw-input flood with a fake API. A hardware encoder case may explicitly skip and the Qt report records that skip. |
| Hardware acceptance | Live WGC SDR/HDR, real-game overlay/controller behavior, AMD compatibility and live shutdown liveness need separate owner receipts. See the existing hardware checks. [Acceptance matrix](acceptance-matrix.md) |

Historical audit receipts in local scratch folders are optional reference
data. The tracked profiler and replay synthetic suite regenerate their inputs.
Feed and native fixture setup does not use an earlier checkout, a local
download, or the optional environment override for a public-feed snapshot.

`receipt.json` records the checkout revision and tree, build cache identity,
product and selected executable hashes, toolchain, flags, command, UTC times,
wall duration, exit code, and per-suite counts. Each run retains the CTest
selection, console log, CTest JUnit and available Qt JUnit. CTest and Qt counts
describe different levels. Qt counts include data rows and setup/cleanup
entries and expose native skips that a passing CTest exit cannot show.

A native failure stays nonzero. A CTest timeout is identified separately.
Missing, mismatched, invalid or incomplete reports cannot produce a passing
receipt. Explicit skips produce `passed_with_skips` when the remaining
execution succeeds. A cancelled job may retain a `running` receipt and a
partial log. Its planned entries without a result were unverified.

## Full updater lifecycle

`tst_updatertransaction` exercises the signed transaction and recovery
headlessly with disposable fixture processes. The existing
`packaging/test-update-lifecycle.py` is the full-application harness used in
a separate Windows validation session. Its real post-update launches and
optional installer are outside the desktop-safe checkpoint.

Stage the old portable ZIP, the candidate update ZIP and its detached
manifest and signature, and the freshly built `updater_test_trust_fixture`
under the checkout. The existing packaging scripts generate the candidate
test manifest via `make-dist.ps1 -TrustMode unsigned-beta -ManifestMode test`
on an isolated packaging runner. The full lifecycle requires `psutil`.

Check input layout and hashes without starting anything:

```powershell
python packaging/test-update-lifecycle.py --check-inputs `
  --old-portable out-inputs/old-portable.zip --release-dir out-inputs/candidate `
  --test-helper out-evidence/updater_test_trust_fixture.exe --version 0.8.1
```

For full acceptance in an isolated session, replace `--check-inputs` with
`--work out-lifecycle` pointing to a new checkout-local folder. Use the
candidate release version in both commands. The preflight checks identity
and digests. The updater performs signature and anti-rollback verification
during the actual transaction. Test-trust results are local or CI evidence.
Production package checks use the production helper and trust table.
