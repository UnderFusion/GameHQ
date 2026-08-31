#include "input/EventLoopStallMonitor.h"

#include <QDebug>
#include <QTimer>

namespace {
constexpr int kBeatMs = 250;
// A beat this late means the main thread stopped serving events long enough
// to be felt: 16 ms is one lost 60 Hz frame, 30–80 ms is the "mouse hitches"
// range users actually report. 50 ms catches those without logging every
// dropped frame; the rate limit keeps a chronically busy loop to one line
// per quiet window.
constexpr qint64 kStallThresholdMs = 50;
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
