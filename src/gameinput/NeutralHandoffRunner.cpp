#include "gameinput/NeutralHandoffRunner.h"

#include "gameinput/GameInputFocusPolicy.h"
#include "input/InputDiagnostics.h"

#include <QDebug>
#include <QTimer>

namespace ModernInput
{

NeutralHandoffRunner::NeutralHandoffRunner(QObject* parent)
    : QObject(parent)
{
}

void NeutralHandoffRunner::setSource(NeutralHandoffSource* source)
{
    m_source = source;
}

void NeutralHandoffRunner::setPolicySink(GameInputFocusRequestSink* sink)
{
    m_sink = sink;
}

void NeutralHandoffRunner::setBoundForTesting(int timeoutMs, int tickMs)
{
    m_timeoutMs = timeoutMs > 0 ? timeoutMs : kNeutralHandoffTimeoutMs;
    m_tickMs = tickMs > 0 ? tickMs : kNeutralHandoffTickMs;
}

QString NeutralHandoffRunner::receipt() const
{
    return QStringLiteral("neutral-handoff=") + receiptText();
}

QString NeutralHandoffRunner::receiptText() const
{
    // The stage decides what the detail means: a timeout prints what was still
    // held, every other stage prints the fact that stopped the wait (or refused
    // it in the first place).
    const QString detail = (m_outcome.stage == QLatin1String("timeout")
                            || m_outcome.stage == QLatin1String("passed"))
        ? m_outcome.heldDetail
        : m_outcome.reason;
    return neutralHandoffReceipt(m_outcome.stage, m_outcome.durationMs, m_outcome.polls, detail);
}

void NeutralHandoffRunner::noteSkipped(const QString& reason)
{
    if (m_running)
        return;   // a wait in flight never turns into a skip
    m_outcome = Outcome{};
    m_outcome.stage = QStringLiteral("not-engaged");
    m_outcome.reason = reason;
    InputDiagnostics::instance().noteGameInputHandoff(receipt());
}

void NeutralHandoffRunner::begin(const QString& releaseReason)
{
    if (m_running) {
        // A second close request (a repeated hotkey, the foreground event, the
        // context watch) must not restart the wait or release twice.
        qInfo().noquote()
            << QStringLiteral("Neutral handoff: already waiting, ignoring the repeated request");
        return;
    }

    m_releaseReason = releaseReason;
    m_outcome = Outcome{};
    m_outcome.releaseReason = releaseReason;
    m_polls = 0;

    // The wait only exists when there is a policy to defer AND a way to observe
    // the pad. Both refusals are reported, never implied: a close that waited
    // blind would either delay for nothing or claim a handoff that never ran.
    if (!m_sink || !m_sink->exclusiveForegroundActive()) {
        noteSkipped(QStringLiteral("the exclusive policy was not in force"));
        emit finished();
        return;
    }
    if (!m_source) {
        noteSkipped(QStringLiteral("no neutral-state source in this session"));
        emit finished();
        return;
    }

    m_running = true;
    m_outcome.attempted = true;
    m_outcome.stage = QStringLiteral("waiting");
    m_outcome.reason = releaseReason;
    InputDiagnostics::instance().noteGameInputHandoff(receipt());
    qInfo().noquote()
        << QStringLiteral("Overlay close: waiting for the controller to reach neutral (%1)")
               .arg(releaseReason);

    m_source->setOverlayReleaseActive(true);
    m_clock.start();

    // First evaluation immediately: an already-neutral pad closes with zero
    // added latency instead of waiting for the first tick.
    poll();
}

void NeutralHandoffRunner::poll()
{
    if (!m_running)
        return;

    // Another path can release the policy while we wait (the foreground probe
    // sees the overlay lose the foreground, the runtime stops). There is then
    // nothing left to defer: say so instead of waiting out a bound that no
    // longer protects anything, and never release a second time.
    if (!m_sink || !m_sink->exclusiveForegroundActive()) {
        finish(QStringLiteral("released-elsewhere"),
               QStringLiteral("the policy was released while waiting"), NeutralHandoffSample{});
        return;
    }

    ++m_polls;
    const NeutralHandoffSample sample = m_source->sampleNeutralPadState();
    const NeutralHandoffDecision decision = decideNeutralHandoff(sample);

    if (decision.neutral) {
        finish(QStringLiteral("passed"), decision.reason, sample);
        return;
    }
    if (m_clock.elapsed() >= m_timeoutMs) {
        // Honest failure: the pad never reached neutral inside the bound. The
        // close still completes — the timeout cannot invent a release event.
        finish(QStringLiteral("timeout"), decision.reason, sample);
        return;
    }

    if (!m_timer) {
        m_timer = new QTimer(this);
        m_timer->setTimerType(Qt::PreciseTimer);
        connect(m_timer, &QTimer::timeout, this, &NeutralHandoffRunner::poll);
    }
    m_timer->start(m_tickMs);
}

void NeutralHandoffRunner::finish(const QString& stage, const QString& reason,
                                  const NeutralHandoffSample& sample)
{
    m_running = false;
    if (m_timer)
        m_timer->stop();

    m_outcome.stage = stage;
    m_outcome.reason = reason;
    m_outcome.durationMs = int(m_clock.isValid() ? m_clock.elapsed() : 0);
    m_outcome.polls = m_polls;
    m_outcome.neutralPassed = (stage == QLatin1String("passed"));
    m_outcome.timedOut = (stage == QLatin1String("timeout"));
    m_outcome.heldDetail = sample.heldSummary;

    // The policy goes back BEFORE the close continues. This order is the whole
    // point of the unit: a held pad must never be exposed by an early release,
    // and a neutral pad must not be kept exclusive one moment longer.
    if (m_sink && m_sink->exclusiveForegroundActive()) {
        const int before = m_sink->focusTransitionCount();
        m_sink->restoreBackground(m_releaseReason);
        m_outcome.releaseTransitions = m_sink->focusTransitionCount() - before;
        m_outcome.released = m_outcome.releaseTransitions > 0;
    }
    if (m_source)
        m_source->setOverlayReleaseActive(false);

    InputDiagnostics::instance().noteGameInputHandoff(receipt());
    qInfo().noquote() << QStringLiteral("Overlay close: %1").arg(receipt());

    emit finished();
}

void NeutralHandoffRunner::cancel()
{
    if (!m_running)
        return;
    m_running = false;
    if (m_timer)
        m_timer->stop();
    if (m_source)
        m_source->setOverlayReleaseActive(false);

    m_outcome = Outcome{};
    m_outcome.stage = QStringLiteral("cancelled");
    m_outcome.reason = QStringLiteral("the close was abandoned before the pad reached neutral");
    m_outcome.durationMs = int(m_clock.isValid() ? m_clock.elapsed() : 0);
    m_outcome.polls = m_polls;
    InputDiagnostics::instance().noteGameInputHandoff(receipt());
    // No finished() on purpose: the owner is going away, and no policy release
    // happens here either — see the header.
}

}  // namespace ModernInput
