#pragma once
#include <QElapsedTimer>
#include <QObject>

// Cheap GUI event-loop stall detector (GitHub stutter report follow-up). A
// coarse heartbeat timer measures its own lateness: a beat arriving well
// after its interval means the main thread was busy or blocked that long,
// and everything it services — queued events, timers, and formerly the
// WH_MOUSE_LL mouse hook — waited too. Stalls are logged with the same
// "Perf:" prefix as the slow-poll reports (PerfTrace) so one log shows
// whether a mouse hitch, a slow backend call and an event-loop stall line
// up in time. Costs one timer wakeup per beat and nothing else.
class EventLoopStallMonitor : public QObject
{
    Q_OBJECT
public:
    explicit EventLoopStallMonitor(QObject* parent = nullptr);

private:
    void beat();

    QElapsedTimer m_sinceLastBeat;
    QElapsedTimer m_sinceLastReport;   // rate limit; invalid until first report
    qint64 m_worstStallMs = 0;
};
