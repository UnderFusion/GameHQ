# Capture Engine

> Milestones 0.4 (screenshots) / 0.5 (video). Target API: **Windows Graphics Capture** (WinRT, Win10 1903+), no injection. SDR screenshots retain GDI; HDR screenshots and replay share the FP16 WGC frame pump and GPU tone mapper.
>
> **TOOLCHAIN (resolved dev.47): WGC runs on the project MinGW kit — no MSVC.** WGC is WinRT and MinGW can't build C++/WinRT (`cppwinrt`), but WGC has a raw-ABI path that doesn't need it: hand-declare the missing interface vtables/IIDs (`src/capture/wgc_shims.h`) and link the import libs mingw already ships (`windowsapp runtimeobject ole32 oleaut32 d3d11 dxgi dxguid uuid`). Every "requires MSVC / install VS Build Tools" note below is superseded.

## Screenshots — SDR GDI path and HDR WGC path

`GameDetector` resolves and gates the foreground game. On an SDR output, `ScreenshotService` uses screen-DC `BitBlt` (`SRCCOPY | CAPTUREBLT`) and reads the result into a `QImage`. When the experimental HDR flag is enabled and the target output has HDR active, it instead asks `FramePumpService` for the next FP16 WGC frame after `GpuToneMapper` converts it to BGRA8 SDR; a staging-texture readback produces the `QImage`. Both paths then write PNG or JPEG under `<ScreenshotsRoot>/<Game>/Screenshots/` and share the DB, thumbnail, gallery, sound and notification tail.

**Threading (dev.13)**: only the `BitBlt` grab (~30 ms) runs on the UI thread; the format/quality config read also happens here (`ConfigManager` is not meant for concurrent access), then `grabbed()` fires (→ shutter sound, if enabled, plays immediately) and the encode + disk write hands off to a `QThreadPool` worker (`QImageWriter`; PNG uses `setCompression(1)` = speed over size, JPEG uses `setQuality(capture.jpeg_quality)`). A synchronous 4K encode took ~2.7 s and froze the UI/overlay — that is gone. The `captured()` signal (DB insert + tray notification) is delivered back to the UI thread from the worker. Cost: near-uncompressed PNGs (~10 MB at 1440p, ~25 MB at 4K); JPEG trades quality for size — storage cleanup is 0.7 scope.

**Publication is atomic (0.6.36)**: `src/capture/CapturePublisher.{h,cpp}` reserves a capture's timestamped name by creating `<name>.part` *exclusively*, so the two encoder threads can never be handed the same path — the previous `exists()`-then-write check could. `QImageWriter` encodes into that `.part` file, which the scanner ignores (`kImageSuffixes` in `CaptureScanner.cpp`), and only a rename publishes it under the real name. A failed encode discards the reservation; a failed publish keeps the `.part` rather than exposing a truncated image, and `ScreenshotService`'s constructor sweeps `.part` files older than 10 minutes left by a crash. **0.7.8 routes saved replay clips through the same publisher.** `FramePumpWorker::saveReplayOnWorker()` reserves `<stamp>.mp4` before the export starts (a second save inside the same second gets `<stamp>_2.mp4`), `ReplayExporter::concat()` remuxes into the reserved `.part` — the MP4 sink writer truncates the reserved file, so the name stays claimed for the whole export — and `CapturePublisher::publish()` renames it into place. The previous code deleted the final path before renaming and again on failure, which destroyed an existing clip that happened to carry the same timestamp; no failure path deletes anything but its own reservation now. The clip's preview name comes from `CapturePublisher::companionPath()`, so it inherits the same `_2` and stays the `<clip base>_clip.png` pair `ThumbnailService::ensureThumbnail()` reattaches. `FramePumpService`'s constructor sweeps abandoned `.part` files under the clips root the same way. Backpressure is bounded too: a capture is refused with a `skipped()` reason once 8 encodes or 256 MB of retained frames are already queued, because the pool's queue is unbounded and a 4K frame is ~33 MB.

Triggers: DualSense Share **tap**, **Ctrl+Shift+S**, tray **Take Screenshot**. Gate: `capture.mode` (`only_in_games` default | `whitelist` | `always`). After a successful screenshot or replay save, the current-game UI state is refreshed from that committed game so a known captured title appears in the desktop and overlay `Game` section immediately. If the foreground check later lands on GameHQ, the overlay, or a mismatched game title, `CurrentGameService` can fall back to captured games whose stored executable path is still running.

**Known limitation**: native HDR JPEG XR output and its SDR companion pair remain separate work. The current HDR screenshot is an SDR PNG/JPEG produced through the same tone mapper as replay.

## Screenshots — HDR WGC readback

`GraphicsCaptureItem` from the active game HWND (`CreateForWindow`) → FP16 frame pool → `GpuToneMapper` BGRA8 target → staging readback → `QImageWriter` → the shared save/DB/thumbnail path.

The screenshot request forces the frame pump onto the current game when its stored target differs. This prevents a replay buffer armed on Steam or another foreground window from continuing to record that stale HWND after the game takes focus.

## WGC frame pump (0.5 Step 3, shipped dev.47, MinGW raw-ABI)

`FramePumpService` (`src/capture/FramePumpService.{h,cpp}`) is the live frame source the replay buffer builds on. Because Qt's GUI thread is an STA (incompatible with `RoInitialize(MTA)` and a free-threaded pool), all WGC/D3D work runs on a **dedicated MTA worker thread** (`FramePumpWorker`); the GUI-thread front-end only relays arm/disarm/save requests. Pipeline: `D3D11CreateDevice(BGRA)` → bridge to WinRT `IDirect3DDevice` (`CreateDirect3D11DeviceFromDXGIDevice`, resolved from d3d11.dll via `GetProcAddress`) → `RoGetActivationFactory` → `IGraphicsCaptureItemInterop::CreateForWindow(hwnd)` → `IDirect3D11CaptureFramePoolStatics2::CreateFreeThreaded(device, B8G8R8A8UIntNormalized, 2, size)` → `CreateCaptureSession` → `StartCapture`. A **free-threaded pool** means no `DispatcherQueue` / no `FrameArrived` delegate: a `QTimer` (~16 ms) on the worker thread polls `TryGetNextFrame`; each frame's surface is QI'd to `IDirect3DDxgiInterfaceAccess`, `GetInterface(ID3D11Texture2D)` gives the raw texture, and `GetDesc` yields width/height/format. This substage is **log-only** (size/format + per-second fps; no encode, no ring). Arming is automatic (`replay.auto`, master switch in Settings → Replay; no hotkey), gated by `capture.mode` like screenshots. Verified dev.47: a real 4K game (`3840×2160`, `DXGI_FORMAT=87` = B8G8R8A8_UNORM) pumped at a sustained ~60 fps with a clean start/stop, proving every hand-declared vtable slot is correct.

ABI shims live in `src/capture/wgc_shims.h` (supersedes `spike/wgc/wgc_abi_shims.h`). MinGW's g++ ignores `__declspec(uuid(...))` (harmless warning) — interfaces are driven through explicit `IID_*` GUID constants, not `__uuidof`.

### Windows capture border

Windows draws a yellow border around anything captured through WGC. Suppressing it is **not** something the app can decide on its own: it needs Windows 11 22000+ (for `IGraphicsCaptureSession3`), it needs the process to hold **Borderless capture access**, and the flag has to be set **before** `StartCapture`.

The trap is that `put_IsBorderRequired(false)` returns `S_OK` and `get_IsBorderRequired` reads back `false` **even when Windows is ignoring the flag** because the caller lacks Borderless access. Earlier versions logged "capture border hidden" off that HRESULT and were simply wrong.

`AppCapabilityAccessStatus` is an ABI contract with no mingw-w64 header, so its integers are **not** guessed — `tools/spikes/winrt_enum_probe.exe` reads them out of `Windows.Security.winmd` through `RoGetMetaDataFile` + `IMetaDataImport2`: `DeniedBySystem=0`, `NotDeclaredByApp=1`, `DeniedByUser=2`, `UserPromptRequired=3`, **`Allowed=4`**. (An earlier draft assumed `Allowed=0`, which would have decoded a hard system denial as a grant; `static_assert`s in `CaptureBorderState.h` and a focused test now pin the mapping.) On build 26200 this unpackaged install measures `Allowed`.

`src/capture/CaptureBorderState.{h,cpp}` is therefore the only place allowed to reach a verdict. It takes the OS build, the QI result, the access status and the read-back, and returns one of:

| State | Meaning |
| --- | --- |
| `Unsupported` | Pre-22000 build, or no `IGraphicsCaptureSession3`. Windows 10 always lands here — it can never report `Hidden`. |
| `Denied` | Supported, but the access request came back as anything other than `Allowed`, or the read-back still says the border is required. |
| `Unknown` | The OS build could not be read, the request was never made, or the setter/read-back failed — no evidence either way. |
| `Hidden` | 22000+, interface present, access `Allowed`, flag set **and** read back as suppressed. |
| `NotRequested` | This session started with `capture.hide_border=false`; no border probe, access request, setter or read-back was attempted. |

The verdict is logged once per session as `FramePump: capture border <state> (<detail>)` and surfaced on `FramePumpService` through the `captureBorderState` / `captureBorderDetail` properties. Access is requested once per process on the MTA worker thread with a bounded (~2 s) wait, so a silent broker can never wedge pipeline bring-up. The statics IID `{743ED370-06EC-5040-A58A-901F0F757095}` was read off the live activation factory via `IInspectable::GetIids()` rather than guessed, since mingw-w64 publishes no declaration for `GraphicsCaptureAccess`. `Hidden` states that Windows accepted and confirmed the suppression request, not that a human has seen the border disappear; the visual A/B on real hardware is a separate manual acceptance step.

Since 0.7.15, **Settings → Capture → Windows capture border** exposes
`capture.hide_border` (boolean, default `true`). It persists through the normal
config override path and Capture's restore-defaults action resets it to `true`.
The service snapshots the setting when dispatching each new WGC session to the
worker. Changing or resetting this key does not restart the replay buffer,
change the current session's border request, or rewrite its displayed verdict.
Disabling it skips all suppression calls for the next session, including the
access request. It also applies to WGC sessions used for HDR screenshots; GDI
screenshots are unaffected.

The Capture page shows **“Windows reports border suppression active. A visible
border may still remain.”** for `Hidden`, and plain explanations for unsupported,
denied, unconfirmed and not-requested states. Starting, stopping or failing a
session clears the previous verdict so it cannot be mistaken for the current
state. The nine new localization IDs live in the QML source, English catalog and
source manifest; translation state records the missing translations honestly.
The full extraction refresh, 15 translated catalogs and review/readiness artifacts
belong to p7-2. Other languages use the normal English source-catalog fallback meanwhile.
Real in-game visual acceptance remains p7-5.

## Save-replay export (0.5 Steps 5/6/8, shipped dev.50; audio path dev.74, MinGW)

The rolling segments (above) are the temporary **ring** in per-game `gamehq-data/replay-cache/<Game>/<media-format>/` folders. The folder fingerprint includes video dimensions/FPS and, when enabled, audio sample rate/channel count so a device or setting change cannot mix incompatible segments. `SegmentRecorder` keeps only `ceil(length_seconds / segment_seconds)` of them (deletes oldest; drops zero-frame segments that fail `Finalize` with `MF_E_SINK_NO_SAMPLES_PROCESSED`). Segment filenames include milliseconds and a collision suffix because `snapshotForSave()` closes one segment and immediately opens the next. On **Share-hold** (or **Ctrl+Shift+E**), `saveReplay()` copies the current `CaptureLocations` clip root into the pump's MTA worker. dev.90: if the buffer is cold there is no past to save, so the request arms it instead of only reporting the failure (an explicit save outranks `replay.auto`, and it restarts the auto tick so the buffer still disarms when the game leaves the foreground) — the first hold starts recording, the next one saves. Once armed: `SegmentRecorder::snapshotForSave()` finalizes the in-flight segment, snapshots the ring, and reopens a new one (recording never stops); then `src/capture/ReplayExporter.cpp` remux-concats the ring into ONE standard MP4 in `<ClipsRoot>/<Game>/Clips/` using native compressed passthrough. Video copies H.264 samples and clones the native type so SPS/PPS reach the output avcC. dev.79 enables the AAC path again when `audio.enabled=true` and resets audio timestamps after each segment roll/snapshot. Device invalidation discards the incomplete segment and re-arms WGC/WASAPI against the current endpoint. dev.76 treats unreadable/skipped segments and writer failures as an export failure rather than committing a partial short clip. `ReplayExporter::grabThumbnail()` decodes the first frame to a PNG. `AppController::commitClip()` inserts the `type="video"` DB row + thumbnail and refreshes the gallery only after the final MP4 exports successfully; sound + notification fire then.

Replay save debugging is keyed by `ReplaySave[...]` in `gamehq.log`. The trace covers request state, snapshot segment paths/sizes, instant/final thumbnail timing, output path, per-segment video dimensions/fps/audio presence, video/audio sample counts, remux duration, output bytes, and explicit failure/success totals.

### Request chain (`CaptureRequest`)

A press mints one `CaptureRequest` (`src/capture/CaptureRequest.{h,cpp}`) at the
input edge: an id, the source that pressed (`controller`, `keyboard`, `mouse`,
`overlay`, `ui`, `tray`) and milliseconds since the first request of the run.
The id is the chain id in every stage line, so one press is one greppable chain:

```
ReplaySave[12 src=controller +4231ms]: accepted
ReplaySave[12 src=controller +4231ms]: armed generation=3 owner=7 game=Elden Ring
ReplaySave[12]: frozen segments=6 elapsedMs=41
ReplaySave[12]: exporting totalSoFarMs=88
ReplaySave[12]: published
```

The stages are `accepted`, `armed`, `frozen`, `exporting`, `published` and
`failed`. `accepted` is logged before any gate, and every early rejection in
`FramePumpService::saveReplay()` now logs `failed - <reason>` next to the
`clipFailed` it already emitted. Before this, those four rejections emitted the
signal and nothing else: a save that died there left no trace of the press at
all, which is exactly what "I pressed the button and nothing happened" looks
like in a log. Screenshots carry the same chain under `Screenshot[...]`,
including the `capture.mode` gate that silently skipped a press.

The chain id is minted at the press; the owners-table lease token
(`ReplayBufferOwners`) stays the internal id for save ownership and appears as
`owner=` on the `armed` line, so the two can be correlated without conflating
them.

## H.264 segment writer (0.5 Step 4, shipped dev.49, MinGW)

`SegmentRecorder` (`src/capture/SegmentRecorder.{h,cpp}`) turns the frame pump's live BGRA D3D11 textures into rolling **~5-second fragmented-MP4** segments in `gamehq-data/replay-cache/`, encoded with Media Foundation's `IMFSinkWriter`. **No ABI shims** — unlike WGC, MinGW already ships the full MF surface; the only extra link libs are `mfplat mfreadwrite mfuuid mf`. Path: input `MFVideoFormat_RGB32` (== BGRA; top-down via a positive `MF_MT_DEFAULT_STRIDE`) → SinkWriter auto-inserts conversion/scaling as needed → `MFVideoFormat_H264`, container `MFTranscodeContainerType_FMPEG4`, HW transforms + real-time hint on, throttling off. One SinkWriter per segment (`Finalize()` + a new `segment_NNN.mp4` every `replay.segment_seconds`), PTS reset to 0 each file so the first frame is always an IDR keyframe and each segment is independently playable/crash-safe. Frames are read off the GPU via a reusable `D3D11_USAGE_STAGING` texture (`CopyResource`+`Map`, honoring `RowPitch`) on the MTA worker thread, throttled to `replay.fps`. The source frame size and configured `replay.resolution` output cap are separate media types, so 4K games do not require a manual CPU downscale loop before H.264 encoding. Fed through the same auto-arm path. Validate a segment with `tools/check_mp4.py` (box-structure gate; no ffmpeg needed). Still on the roadmap for 0.5: stale-cache cleanup and real audio verification.

## Replay audio (0.5 Step 7, code written dev.74)

`AudioCapture` runs on the frame-pump MTA worker. It uses default render-device desktop loopback (`AUDCLNT_STREAMFLAGS_LOOPBACK`) when `audio.enabled=true`; the earlier process-loopback experiment remains disabled until it can be validated safely. Captured float32 interleaved PCM is timestamped from QPC and fed to `SegmentRecorder::writeAudio`, which creates an AAC stream in each fMP4 segment when audio is active. Audio PTS is reset with video PTS after every segment roll/snapshot so new segments keep accepting AAC samples. `ReplayExporter` preserves AAC during final remux. This path builds cleanly; mark complete only after a real saved replay is confirmed to play with audio.

## Video (0.5)

Frame pump (above) at 30 fps -> Media Foundation `SinkWriter` with hardware H.264 MFT when available (NVENC/AMF/QuickSync via MF), fallback software. 1080p30, 12-16 Mbps. Audio from WASAPI process-loopback first, desktop loopback fallback, encoded as AAC. Segments written as fragmented MP4 (crash-safe); export = remux concat, no re-encode. Details: [replay-buffer.md](replay-buffer.md).

## Fallbacks

If WGC unavailable/fails: log; DXGI Desktop Duplication as a later monitor-capture fallback (not MVP scope).

## HDR detection, replay and SDR output

`HdrCapabilities` detects the active advanced-colour state and Windows SDR-white level per output. `internal.capture.experimental_hdr` defaults to `true`; on an HDR target the frame pump requests `R16G16B16A16_FLOAT`, normalizes the 80-nit scRGB scene values against that live SDR-white level, tone-maps through `GpuToneMapper`, and feeds BGRA8 to both H.264 replay encoding and one-shot SDR PNG/JPEG readback. If the emergency fallback key is set to `false`, or the target is SDR, the existing SDR path remains unchanged.

Detection combines a fresh `IDXGIOutput6::GetDesc1` snapshot with the matching active DisplayConfig target. On current Windows, `DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2` supplies the explicit SDR/WCG/HDR `activeColorMode`; this avoids treating WCG/automatic colour management as HDR merely because the older combined `advancedColorEnabled` bit remains set. Windows 10 falls back to the legacy Advanced Color query, and DXGI PQ/BT.2020 is used only when DisplayConfig cannot report state. `BitsPerColor`, `MinLuminance`, `MaxLuminance` and `MaxFullFrameLuminance` come from DXGI. Outputs whose state cannot be queried report as unavailable rather than silently as SDR.

Encoder probe: `MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, ..., MFVideoFormat_HEVC)`, then each returned MFT is activated and offered an HEVC **Main10** output type (`MF_MT_MPEG2_PROFILE` — the same GUID the SDK also calls `MF_MT_VIDEO_PROFILE` — set to `eAVEncH265VProfile_Main_420_10`). Presence of an HEVC encoder alone proves nothing: plenty of hardware encodes 8-bit HEVC only, so the probe asks for the type it would actually use.

HDR is a runtime toggle, so display state is never treated as a permanent fact. `AppController::refreshHdrStatus()` performs a full probe at startup, whenever the display topology changes (`QGuiApplication::screenAdded`/`screenRemoved`/`primaryScreenChanged`), and from the **Settings → Advanced → Display HDR** Refresh button. A lightweight one-second watcher refreshes DXGI/DisplayConfig state without repeating the slow encoder probe; an actual change updates the Advanced page and re-arms the replay buffer to avoid mixed SDR/HDR segments. `FramePumpService::startPump` separately re-reads the monitor the capture target sits on, because a game can be on a different display than the one probed at startup. All lines are logged with the `Hdr:` / `FramePump:` prefixes and are included in the copied diagnostic summary.
