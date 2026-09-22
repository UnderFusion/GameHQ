# GameHQ 0.7.6 (2026-08-31)

> This document is the original **English (en-US)** release note. No reviewed Turkish translation exists for this version, so the complete English text is published unchanged.

## Fixed

- Fixed an input path that could cause system-wide mouse stuttering or brief freezes while GameHQ was running. Global Mouse Back/Forward/Middle monitoring is now installed only while a mouse binding exists or mouse-binding capture requires it.
- When global mouse monitoring is required, the Windows low-level mouse hook now runs on a dedicated worker thread instead of the interface thread. Hook start/stop and rapid-restart handling were also hardened so held buttons are released cleanly and stale queued input cannot survive across hook lifetimes.
- WinMM joystick discovery no longer performs potentially slow device scans on the interface thread, preventing periodic GameHQ interface stalls on systems where discovery is slow.

## Diagnostics / Reliability

- Added lightweight, rate-limited performance diagnostics for slow XInput and WinMM operations and interface event-loop stalls. Any remaining hitch should now leave useful Perf: evidence in gamehq.log.
