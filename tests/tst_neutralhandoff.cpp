// cpo-o06e: the close's release transition, without a desktop session, a device
// or a game:
//
//   * the ONE definition of "the pad is neutral" (pure);
//   * the held-control tracker fed by raw edges (pure), including a device that
//     goes away taking its held controls with it;
//   * the bounded, asynchronous handoff runner: it keeps the exclusive policy in
//     force while the pad is held, releases it exactly once when neutral is
//     reached (or the bound expires), never blocks on anything, and records an
//     honest receipt for every outcome — including the timeout, which must never
//     be dressed up as a clean handoff.
//
// What this does NOT prove: that a real game receives no leaked action. That
// needs a device and a game and stays with the physical acceptance work; here
// the state machine and the order it enforces are pinned down.
#include "gameinput/GameInputFocusPolicy.h"
#include "gameinput/NeutralHandoff.h"
#include "gameinput/NeutralHandoffRunner.h"
#include "input/HeldControlTracker.h"
#include "input/InputDiagnostics.h"

#include <QSignalSpy>
#include <QTest>

namespace {

using ModernInput::GameInputFocusMode;
using ModernInput::NeutralHandoffRunner;
using ModernInput::NeutralHandoffSample;

// A focus-policy owner that behaves like the real controller: idempotent
// releases, counted transitions, and no policy change when nothing was engaged.
class CountingFocusSink final : public ModernInput::GameInputFocusRequestSink
{
public:
    bool requestExclusiveForeground(const QString&) override
    {
        if (m_exclusive)
            return true;
        m_exclusive = true;
        ++m_transitions;
        return true;
    }

    void restoreBackground(const QString& reason) override
    {
        if (!m_exclusive) {
            ++m_idleReleases;
            return;
        }
        m_exclusive = false;
        ++m_transitions;
        m_releases.append(reason);
    }

    GameInputFocusMode focusMode() const override
    {
        return m_exclusive ? GameInputFocusMode::ExclusiveForeground
                           : GameInputFocusMode::Background;
    }
    bool exclusiveForegroundActive() const override { return m_exclusive; }
    QString focusModeName() const override { return ModernInput::gameInputFocusModeName(focusMode()); }
    int focusTransitionCount() const override { return m_transitions; }
    bool policyAttached() const override { return true; }

    QStringList releases() const { return m_releases; }
    int idleReleases() const { return m_idleReleases; }

private:
    bool m_exclusive = false;
    int m_transitions = 0;
    int m_idleReleases = 0;
    QStringList m_releases;
};

// A scriptable pad: the test decides what is held, and the source records what
// the policy looked like at every poll — which is how the ORDER ("exclusive
// stays on while waiting") becomes an assertion instead of a comment.
class ScriptedSource final : public ModernInput::NeutralHandoffSource
{
public:
    void attachSink(ModernInput::GameInputFocusRequestSink* sink) { m_sink = sink; }

    NeutralHandoffSample sampleNeutralPadState() const override
    {
        ++m_samples;
        if (m_sink)
            m_exclusiveAtSample.append(m_sink->exclusiveForegroundActive());
        return m_sample;
    }

    void setOverlayReleaseActive(bool active) override
    {
        m_quiescedCalls.append(active);
        m_sample.overlayActionsQuiesced = active;
    }

    void setSample(int heldControls, const QString& summary, int devices = 1)
    {
        m_sample.heldControls = heldControls;
        m_sample.heldSummary = summary;
        m_sample.attachedDevices = devices;
    }

    int samples() const { return m_samples; }
    QVector<bool> exclusiveAtSample() const { return m_exclusiveAtSample; }
    QVector<bool> quiescedCalls() const { return m_quiescedCalls; }
    // The input path must go quiet for the wait and come back exactly once.
    bool quiescedOnThenOff() const
    {
        return m_quiescedCalls.size() == 2 && m_quiescedCalls.at(0) && !m_quiescedCalls.at(1);
    }

private:
    mutable NeutralHandoffSample m_sample;
    ModernInput::GameInputFocusRequestSink* m_sink = nullptr;
    mutable int m_samples = 0;
    mutable QVector<bool> m_exclusiveAtSample;
    QVector<bool> m_quiescedCalls;
};

}  // namespace

class NeutralHandoffTest : public QObject
{
    Q_OBJECT

private slots:
    void init();

    void neutralIsOneDefinition();
    void theReceiptVocabularyIsStable();

    void trackerKeepsRawEdgesPerDevice();
    void aDeviceThatGoesAwayCannotHoldAnything();

    void aCloseWithNothingToDeferDoesNotWait();
    void aNeutralPadClosesWithoutWaiting();
    void aHeldPadKeepsThePolicyUntilItIsReleased();
    void aPadThatNeverReleasesEndsInAnHonestTimeout();
    void aWaitStopsWhenAnotherPathRestoresThePolicy();
    void aRepeatedBeginNeverRestartsTheWait();
    void cancelStopsTheWaitWithoutReleasingThePolicy();
};

void NeutralHandoffTest::init()
{
    InputDiagnostics::instance().clear();
}

// --- the decision -----------------------------------------------------------

void NeutralHandoffTest::neutralIsOneDefinition()
{
    using ModernInput::decideNeutralHandoff;

    NeutralHandoffSample sample;
    sample.overlayActionsQuiesced = false;
    QVERIFY(!decideNeutralHandoff(sample).neutral);

    // Quiesced, one control held: not neutral, and the reason names it — the
    // receipt has to say what the close was waiting for.
    sample.overlayActionsQuiesced = true;
    sample.attachedDevices = 1;
    sample.heldControls = 1;
    sample.heldSummary = QStringLiteral("gamepad.cross");
    const ModernInput::NeutralHandoffDecision held = decideNeutralHandoff(sample);
    QVERIFY(!held.neutral);
    QVERIFY2(held.reason.contains(QStringLiteral("gamepad.cross")), qPrintable(held.reason));

    // Several controls held: the summary is what a report reads.
    sample.heldControls = 3;
    sample.heldSummary = QStringLiteral("gamepad.cross, gamepad.l1, gamepad.dpad_up");
    QVERIFY(!decideNeutralHandoff(sample).neutral);

    // Released: neutral, and the device presence is only a wording difference.
    sample.heldControls = 0;
    sample.heldSummary.clear();
    const ModernInput::NeutralHandoffDecision released = decideNeutralHandoff(sample);
    QVERIFY(released.neutral);
    QCOMPARE(released.reason, QStringLiteral("all controls released"));

    // No device at all counts as neutral: nothing can carry a held state.
    sample.attachedDevices = 0;
    const ModernInput::NeutralHandoffDecision absent = decideNeutralHandoff(sample);
    QVERIFY(absent.neutral);
    QCOMPARE(absent.reason, QStringLiteral("no controller attached"));
}

void NeutralHandoffTest::theReceiptVocabularyIsStable()
{
    QCOMPARE(ModernInput::neutralHandoffReceipt(QStringLiteral("passed"), 12, 2, QString()),
             QStringLiteral("passed duration_ms=12 polls=2"));
    QCOMPARE(ModernInput::neutralHandoffReceipt(QStringLiteral("timeout"), 402, 41,
                                                QStringLiteral("gamepad.cross")),
             QStringLiteral("timeout duration_ms=402 polls=41 held=\"gamepad.cross\""));
    QCOMPARE(ModernInput::neutralHandoffReceipt(QStringLiteral("not-engaged"), 0, 0,
                                                QStringLiteral("the exclusive policy was not in force")),
             QStringLiteral("not-engaged (the exclusive policy was not in force)"));
    QCOMPARE(ModernInput::neutralHandoffReceipt(QStringLiteral("waiting"), 0, 0,
                                                QStringLiteral("the overlay closed")),
             QStringLiteral("waiting (the overlay closed)"));
    // A passed handoff never prints a held list: it is always empty by then.
    QCOMPARE(ModernInput::neutralHandoffReceipt(QStringLiteral("passed"), 3, 1,
                                                QStringLiteral("stale")),
             QStringLiteral("passed duration_ms=3 polls=1"));
}

// --- the tracker ------------------------------------------------------------

void NeutralHandoffTest::trackerKeepsRawEdgesPerDevice()
{
    HeldControlTracker tracker;
    QCOMPARE(tracker.presentDevices(), 0);
    QCOMPARE(tracker.heldCount(), 0);

    tracker.noteDevicePresent(QStringLiteral("pad:a"));
    tracker.noteDevicePresent(QStringLiteral("pad:b"));
    QCOMPARE(tracker.presentDevices(), 2);
    QVERIFY(!tracker.anyHeld());

    tracker.notePressed(QStringLiteral("pad:a"), QStringLiteral("gamepad.cross"));
    tracker.notePressed(QStringLiteral("pad:a"), QStringLiteral("gamepad.dpad_up"));
    tracker.notePressed(QStringLiteral("pad:b"), QStringLiteral("gamepad.l2"));
    QCOMPARE(tracker.heldCount(), 3);
    // The summary is deterministic and bounded.
    QCOMPARE(tracker.heldControls(), QStringList({QStringLiteral("gamepad.cross"),
                                                  QStringLiteral("gamepad.dpad_up"),
                                                  QStringLiteral("gamepad.l2")}));
    QCOMPARE(tracker.heldControls(2), QStringList({QStringLiteral("gamepad.cross"),
                                                   QStringLiteral("gamepad.dpad_up"),
                                                   QStringLiteral("+1 more")}));

    // A duplicate press (a mirror API repeating the edge) is not a second hold.
    tracker.notePressed(QStringLiteral("pad:a"), QStringLiteral("gamepad.cross"));
    QCOMPARE(tracker.heldCount(), 3);

    // A release for something not held changes nothing.
    tracker.noteReleased(QStringLiteral("pad:a"), QStringLiteral("gamepad.square"));
    QCOMPARE(tracker.heldCount(), 3);

    tracker.noteReleased(QStringLiteral("pad:a"), QStringLiteral("gamepad.cross"));
    tracker.noteReleased(QStringLiteral("pad:b"), QStringLiteral("gamepad.l2"));
    QCOMPARE(tracker.heldCount(), 1);
    tracker.noteReleased(QStringLiteral("pad:a"), QStringLiteral("gamepad.dpad_up"));
    QVERIFY(!tracker.anyHeld());
    QCOMPARE(tracker.presentDevices(), 2);

    tracker.reset();
    QCOMPARE(tracker.presentDevices(), 0);
}

void NeutralHandoffTest::aDeviceThatGoesAwayCannotHoldAnything()
{
    HeldControlTracker tracker;
    tracker.noteDevicePresent(QStringLiteral("pad:a"));
    tracker.notePressed(QStringLiteral("pad:a"), QStringLiteral("gamepad.cross"));
    QCOMPARE(tracker.heldCount(), 1);

    // The controller disappears mid-hold: it cannot deliver the release any
    // more, so waiting for one would be waiting for a device that is gone.
    tracker.noteDeviceGone(QStringLiteral("pad:a"));
    QVERIFY(!tracker.anyHeld());
    QCOMPARE(tracker.presentDevices(), 0);

    NeutralHandoffSample sample;
    sample.overlayActionsQuiesced = true;
    sample.attachedDevices = tracker.presentDevices();
    sample.heldControls = tracker.heldCount();
    const ModernInput::NeutralHandoffDecision decision = ModernInput::decideNeutralHandoff(sample);
    QVERIFY(decision.neutral);
    QCOMPARE(decision.reason, QStringLiteral("no controller attached"));
}

// --- the runner -------------------------------------------------------------

void NeutralHandoffTest::aCloseWithNothingToDeferDoesNotWait()
{
    CountingFocusSink sink;
    ScriptedSource source;
    source.attachSink(&sink);
    source.setSample(0, QString());

    NeutralHandoffRunner runner;
    runner.setPolicySink(&sink);
    runner.setSource(&source);

    QSignalSpy finished(&runner, &NeutralHandoffRunner::finished);
    runner.begin(QStringLiteral("the overlay closed"));

    // Nothing was engaged: the close must not pretend it waited for anything.
    QCOMPARE(finished.count(), 1);
    const NeutralHandoffRunner::Outcome outcome = runner.outcome();
    QVERIFY(!outcome.attempted);
    QCOMPARE(outcome.stage, QStringLiteral("not-engaged"));
    QCOMPARE(outcome.reason, QStringLiteral("the exclusive policy was not in force"));
    QVERIFY(!runner.running());
    QVERIFY(!source.quiescedCalls().contains(true));
    QCOMPARE(source.samples(), 0);
    QVERIFY(sink.releases().isEmpty());
}

void NeutralHandoffTest::aNeutralPadClosesWithoutWaiting()
{
    CountingFocusSink sink;
    sink.requestExclusiveForeground(QStringLiteral("test"));
    ScriptedSource source;
    source.attachSink(&sink);
    source.setSample(0, QString());

    NeutralHandoffRunner runner;
    runner.setPolicySink(&sink);
    runner.setSource(&source);

    QSignalSpy finished(&runner, &NeutralHandoffRunner::finished);
    runner.begin(QStringLiteral("the overlay closed"));

    // Synchronous on purpose: an already-neutral pad adds no latency at all.
    QCOMPARE(finished.count(), 1);
    const NeutralHandoffRunner::Outcome outcome = runner.outcome();
    QVERIFY(outcome.attempted);
    QVERIFY(outcome.neutralPassed);
    QVERIFY(!outcome.timedOut);
    QCOMPARE(outcome.stage, QStringLiteral("passed"));
    QVERIFY2(outcome.durationMs <= 20, qPrintable(QString::number(outcome.durationMs)));
    QCOMPARE(outcome.polls, 1);
    QCOMPARE(outcome.releaseTransitions, 1);
    QVERIFY(outcome.released);

    // The policy went back exactly once, and the input path was quiesced for the
    // duration and released again afterwards.
    QCOMPARE(sink.releases().size(), 1);
    QCOMPARE(sink.releases().first(), QStringLiteral("the overlay closed"));
    QVERIFY(!sink.exclusiveForegroundActive());
    QVERIFY(source.quiescedOnThenOff());

    const QString text = InputDiagnostics::instance().exportBetaText(
        QStringLiteral("build"), QStringLiteral("windows"), {}, {});
    QVERIFY2(text.contains(QStringLiteral("neutral-handoff=passed duration_ms=")),
             qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("GameInput release handoff:")), qPrintable(text));
}

void NeutralHandoffTest::aHeldPadKeepsThePolicyUntilItIsReleased()
{
    CountingFocusSink sink;
    sink.requestExclusiveForeground(QStringLiteral("test"));
    ScriptedSource source;
    source.attachSink(&sink);
    source.setSample(1, QStringLiteral("gamepad.cross"));

    NeutralHandoffRunner runner;
    runner.setBoundForTesting(2000, 5);
    runner.setPolicySink(&sink);
    runner.setSource(&source);

    QSignalSpy finished(&runner, &NeutralHandoffRunner::finished);
    runner.begin(QStringLiteral("the overlay closed"));

    // The wait is asynchronous: begin() returned, the pad is still held, and the
    // policy is STILL exclusive. Releasing it here is the exact leak this unit
    // exists to prevent.
    QCOMPARE(finished.count(), 0);
    QVERIFY(runner.running());
    QVERIFY(sink.exclusiveForegroundActive());
    QVERIFY(sink.releases().isEmpty());

    // Several polls while held, then the physical release.
    QTest::qWait(40);
    QVERIFY(runner.running());
    QVERIFY(sink.exclusiveForegroundActive());
    QVERIFY(source.samples() >= 2);
    // Every poll taken while waiting saw the policy in force: the order is
    // "wait first, release after", never the other way round.
    for (bool exclusive : source.exclusiveAtSample())
        QVERIFY(exclusive);

    source.setSample(0, QString());
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 2000);

    const NeutralHandoffRunner::Outcome outcome = runner.outcome();
    QVERIFY(outcome.neutralPassed);
    QCOMPARE(outcome.stage, QStringLiteral("passed"));
    QVERIFY(outcome.polls >= 3);
    QVERIFY(outcome.durationMs >= 30);
    QCOMPARE(outcome.releaseTransitions, 1);
    QCOMPARE(sink.releases().size(), 1);
    QVERIFY(source.quiescedOnThenOff());

    const QString text = InputDiagnostics::instance().exportBetaText(
        QStringLiteral("build"), QStringLiteral("windows"), {}, {});
    QVERIFY2(text.contains(QStringLiteral("neutral-handoff=waiting (the overlay closed)")),
             qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("neutral-handoff=passed duration_ms=")), qPrintable(text));
}

void NeutralHandoffTest::aPadThatNeverReleasesEndsInAnHonestTimeout()
{
    CountingFocusSink sink;
    sink.requestExclusiveForeground(QStringLiteral("test"));
    ScriptedSource source;
    source.attachSink(&sink);
    source.setSample(1, QStringLiteral("gamepad.l2"));

    NeutralHandoffRunner runner;
    runner.setBoundForTesting(60, 10);
    runner.setPolicySink(&sink);
    runner.setSource(&source);

    QSignalSpy finished(&runner, &NeutralHandoffRunner::finished);
    runner.begin(QStringLiteral("the overlay closed"));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 2000);

    const NeutralHandoffRunner::Outcome outcome = runner.outcome();
    QVERIFY(outcome.attempted);
    QVERIFY(!outcome.neutralPassed);
    QVERIFY(outcome.timedOut);
    QCOMPARE(outcome.stage, QStringLiteral("timeout"));
    QVERIFY(outcome.durationMs >= 60);
    QCOMPARE(outcome.heldDetail, QStringLiteral("gamepad.l2"));

    // The timeout cannot invent a release: it completes the close, restores the
    // policy once, and says plainly that the handoff did not pass.
    QCOMPARE(sink.releases().size(), 1);
    QVERIFY(!sink.exclusiveForegroundActive());
    QVERIFY(source.quiescedOnThenOff());

    const QString text = InputDiagnostics::instance().exportBetaText(
        QStringLiteral("build"), QStringLiteral("windows"), {}, {});
    QVERIFY2(text.contains(QStringLiteral("neutral-handoff=timeout duration_ms=")), qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("held=\"gamepad.l2\"")), qPrintable(text));
}

void NeutralHandoffTest::aWaitStopsWhenAnotherPathRestoresThePolicy()
{
    CountingFocusSink sink;
    sink.requestExclusiveForeground(QStringLiteral("test"));
    ScriptedSource source;
    source.attachSink(&sink);
    source.setSample(1, QStringLiteral("gamepad.cross"));

    NeutralHandoffRunner runner;
    runner.setBoundForTesting(2000, 5);
    runner.setPolicySink(&sink);
    runner.setSource(&source);

    QSignalSpy finished(&runner, &NeutralHandoffRunner::finished);
    runner.begin(QStringLiteral("the overlay closed"));
    QCOMPARE(finished.count(), 0);

    // The foreground probe (or the runtime stopping) can restore the policy
    // first. Then there is nothing left to defer — and nothing to release twice.
    sink.restoreBackground(QStringLiteral("the overlay lost the foreground"));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 2000);

    const NeutralHandoffRunner::Outcome outcome = runner.outcome();
    QCOMPARE(outcome.stage, QStringLiteral("released-elsewhere"));
    QVERIFY(!outcome.released);
    QCOMPARE(outcome.releaseTransitions, 0);
    QCOMPARE(sink.releases().size(), 1);
    QCOMPARE(sink.releases().first(), QStringLiteral("the overlay lost the foreground"));
    QVERIFY(source.quiescedOnThenOff());
}

void NeutralHandoffTest::aRepeatedBeginNeverRestartsTheWait()
{
    CountingFocusSink sink;
    sink.requestExclusiveForeground(QStringLiteral("test"));
    ScriptedSource source;
    source.attachSink(&sink);
    source.setSample(1, QStringLiteral("gamepad.cross"));

    NeutralHandoffRunner runner;
    runner.setBoundForTesting(2000, 5);
    runner.setPolicySink(&sink);
    runner.setSource(&source);
    QSignalSpy finished(&runner, &NeutralHandoffRunner::finished);

    runner.begin(QStringLiteral("the overlay closed"));
    const int pollsAfterFirstBegin = source.samples();
    runner.begin(QStringLiteral("a second close request"));

    // Ignored: same wait, no second quiesce, no second release later.
    QCOMPARE(finished.count(), 0);
    QVERIFY(runner.running());
    QCOMPARE(source.quiescedCalls().size(), 1);
    QVERIFY(source.samples() >= pollsAfterFirstBegin);

    source.setSample(0, QString());
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 2000);
    QCOMPARE(sink.releases().size(), 1);
    QCOMPARE(finished.count(), 1);
}

void NeutralHandoffTest::cancelStopsTheWaitWithoutReleasingThePolicy()
{
    CountingFocusSink sink;
    sink.requestExclusiveForeground(QStringLiteral("test"));
    ScriptedSource source;
    source.attachSink(&sink);
    source.setSample(1, QStringLiteral("gamepad.cross"));

    NeutralHandoffRunner runner;
    runner.setBoundForTesting(2000, 5);
    runner.setPolicySink(&sink);
    runner.setSource(&source);
    QSignalSpy finished(&runner, &NeutralHandoffRunner::finished);

    runner.begin(QStringLiteral("the overlay closed"));
    QVERIFY(runner.running());

    runner.cancel();
    QVERIFY(!runner.running());
    // The owner of the runtime session restores the policy when the session goes
    // away; an abandoned close must not hand a held pad back mid-gesture.
    QVERIFY(sink.exclusiveForegroundActive());
    QCOMPARE(sink.releases().size(), 0);
    QCOMPARE(runner.outcome().stage, QStringLiteral("cancelled"));
    QVERIFY(!finished.count());

    // ... and the input layer is not left quiesced by a close that went away.
    QVERIFY(source.quiescedOnThenOff());
}

QTEST_MAIN(NeutralHandoffTest)
#include "tst_neutralhandoff.moc"
