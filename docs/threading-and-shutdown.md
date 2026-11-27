# Threads, ownership and shutdown

How GameHQ's workers are owned, how they are told to stop, and what the exit
waits for. The rule behind every entry: **a worker that cannot be interrupted
safely is waited for, never abandoned.** No `QThread::terminate`, no
"timeout then delete" of an object a worker still uses. A wait that outlives
its grace period is reported through `ShutdownWait::untilDone`
(`src/core/ShutdownWait.h`): after 2 s the log names the dependency holding
the exit, repeats every 5 s, and records the total once it finishes.

## Exit order

1. Tray *Quit* or `AppController::exitRequested` → `App::requestExit`
   (queued) → `FramePumpService::shutdownForExit`.
2. The capture worker stops the pump. If a replay export is running, it quits
   its thread only after the export's `saveRequestFinished`; otherwise at once.
   Only one export can exist at a time (`ReplaySaveCoordinator.cpp`, the
   `m_exportBusy` refusal), so the pending export always reports back.
3. `QThread::finished` → `FramePumpWorker::onThreadFinished` on the worker
   thread: join the export, tear the pipeline down (recorder, encoder, audio,
   WGC), then `MFShutdown` and `RoUninitialize`.
4. `FramePumpService::shutdownFinished` → `QCoreApplication::quit`. The event
   loop stays alive until here, so queued completions still run.
5. `~App` destroys members in reverse declaration order (`App.h`); each owner
   stops its own workers in its destructor, below.

## Workers

| Worker | Owner / thread | Stop request | Completion observed | Bound |
|---|---|---|---|---|
| Capture pipeline | `FramePumpService` → `FramePumpWorker` on its `QThread` | `shutdownForExit` (queued), interruption flag | `QThread::finished`; `~FramePumpService` waits via `ShutdownWait` | Waits for the steps below |
| Replay export | `ReplayExportTask` (`QThread`), joined by the worker | None: a remux is not cancellable | `QThread::finished` → queued result | 5 s grace, then keeps waiting (logged) |
| Segment writer prep | `SegmentRecorder::m_prepThread` (`std::thread`) | None | `join()` | One writer creation |
| Hardware encoder (MFT) | Capture worker thread | `close()` / `ShutdownObject` | Call returns | Driver-bound; not interruptible |
| Audio drain | `AudioCapture::m_drainThread` | `m_stopDrain` + `SetEvent` | `join()` | Loop wakes every 20 ms |
| Screenshot encodes | `ScreenshotService::m_encodePool` (2 threads) | None: a started write always completes | `waitForDone` via `ShutdownWait` | One encode per thread |
| Library scan | `CaptureScanner::m_pool` (1 thread) | `m_cancel` checked per file | `waitForDone` via `ShutdownWait` | One file (video: one MF decode) |
| Thumbnails | `ThumbnailScheduler::m_pool` (2 threads) | Waiting jobs dropped, `m_closing` | `waitForDone` via `ShutdownWait` | Two decodes |
| Sony HID reader | `SonyHidReader` worker | `stopEvent` → `CancelIoEx` | `doneEvent`, 500 ms | Abandonment handshake (below) |
| Sony PnP scan | `DualSenseDevice::m_pnpScanPool` | None | `waitForDone` via `ShutdownWait` | One CfgMgr scan |
| WinMM sampler | `WinMMDevice::m_worker` (`std::thread`) | `m_quit` + condition variable | `join()` | One `joyGetPosEx` call plus a scan |
| GameInput dispatcher | `GameInputQtDispatcher` (`std::thread`) | `stopAccepting()` wakes it | `join()`; at most one queued drain, dropped with the object | Immediate |
| Update download | `UpdateDownloader` on the GUI event loop | `abort` + `cancelled()` | Reply signals | No thread |
| Update helper | `UpdateInstaller` (detached process) | Helper protocol | Ready event | 15 s, then the install fails safely |

## The one bounded abandonment: Sony HID

`SonyHidReader` is the only worker GameHQ leaves running at shutdown, and it
does so safely. Its owner waits 500 ms for `doneEvent`. If the cancelled read
has not completed, the owner sets `abandoned` under `lifetimeMutex`, detaches
the thread and hands it ownership of its `Impl`. The worker frees `Impl` itself
once the kernel releases its buffer. Its callbacks check `abandoned` under the
same mutex, so nothing reaches the destroyed `DualSenseDevice`. The cost is a
leaked thread and handle if the driver never completes the read; it is
logged. Do not replace this with thread termination.

## Not interruptible (waited for, by design)

- A Media Foundation remux (`ReplayExporter::concat`), the first-frame video
  thumbnail decode, MFT encoder calls and `ShutdownObject`.
- A screenshot encode/write that has started.
- Driver calls: `joyGetPosEx`, CfgMgr PnP scans.

A stalled one holds the exit and says so in the log. If measurement ever shows
one of them hanging in practice, isolate it (a child process) rather than
abandon it in-process.

## Tests

- `tst_shutdownwait`: a wait returns only after the work finished and logs
  past the grace.
- `tst_thumbnailscheduler`: a 2.4 s decode at destruction is completed, not
  abandoned; waiting jobs are dropped.
- `tst_captureacknowledgement`: a 4K screenshot still encoding when the
  service is destroyed is written in full.
- `tst_capturefilesync`: a scan cancelled at shutdown stops within one file.
- `tst_inputengineshutdown`, `tst_gameinputdelivery`: input teardown order and
  the GameInput hand-off.
