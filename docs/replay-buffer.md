# Replay Buffer

> Milestone 0.5. PlayStation-style "save the last 5 minutes" without storing raw frames in RAM.

## Design

Continuously encode **5-second segments** to `gamehq-data/replay-cache/<Game>/<media-format>/` (fragmented MP4). The media-format folder fingerprints dimensions/FPS and audio sample rate/channels, preventing incompatible segments from being restored together after a setting or device change. Buffer length is `replay.length_seconds`; ring size = `ceil(length_seconds / replay.segment_seconds)` - e.g. a 5-minute buffer = 60 x 5 s segments. The writer deletes the oldest as it rolls. Segment filenames include milliseconds so a save-time close/reopen cannot reuse the same path.

```txt
Share held 2 s -> freeze ring -> snapshot the last N segments
-> remux-concat into final MP4 (no re-encode)
-> write to <ClipsRoot>/<Game>/Clips/ -> DB insert -> thumbnail
-> replay_saved sound -> notification -> gallery refresh
```

## Why segments

Low RAM, crash-resistant fMP4 segments, trivial cleanup, replay length equals segment count, export is fast remux.

## Config

- `replay.length_seconds` - buffer length; allowed set `{30, 60, 180, 300, 600, 900}` (30 s / 1 m / 3 m / 5 m / 10 m / 15 m), default `300`. Exposed as the **REPLAY** dropdown in Settings.
- `replay.segment_seconds` - internal ring granularity, default `5` (not in the UI).
- `replay.fps` - target frame rate, `30` (default) or `60`. Exposed as the **Frame rate** dropdown in Settings.
- `replay.resolution` - encode cap, `1280x720` / `1920x1080` (default) / `3840x2160`. Exposed as the **Resolution** dropdown in Settings (720p / 1080p / 4K). Captured frames are fed to Media Foundation with the full source size and this cap as the output size, so conversion/scaling is handled by the sink-writer pipeline instead of a manual CPU loop.
- `replay.bitrate_mbps` (`14`, not in the UI).
- `SettingsApplyPolicy` is the single restart classifier used by individual changes, individual resets, group resets and reset-all. Unknown forward-compatible keys default to live, while a completeness test requires every built-in default to be listed explicitly.
- `FramePumpService::recordingStateChanged(active, gameName)` drives `AppController::replayBufferActive`/`replayBufferGame`, shown as a live "Buffer state" row in Replay Settings.
- `audio.enabled` controls replay AAC capture. When enabled, WASAPI desktop loopback is attached and the audio format becomes part of the cache fingerprint. Audio samples are re-expressed on the video clock (shared QPC epoch) before encoding so the AAC track stays aligned with the frames. If Windows invalidates the render endpoint during an HDR/display transition, the incomplete segment is discarded and the capture pipeline automatically re-arms.

### Settings application policy

| Application | Keys | Effect |
|---|---|---|
| Restart replay buffer | `replay.resolution`, `replay.fps`, `replay.bitrate_mbps`, `replay.length_seconds`, `replay.segment_seconds`, `replay.auto`, `audio.enabled` | A running buffer re-arms once with the new recording parameters. |
| Apply live | `replay.clip_sound`, `replay.clip_notify`, `replay.manual_idle_s` | Feedback changes immediately; the manual idle deadline uses the value on its next evaluation. |
| Apply live | All `capture.*` keys | Screenshot options affect the next screenshot. `capture.hide_border` affects the next WGC capture session without restarting the current session or buffer. |
| Apply live | All `sounds.*`, `notifications.*`, `storage.*`, `startup.*`, `tray.*`, `input.*`, `ui.*`, `theme.*` and `updates.*` keys | The owning service or next operation reads the new value without rebuilding the replay pipeline. |

A mixed Replay group reset requests at most one buffer restart. Resetting only
live Replay keys, or the Sounds, Capture or Notifications groups, requests none.

## Timing model

### Confirmed readiness

`FramePumpService` uses `ReplayBufferState` as its GUI-thread state authority.
Every start/stop command advances a generation; worker confirmations, failures,
restart requests and screenshot/update cleanup cannot overwrite a newer command.
The existing `recordingStateChanged(bool, game)` signal remains compatible: it is
true only in `Recording` or `Ready`. `app.replayBufferState` exposes all five
states through the QML `ReplayBufferState` enum; `app.replayBufferActive` is
derived from that same confirmed state.

| State | Meaning | Save request |
|---|---|---|
| `Stopped` | No recording requested | Request a start, then reject this save explicitly |
| `Starting` | Worker startup has not yet succeeded | Reject and ask the user to retry shortly |
| `Recording` | Capture session started and replay recorder is active | Reject until closed media exists |
| `Ready` | The current recorder has finalized a segment containing video | Queue the save with the current generation |
| `Failed` | Startup or the active recording failed | Request a fresh start, then reject this save explicitly |

No save waits silently in a queue for readiness. Worker-side save guards also
check the generation and recorder facts before taking a snapshot. Restored cache
filenames alone are not proof of usable media: a re-arm stays `Recording` until
the current recorder successfully finalizes video. A separate 250 ms worker
timer observes these facts and recorder reopen failure, keeping readiness work
out of the frame-arrival/encoding loop. Optional audio/HDR fallback behavior is
unchanged; a capture-only session with a failed replay recorder reports `Failed`
for replay, while it can still supply an HDR screenshot.

`tst_framepumpservice` covers the shared state authority with deterministic
worker facts: successful progression, startup failure, stale generations,
save-before-ready rejection and the UI enum. Real-game launch/log checks remain
part of the p2 group milestone.

### Session ownership

`ReplayBufferOwners` keeps `{Auto, ManualSave, HdrScreenshot}` independently of
pump generations. Ordinary stop requests tear down the buffer only when this
set is empty. A settings-driven re-arm replaces the pipeline on its previously
owned game window with the latest settings, while retaining owner tokens and
any export's path lease; focusing Settings does not retarget a manual session.

| Owner | Acquired | Released |
|---|---|---|
| `Auto` | Always-on enabled with an eligible foreground game | Always-on disabled, or two foreground polls without an eligible game |
| `ManualSave` | An explicit save request, including a cold arm | That dispatched save succeeds or fails; a cold arm instead expires on idle |
| `HdrScreenshot` | An accepted HDR screenshot request | That screenshot succeeds or fails, including re-arm cancellation |

Cold-arm timeout is `replay.manual_idle_s` (default 90 seconds, clamped to
10?600 seconds), measured with a monotonic clock and checked every 1.5 seconds.
A follow-up request before readiness refreshes the deadline. Once a save is
dispatched, idle expiry cannot release it. A repeated save while one is pending
is rejected without releasing its owner. Worker preflight failures and export
completion both acknowledge the exact request token; older completions cannot
release a newer manual session.

An HDR screenshot of the same window shares the session. A different HDR
target is rejected while manual replay owns the current window. Foreground
auto-tracking does not replace a manually/HDR-owned target. Ending either
explicit operation leaves the other owner and any `Auto` owner intact. Logs
use `ReplayOwner[id]` and `ReplaySave[id]` to correlate ownership with exports.
An update explicitly cancels a cold arm and drains any dispatched export before
shutdown; application exit retains the export grace period and continued join.

The focused state tests cover cold arm ? Ready ? save, re-arm with manual
ownership, stale completion tokens, HDR overlap, terminal save completion,
and idle expiry without releasing other owners.

### Media clocks

- **Video PTS**: schedule-anchored throttle on the capture clock; each segment's first frame is rebased to exactly t=0 (IDR), export concat advances the timeline by each segment's max sample end.
- **Audio PTS**: a continuous **sample-count clock** — anchored on the first packet after the first video frame, advanced by `numFrames/rate` per packet, re-anchored only on >0.5 s wall-clock drift. Never derived from poll timestamps: after any pump stall the drained packet burst carries near-identical poll times, which previously caused ~350 ms audio holes at every segment boundary (clips visibly "pausing" every 5 s).
- **Segment rolls**: the next segment's sink writer is pre-built on a side thread and adopted at roll time (`segment roll took N ms (pre-built)` in the log; ~30 ms vs ~300 ms inline). Diagnose clips with `python tools/analyze_mp4_timing.py <clip.mp4> [gap_ms]`.

## Performance model

- **GPU downscale**: when `replay.resolution` is below the source size, a D3D11 VideoProcessor scales each frame on the GPU before readback, so the CPU path (staging copy, row memcpy, MF color convert) runs at encode size — ~4× cheaper for 4K→1080p. Automatic fallback to full-size readback when the video processor is unavailable (`SegmentRecorder: GPU downscale active`/`CPU scaling` in the log).
- **Async export**: Share-hold finalizes the current segment and returns a `SegmentLease` for the newest replay window. Remux + final thumbnail run on an owned `ReplayExportTask`; only one export runs at a time. The task releases its lease on return or exception, before `finished`, independently of worker callbacks. Failed output reservations are also discarded inside the export task.
- **Lease-aware pruning**: a process-wide registry counts leases by normalized absolute path (case-insensitive on Windows). Restore, stale-cache sweep and ring trimming cannot delete leased paths. The ring retains the newest configured window plus any older leased paths; an old lease does not prevent deleting later expired, unleased segments. A game switch, buffer restart or replacement recorder does not change export ownership. Completion only updates worker/UI state and opportunistically trims the current recorder.
- **Shutdown**: the worker owns and joins the export thread before apartment shutdown and destruction. A five-second grace timeout logs a warning and continues waiting; it never destroys a running thread or abandons the export. Shutdown can therefore exceed five seconds if the muxer is slow or stuck. Export work captures value inputs and shared result state, with no pipeline/recorder/worker pointers. A generation fence ignores obsolete completion callbacks; neither lease nor reservation cleanup needs the event loop.
- **Stale-cache sweep (Step 9)**: on worker start, `replay-cache/` is swept for `*_clip.mp4` older than 10 minutes (matching the ring-restore threshold) and emptied per-game folders are pruned.

## Status

Implemented: rolling H.264 ring in per-game format-fingerprinted cache folders (`SegmentRecorder`, Step 5 deletion by `length_seconds`); **Share-hold / Ctrl+Shift+E** save -> `ReplayExporter` remux-concat (no re-encode) -> one MP4 in the current `<ClipsRoot>/<Game>/Clips/` + video thumbnail + DB (`type="video"`) + sound + notification (Steps 6/8). `CaptureLocations` resolves the separate clip root at each save, so Settings changes do not require re-arming the ring. dev.79 re-enables audio when `audio.enabled=true` and resets audio timestamps on segment roll/snapshot. Endpoint invalidation now drops the incomplete segment and automatically rebuilds capture. dev.76 also makes export all-or-nothing: unreadable/skipped segments or writer failures produce an explicit failed-save notification instead of a partial short clip.

Debugging: every save attempt writes a correlated `ReplaySave[...]` block to `gamehq.log`, including ring snapshot paths/sizes, thumbnail timings, remux segment media metadata, per-segment video/audio sample counts, output bytes, and final success/failure timing.

Focused regression coverage: `tst_segmentrecorder` exercises the actual recorder restore/trim code with file fixtures and the export task with controlled work: stale leased files during replacement/re-arm, overlapping leases, continued unrelated trimming, complete snapshot reads after recorder replacement, callback-independent return/exception cleanup, and joining beyond the shutdown grace period. It does not execute the encoder/muxer or replace real game/audio acceptance. Stale-cache cleanup runs on worker startup; shutdown joins any active export.

## Rules

- Buffer runs only per capture mode ("only in games" default); paused means tray status shows why.
- Segment ring is temp data. Recent per-game segments may be restored after focus churn so a brief disarm does not erase the replay window; stale cleanup remains Step 9.
- A save must never trigger the screenshot action (tap/hold exclusivity; see [controller-input.md](controller-input.md)).
- Defaults: 1080p30, H.264 12-16 Mbps, audio controlled by `audio.enabled`, 5 min.
