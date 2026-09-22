#pragma once

#include "gameinput/NeutralHandoff.h"

#include <QElapsedTimer>
#include <QObject>
#include <QString>

class QTimer;

namespace ModernInput
{

class GameInputFocusRequestSink;

// cpo-o06e: the ONE owner of the release transition, and the order it enforces.
//
//   1. the close enters ReleasingInput and the overlay input path quiesces;
//   2. the exclusive policy STAYS in force while the pad is still held — the
//      obvious order (release first, then wait) would expose exactly the held
//      state the wait exists to hide;
//   3. once the pad is neutral, or the bound expires, the policy is restored
//      BEFORE anything else happens;
//   4. the owner then hands the foreground back and completes the close.
//
// Bounded and asynchronous: a QTimer drives the polls, there is no wait loop and
// no thread of its own, so the GUI thread is never blocked and the close can
// never end in an indefinite "release the button to get back to the game" state.
// A pad that never reaches neutral ends in an honest timeout receipt — never in
// a synthesized release event, and never labelled a clean handoff.
//
// An already-neutral pad completes inside begin(), synchronously: the normal
// close (nothing held) pays no latency and no event-loop turn.
class NeutralHandoffRunner : public QObject
{
    Q_OBJECT

public:
    struct Outcome
    {
        bool attempted = false;       // the bounded wait actually ran
        bool neutralPassed = false;   // ... and it observed a neutral pad in time
        bool timedOut = false;
        bool released = false;        // this runner restored the background policy
        int durationMs = 0;
        int polls = 0;
        int releaseTransitions = 0;   // policy applications the release caused
        QString stage;                // waiting / passed / timeout / not-engaged / ...
        QString reason;               // why it stopped, or why it was not engaged
        QString releaseReason;        // the reason the policy was restored with
        QString heldDetail;           // held controls at the last poll, bounded
    };

    explicit NeutralHandoffRunner(QObject* parent = nullptr);

    // Non-owning. The source is the input layer, the sink is the process-wide
    // policy owner (GameInputFocusController). Either may be absent — an
    // unmeasurable handoff is reported as not engaged instead of being faked.
    void setSource(NeutralHandoffSource* source);
    void setPolicySink(GameInputFocusRequestSink* sink);
    bool hasSource() const { return m_source != nullptr; }

    // Starts the bounded wait. Emits finished() either synchronously (nothing to
    // wait for) or from the poll timer. A second begin() while waiting is
    // ignored: the policy is released exactly once per close.
    void begin(const QString& releaseReason);
    // Records the receipt for a close that does not need the wait at all.
    void noteSkipped(const QString& reason);
    // Abandons a wait (owner going away). Deliberately does NOT release the
    // policy: the session that is going away restores it itself (the controller
    // detach), and a cancelled close must not hand a held pad back mid-gesture.
    void cancel();

    bool running() const { return m_running; }
    Outcome outcome() const { return m_outcome; }
    // The same receipt the diagnostics timeline holds, for the owner's close
    // record: one formatter decides what the parenthesis means.
    QString receiptText() const;
    // Test seam: tighten the bound. Never used by production code.
    void setBoundForTesting(int timeoutMs, int tickMs);

signals:
    void finished();

private:
    void poll();
    void finish(const QString& stage, const QString& reason, const NeutralHandoffSample& sample);
    QString receipt() const;

    NeutralHandoffSource* m_source = nullptr;
    GameInputFocusRequestSink* m_sink = nullptr;
    QTimer* m_timer = nullptr;
    QElapsedTimer m_clock;
    bool m_running = false;
    int m_polls = 0;
    int m_timeoutMs = kNeutralHandoffTimeoutMs;
    int m_tickMs = kNeutralHandoffTickMs;
    QString m_releaseReason;
    Outcome m_outcome;
};

}  // namespace ModernInput
