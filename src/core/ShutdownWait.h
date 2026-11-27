#pragma once

#include <QDebug>
#include <QElapsedTimer>

// Shutdown waits for work that cannot be interrupted safely: an encode that is
// writing a capture, a Media Foundation call, a driver read. Abandoning such a
// worker (terminate, or delete what it still uses) trades a slow exit for a
// crash or a lost file, so these waits never give up. What they must not do is
// stall silently: past the grace period the log says which dependency is
// holding the exit, keeps saying so, and records how long it took in the end.
// See docs/threading-and-shutdown.md.
namespace ShutdownWait
{
inline constexpr int kDefaultGraceMs = 2000;
inline constexpr int kReportEveryMs = 5000;

// timedWait(ms) waits at most ms and returns true once the work is done.
// Returns true when it finished inside the grace period; either way it has
// finished when this returns.
template <typename TimedWait>
bool untilDone(const char* dependency, TimedWait timedWait, int graceMs = kDefaultGraceMs)
{
    QElapsedTimer elapsed;
    elapsed.start();
    if (timedWait(graceMs))
        return true;
    qWarning().nospace() << "Shutdown: " << dependency << " still running after "
                         << elapsed.elapsed() << " ms; waiting for it to finish (not interruptible)";
    while (!timedWait(kReportEveryMs)) {
        qWarning().nospace() << "Shutdown: still waiting for " << dependency << " ("
                             << elapsed.elapsed() << " ms)";
    }
    qWarning().nospace() << "Shutdown: " << dependency << " finished after "
                         << elapsed.elapsed() << " ms";
    return false;
}
} // namespace ShutdownWait
