# GameHQ resource profiling

Source code measured at `e3692dbef39504d9bc75d33ec658046d2ca860af` on the local AUTO branch. These measurements establish stage costs and a regression reference. They did not select a production optimization.

## Environment

- Windows 11 Pro build 26300, AMD Ryzen 7 9800X3D with 8 cores and 16 logical processors, two display outputs.
- NVIDIA GeForce RTX 5090 driver `32.0.16.1742` used by the D3D11 stage probe. AMD integrated graphics driver `32.0.21045.9003` was also installed.
- Release build with Qt 6.8.3, MinGW GCC 13.1.0, CMake 4.3.3, Ninja 1.13.2, Python 3.12.9, and psutil 7.2.2. Standalone C++ probes use C++20 and `-O3`.
- Tests enabled. Production trust test keys, pseudo-locales, and integrated Telegram disabled. No audio was played, no live video was recorded, and the installed GameHQ instance kept running.

## Reproduce

Run from the repository root. Set `$project` to the registered GameHQ root containing its `tools` folder, and `$build` to a disposable Release build tree configured with `GAMEHQ_BUILD_TESTS=ON`. Use empty output directories for each run.

```powershell
$qt = "$project/tools/Qt/6.8.3/mingw_64"
$env:PATH = "$project/tools/Qt/Tools/mingw1310_64/bin;$qt/bin;$env:PATH"
$reports = "$project/.claude-gui-temp/t45-repeat"
New-Item -ItemType Directory -Path $reports -Force | Out-Null

& "$project/tools/cmake/bin/cmake.exe" --build $build --target tst_segmentrecorder
python docs/audit/t45-profiling/measure.py $reports $build (Get-Location).Path $qt 0
& ./docs/audit/t45-profiling/run.ps1 -OutputDirectory "$reports/stages"
python docs/audit/t45-profiling/measure.py --timer-contexts "$reports/stages/fifo_and_timers.exe"
```

The `0` runs only the disposable processes. To repeat the separate passive observation, pass the explicit PID of a GameHQ instance. Each named scenario runs three times. The sampler requests 100 ms sleeps between samples. The measured sample spans and counts reflect the extra time spent reading Windows process and thread counters.

The standalone runner builds the production `AudioCapture.cpp` against a scratch header that exposes private test state, and extracts the unchanged `readBgraTexture` helper from the production pipeline. Neither fixture is part of the application build.

## Controlled benchmarks

| Workload | Repetitions | Result |
| --- | ---: | --- |
| Software D3D11 replay and synthetic A/V | 3 | 14.63–14.94 s wall, 98.76–100.43% of one logical core over the sampled span, and 134.48–139.59 MiB sampled peak private memory. Gap timing and audio continuity passed in all runs. |
| Hardware replay encoder, 1280×720 input and 640×360 output, 30 fps, 48 kHz stereo | 3 | All 225 frames survived rolling and export in each run. Wall time was 1.42–1.46 s. Two process samples per run were insufficient for a reliable peak or CPU comparison. |
| Offscreen overlay layout, Qt software rendering | 3 | All five Qt Test entries passed per run. Test execution was 12–22 ms. The fixture is too short for meaningful sampled process resources. |
| DXGI display refresh, cached encoder capability | 3 × 30 after warmup | Median 0.64–0.71 ms, p95 1.12–1.51 ms. |
| D3D11 1080p FP16 tone-map, preallocated BGRA staging | 3 × 60 after warmup | GPU shader median 0.011 ms, GPU copy median 0.292 ms, blocking CPU Map median 0.485–0.507 ms and p95 1.04–1.44 ms. |
| D3D11 4K FP16 tone-map, preallocated BGRA staging | 3 × 60 after warmup | GPU shader median 0.029 ms, GPU copy median 1.286–1.299 ms, blocking CPU Map median 1.50–1.52 ms and p95 2.64–2.95 ms. |

The A/V fixture synthesizes its inputs and advances the media clock directly. Its CPU usage includes setup, encoding, decoding, and assertions. The D3D11 fixture submits a shader and staging copy to a separate device. CPU Map wait can overlap GPU execution. Adding the three timings would double count work.

A separate reproduction during the clean build passed the same checks. Software replay then took 20.19–36.93 s and hardware replay took 1.60–6.70 s. Wall times are sensitive to host load and should be compared in a quiet session.

### Isolated FIFO, timers, and screenshot readback

These follow-up fixtures ran on the same host while a clean integration build was active. Reported ranges span three runs.

| Workload | Repetitions | Result |
| --- | ---: | --- |
| Production audio FIFO, 48 kHz stereo, 480 frames per packet, alternating push and pop | 3 × 10,000 packets | 10,625 enqueue allocations requesting 38,720,000 bytes per run. Dequeue made zero allocations. Combined push and pop median was 0.200 µs, p95 0.300–0.400 µs, p99 0.500 µs. |
| Audio FIFO burst without a consumer | 3 × 310 packets | Retained the latest 200 packets (2 seconds), dropped the oldest 110, and preserved timestamp continuity across a partial read. |
| Disposable Qt event loop, four timers enabled versus no periodic timers | 3 paired runs of 4.8 s | 300 capture, 19 overlay, 4 HDR, and 3 foreground callbacks per enabled run. Mid-run process counters recorded 865–1,103 context switches over 2.13–2.16 s with timers enabled and zero with timers off. |
| Production screenshot readback, 1080p, stage allocation and CPU row copy included | 3 × 60 after warmup | Median 2.13–2.42 ms, p95 3.35–3.64 ms, worst observed 14.85 ms. |
| Production screenshot readback, 4K, stage allocation and CPU row copy included | 3 × 60 after warmup | Median 7.91–9.61 ms, p95 9.69–12.18 ms, worst observed 16.90 ms. |

The allocation counter covers ordinary C++ `operator new` calls in the fixture. It includes deque blocks, excludes Qt DLL and GPU-driver allocations, and measures a single producer/consumer sequence without contention. The timer fixture places empty callbacks on one thread. It isolates scheduling activity and cannot attribute context switches to individual timers or stand in for GameHQ thread or frame cadence. Per-thread CPU readings varied from zero to 46.875 ms at the observed 15.625 ms granularity. The full screenshot stage runs after the preallocated staging measurement on an already-generated texture and uses the original production allocation and copy code.

Ranked follow-up measurements:

1. Measure full screenshot readback on a live target. It had the highest measured per-call cost, with a wider spread at 4K, and runs on the screenshot path.
2. Attribute thread activity in controlled idle and active capture traces before considering timer changes. The isolated timer experiment measured frequency and scheduling, while ordinary callback work was excluded.
3. Measure production audio FIFO allocator and lock time under real loopback packet arrival. The isolated steady-state count and burst bounds provide a reference.

## Passive observation

The existing GameHQ process was sampled in three approximately six-second windows. CPU consumption was 7.63–8.99% of one logical core and private memory was 574.36 MiB. Windows reported about 1,952–2,072 process context switches per second. Activity and capture state were unknown. These figures cannot establish a baseline for idle, active capture, or controller use.

## Cumulative integration checkpoint

The checkpoint covers the full product source at `e3692db`, including the updater, capture, library, input, shutdown, IPC, logging, and settings changes. Use the project-local Qt and MinGW paths given above. Point `$integration` at a fresh directory.

```powershell
$mingw = "$project/tools/Qt/Tools/mingw1310_64/bin"
$cmake = "$project/tools/cmake/bin/cmake.exe"
& $cmake -S . -B $integration -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DGAMEHQ_BUILD_TESTS=ON `
  -DGAMEHQ_RELEASE_TRUST_TEST_KEYS=OFF "-DCMAKE_PREFIX_PATH=$qt" `
  "-DCMAKE_C_COMPILER=$mingw/gcc.exe" "-DCMAKE_CXX_COMPILER=$mingw/c++.exe" `
  "-DCMAKE_MAKE_PROGRAM=$project/tools/ninja.exe"
& $cmake --build $integration --target GameHQ GameHQLauncher GameHQUpdater
& "$integration/GameHQUpdater.exe" --release-trust-self-test
```

The checkpoint used the existing local source caches for the pinned miniz 3.1.2 and Monocypher 4.0.3 dependencies. To use the same caches, pass `-DFETCHCONTENT_SOURCE_DIR_MINIZ` and `-DFETCHCONTENT_SOURCE_DIR_MONOCYPHER` with their local checkout paths.

Check product compile flags in `compile_commands.json` and the expanded product link commands from `ninja -C $integration -t compdb -x`. The app and updater must use `gamehq_release_trust`. The test signer, test trust variant, and update service test hooks belong to the test targets. The helper self-test must list the production trust table without a test key.

The fresh build linked all three product executables. The compile and link audit found zero test-only flags or signing dependencies in them. The helper self-test reported only the production trust table. All 23 focused CTest targets passed, including the updater transaction test that verifies the shipped helper rejects test signatures. Five synthetic recorder cases also passed, covering gap timing, audio continuity, hardware encoder reuse, and tail recovery on both encoder paths.

```powershell
$regressions = @(
  'configmanager','settingsapplypolicy','navigationstate','portableprofileimporter',
  'updatediscovery','updatertransaction','ipcbounds','shutdownwait','logperf',
  'logrotation','gameinputdelivery','galleryscale','thumbnailscheduler','gdireadback',
  'capturefilesync','controllerstatelifetime','framepumpservice','hdrgputonemap',
  'overlaylifetime','overlaylayout','releasetrust','releasemanifest','maindeleteui'
)
$compiled = $regressions | Where-Object { $_ -ne 'overlaylayout' } |
  ForEach-Object { "tst_$_" }
& $cmake --build $integration --target @compiled tst_segmentrecorder
$env:QT_QPA_PLATFORM = 'offscreen'
$env:QT_QUICK_BACKEND = 'software'
$env:GAMEHQ_AUDIO_E2E = '0'
& "$project/tools/cmake/bin/ctest.exe" --test-dir $integration --output-on-failure `
  -R ('^tst_(' + ($regressions -join '|') + ')$')
Push-Location $integration
try {
  & ./tst_segmentrecorder.exe silentStretchKeepsAudioContinuousAndInSync `
    captureGapKeepsTheVideoTimeline reusedEncoderWritesIndependentSegments `
    exportRecoversSamplesMediaFoundationSkips
} finally { Pop-Location }
```

## Remaining validation

Controlled live WGC SDR and HDR sessions, native overlay interaction, physical controller latency, and attributed scheduler/heap traces require an owner-approved desktop session. These scenarios still need matched capture settings and repeated before/after checks for frame delivery, color, A/V sync, and input latency. Offscreen timing and allocation data cannot establish those outcomes.
