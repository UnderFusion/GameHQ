#include "input/PerfTrace.h"

#include <QByteArray>
#include <QDebug>
#include <QElapsedTimer>
#include <QHash>

#include <limits>
#include <mutex>

namespace {
constexpr qint64 kQuietMs = 5000;   // at most one line per site per 5 s

std::mutex s_mutex;
QElapsedTimer s_clock;
QHash<QByteArray, qint64> s_lastReportMs;
}

void PerfTrace::reportSlow(const char* site, qint64 elapsedUs, qint64 thresholdUs)
{
    if (elapsedUs < thresholdUs)
        return;

    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_clock.isValid())
        s_clock.start();
    const qint64 now = s_clock.elapsed();
    const QByteArray key(site);
    const qint64 last = s_lastReportMs.value(key, std::numeric_limits<qint64>::min() / 2);
    if (now - last < kQuietMs)
        return;
    s_lastReportMs.insert(key, now);

    qWarning().noquote() << QStringLiteral("Perf: %1 took %2 ms")
                                .arg(QLatin1String(site))
                                .arg(elapsedUs / 1000.0, 0, 'f', 1);
}
