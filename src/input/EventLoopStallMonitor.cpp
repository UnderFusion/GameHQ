#include "input/EventLoopStallMonitor.h"

#include <QDebug>
#include <QTimer>

namespace {
constexpr int kBeatMs = 250;
// A beat this late means the main thread stopped serving events long enough
// to be felt — Windows' low-level-hook timeout budget is a few hundred ms,
// so 100 ms is where main-thread stalls start turning into visible jank.
constexpr qint64 kStallThresholdMs = 100;
constexpr qint64 kReportQuietMs = 2000;
}

EventLoopStallMonitor::EventLoopStallMonitor(QObject* parent)
    : QObject(parent)
{
    auto* timer = new QTimer(this);
    timer->setTimerType(Qt::CoarseTimer);
    timer->setInterval(kBeatMs);
    connect(timer, &QTimer::timeout, this, &EventLoopStallMonitor::beat);
    m_sinceLastBeat.start();
    timer->start();
}

void EventLoopStallMonitor::beat()
{
    const qint64 late = m_sinceLastBeat.restart() - kBeatMs;
    if (late < kStallThresholdMs)
        return;
    if (late > m_worstStallMs)
        m_worstStallMs = late;
    if (m_sinceLastReport.isValid() && m_sinceLastReport.elapsed() < kReportQuietMs)
        return;
    m_sinceLastReport.start();
    qWarning().noquote() << QStringLiteral("Perf: GUI event loop stalled ~%1 ms (worst %2 ms)")
                                .arg(late)
                                .arg(m_worstStallMs);
}
