#pragma once
#include <QtGlobal>

// Rate-limited slow-call reporting for the input backends (GitHub stutter
// report follow-up). A poll or rescan pass that blocks the GUI thread for
// milliseconds is invisible in normal logs yet is exactly what makes the
// event loop — and everything serviced by it — sluggish. Call sites report
// every pass; only passes above the threshold are logged, at most one line
// per site per quiet window, so a chronically slow driver cannot flood the
// log. Lines carry the "Perf:" prefix so a reporter's log correlates them
// with the event-loop stall reports (EventLoopStallMonitor).
namespace PerfTrace {

// 2 ms: an order of magnitude above a healthy XInputGetState/joyGetPosEx
// call, low enough to catch the multi-ms driver stalls that matter.
inline constexpr qint64 kSlowCallUs = 2000;

void reportSlow(const char* site, qint64 elapsedUs, qint64 thresholdUs = kSlowCallUs);

} // namespace PerfTrace
