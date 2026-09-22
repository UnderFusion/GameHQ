#include "gameinput/FakeGameInputApi.h"
#include "gameinput/GameInputEventQueue.h"
#include "gameinput/GameInputFocusController.h"
#include "gameinput/GameInputWrapper.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>
#include <QThread>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

using namespace ModernInput;

class TestGameInputWrapper : public QObject
{
    Q_OBJECT

private slots:
    void discreteOrderAndStateCoalescing();
    void quickTapSurvivesOneDrain();
    void staleReadingIsDroppedWhenDeviceRemoved();
    void overflowRequestsRecovery();
    void callbacksArriveOnQtThread();
    void backgroundFocusPolicyIsAppliedBeforeCallbackRegistration();
    void focusPolicyOwnerIsAttachedAndDetachedWithTheSession();
    void shutdownIsOrderedAndLateCallbacksAreHarmless();
    void restartAfterShutdownDeliversEventsAgain();
    void initializationFailureFailsSoft();
    void concurrentProducersDrainAndShutdownStayConsistent();
};

void TestGameInputWrapper::discreteOrderAndStateCoalescing()
{
    GameInputEventQueue queue(8, 4);
    GameInputEvent state1;
    state1.kind = GameInputEventKind::Reading;
    state1.deviceId = QStringLiteral("pad-a");
    state1.standardButtons = 1;
    // First reading is a button transition (unknown -> 1): ordered/discrete.
    QVERIFY(queue.push(state1));
    // Button-stable repeats coalesce into one latest-state entry.
    QVERIFY(queue.push(state1));
    QVERIFY(queue.push(state1));

    for (int i = 0; i < 3; ++i) {
        GameInputEvent edge;
        edge.kind = i % 2 ? GameInputEventKind::SystemButtonReleased
                          : GameInputEventKind::SystemButtonPressed;
        edge.deviceId = QStringLiteral("pad-a");
        edge.controlId = QStringLiteral("control-%1").arg(i);
        QVERIFY(queue.push(edge));
    }

    const GameInputEventBatch batch = queue.take();
    QCOMPARE(batch.events.size(), 5);
    QCOMPARE(batch.events[0].kind, GameInputEventKind::Reading);
    QCOMPARE(batch.events[0].standardButtons, quint32(1));
    QCOMPARE(batch.events[1].controlId, QStringLiteral("control-0"));
    QCOMPARE(batch.events[2].controlId, QStringLiteral("control-1"));
    QCOMPARE(batch.events[3].controlId, QStringLiteral("control-2"));
    QCOMPARE(batch.events[4].kind, GameInputEventKind::Reading);
    QVERIFY(batch.events[1].sequence < batch.events[2].sequence);
    QVERIFY(batch.events[2].sequence < batch.events[3].sequence);
    QVERIFY(!batch.forceResync);
}

void TestGameInputWrapper::quickTapSurvivesOneDrain()
{
    // A press-then-release arriving before a single drain must deliver both
    // edges in order — coalescing here would erase the whole tap.
    GameInputEventQueue queue(8, 4);
    GameInputEvent reading;
    reading.kind = GameInputEventKind::Reading;
    reading.deviceId = QStringLiteral("pad-tap");
    reading.buttonStates = {0, 0, 0};
    QVERIFY(queue.push(reading));
    reading.buttonStates = {0, 0, 1};
    QVERIFY(queue.push(reading));
    reading.buttonStates = {0, 0, 0};
    QVERIFY(queue.push(reading));

    const GameInputEventBatch batch = queue.take();
    QCOMPARE(batch.events.size(), 3);
    QCOMPARE(batch.events[0].buttonStates, (QVector<quint8>{0, 0, 0}));
    QCOMPARE(batch.events[1].buttonStates, (QVector<quint8>{0, 0, 1}));
    QCOMPARE(batch.events[2].buttonStates, (QVector<quint8>{0, 0, 0}));
    QVERIFY(batch.events[0].sequence < batch.events[1].sequence);
    QVERIFY(batch.events[1].sequence < batch.events[2].sequence);
    QVERIFY(!batch.forceResync);
}

void TestGameInputWrapper::staleReadingIsDroppedWhenDeviceRemoved()
{
    GameInputEventQueue queue(8, 4);
    GameInputEvent reading;
    reading.kind = GameInputEventKind::Reading;
    reading.deviceId = QStringLiteral("pad-gone");
    reading.standardButtons = 1;
    QVERIFY(queue.push(reading)); // discrete transition
    QVERIFY(queue.push(reading)); // coalesced latest state

    GameInputEvent removed;
    removed.kind = GameInputEventKind::DeviceRemoved;
    removed.deviceId = reading.deviceId;
    QVERIFY(queue.push(removed));

    // The pending coalesced reading predates the removal; delivering it after
    // DeviceRemoved would let the consumer re-observe a dead device.
    const GameInputEventBatch batch = queue.take();
    QCOMPARE(batch.events.size(), 2);
    QCOMPARE(batch.events[0].kind, GameInputEventKind::Reading);
    QCOMPARE(batch.events[1].kind, GameInputEventKind::DeviceRemoved);
}

void TestGameInputWrapper::overflowRequestsRecovery()
{
    GameInputEventQueue queue(2, 2);
    for (int i = 0; i < 5; ++i) {
        GameInputEvent edge;
        edge.kind = i % 2 ? GameInputEventKind::SystemButtonReleased
                          : GameInputEventKind::SystemButtonPressed;
        edge.deviceId = QStringLiteral("pad-overflow");
        edge.controlId = QStringLiteral("gamepad.extra.%1").arg(i);
        QVERIFY(queue.push(edge));
    }
    const GameInputEventBatch batch = queue.take();
    QVERIFY(batch.forceResync);
    QVERIFY(batch.overflowCount >= 1);
    QCOMPARE(batch.uncertainDevices, QStringList{QStringLiteral("pad-overflow")});
    QCOMPARE(batch.events.back().kind, GameInputEventKind::RecoveryRequired);
    QCOMPARE(batch.events.back().deviceId, QStringLiteral("pad-overflow"));
}

void TestGameInputWrapper::callbacksArriveOnQtThread()
{
    auto fake = std::make_unique<FakeGameInputApi>();
    FakeGameInputApi* raw = fake.get();
    GameInputWrapper wrapper(std::move(fake));
    QString error;
    QVERIFY2(wrapper.start(error), qPrintable(error));

    bool deliveredOnQtThread = false;
    GameInputEventBatch delivered;
    connect(&wrapper, &GameInputWrapper::eventsReady, this,
            [&](const GameInputEventBatch& batch) {
        deliveredOnQtThread = QThread::currentThread() == qApp->thread();
        delivered = batch;
    });

    GameInputEvent event;
    event.kind = GameInputEventKind::SystemButtonPressed;
    event.deviceId = QStringLiteral("pad-thread");
    event.controlId = QStringLiteral("gamepad.capture");
    raw->emitSystem(event, true);

    QTRY_VERIFY_WITH_TIMEOUT(!delivered.events.isEmpty(), 2000);
    QVERIFY(deliveredOnQtThread);
    QCOMPARE(delivered.events.front().controlId, QStringLiteral("gamepad.capture"));
}

// GameHQ runs behind the focused game; without the background focus policy
// the runtime withholds standard readings AND the system Share/Guide buttons
// from an unfocused process. The policy is only honored for callbacks
// registered after it is set, so the order is part of the contract.
void TestGameInputWrapper::backgroundFocusPolicyIsAppliedBeforeCallbackRegistration()
{
    auto fake = std::make_unique<FakeGameInputApi>();
    FakeGameInputApi* raw = fake.get();
    GameInputWrapper wrapper(std::move(fake));
    QString error;
    QVERIFY2(wrapper.start(error), qPrintable(error));

    const QStringList log = raw->callLog();
    const int initialize = log.indexOf(QStringLiteral("initialize"));
    const int policy = log.indexOf(QStringLiteral("focus-policy:background"));
    const int firstRegistration = log.indexOf(QStringLiteral("register:1"));
    QVERIFY(policy > initialize);
    QVERIFY(firstRegistration > policy);

    // A restart re-applies the policy on the fresh runtime session.
    wrapper.shutdown();
    QVERIFY(wrapper.start(error));
    QVERIFY(raw->callLog().lastIndexOf(QStringLiteral("focus-policy:background"))
            > raw->callLog().lastIndexOf(QStringLiteral("unload")));
    wrapper.shutdown();
}

// cpo-o06c: when a controller owns the process-wide policy, the wrapper is the
// thing that attaches it to the runtime session — and it must detach it on
// shutdown, so a stopped or restarted session can never inherit the exclusive
// state an interactive overlay asked for.
void TestGameInputWrapper::focusPolicyOwnerIsAttachedAndDetachedWithTheSession()
{
    auto fake = std::make_unique<FakeGameInputApi>();
    FakeGameInputApi* raw = fake.get();
    auto controller = std::make_unique<GameInputFocusController>();
    GameInputFocusController* focus = controller.get();
    GameInputWrapper wrapper(std::move(fake));
    wrapper.setFocusController(focus);

    QString error;
    QVERIFY2(wrapper.start(error), qPrintable(error));
    QVERIFY(focus->policyAttached());
    QVERIFY(focus->focusMode() == GameInputFocusMode::Background);
    // Attaching IS what applies the start-up policy, so the ordering contract is
    // unchanged: policy before the first callback registration.
    QVERIFY(raw->callLog().indexOf(QStringLiteral("focus-policy:background"))
            < raw->callLog().indexOf(QStringLiteral("register:1")));

    // The overlay asks for exclusive input while it is interactive.
    QVERIFY(focus->requestExclusiveForeground(QStringLiteral("overlay-interactive")));
    QVERIFY(focus->exclusiveForegroundActive());

    wrapper.shutdown();

    // Released before the runtime was unloaded, and the session is no longer
    // attached at all.
    const QStringList log = raw->callLog();
    const int restore = log.lastIndexOf(QStringLiteral("focus-policy:background"));
    QVERIFY(restore > log.lastIndexOf(QStringLiteral("focus-policy:exclusive-foreground")));
    QVERIFY(restore < log.lastIndexOf(QStringLiteral("unload")));
    QVERIFY(!focus->policyAttached());
    QVERIFY(focus->focusMode() == GameInputFocusMode::Background);

    // A fresh session starts from the background policy, whatever the previous
    // one ended in. Hold the policy exclusive — as the overlay does while it is
    // up — then tear the session down and bring it back.
    QVERIFY2(wrapper.start(error), qPrintable(error));
    QVERIFY(focus->requestExclusiveForeground(QStringLiteral("overlay-interactive")));
    QVERIFY(focus->exclusiveForegroundActive());
    wrapper.shutdown();
    // While the runtime is gone a request is refused rather than remembered, so
    // nothing can be applied later to a session that never asked for it.
    QVERIFY(!focus->requestExclusiveForeground(QStringLiteral("overlay-interactive")));
    QVERIFY2(wrapper.start(error), qPrintable(error));
    QVERIFY(focus->focusMode() == GameInputFocusMode::Background);
    QVERIFY(!focus->exclusiveForegroundActive());
    wrapper.shutdown();
}

void TestGameInputWrapper::shutdownIsOrderedAndLateCallbacksAreHarmless()
{
    auto fake = std::make_unique<FakeGameInputApi>();
    FakeGameInputApi* raw = fake.get();
    GameInputWrapper wrapper(std::move(fake));
    QString error;
    QVERIFY(wrapper.start(error));
    QSignalSpy delivered(&wrapper, &GameInputWrapper::eventsReady);

    wrapper.shutdown();
    const QStringList log = raw->callLog();
    const int release = log.indexOf(QStringLiteral("release-devices"));
    const int unload = log.indexOf(QStringLiteral("unload"));
    QVERIFY(release > 0);
    QVERIFY(unload > release);
    for (int token = 1; token <= 3; ++token) {
        const int stop = log.indexOf(QStringLiteral("stop:%1").arg(token));
        const int unregister = log.indexOf(QStringLiteral("unregister:%1").arg(token));
        QVERIFY(stop > 0);
        QVERIFY(unregister > stop);
        QVERIFY(unregister < release);
    }

    GameInputEvent late;
    late.kind = GameInputEventKind::SystemButtonReleased;
    late.deviceId = QStringLiteral("late-pad");
    raw->emitRetired(late);
    QCoreApplication::processEvents();
    QCOMPARE(delivered.count(), 0);
    QVERIFY(!wrapper.running());
}

void TestGameInputWrapper::restartAfterShutdownDeliversEventsAgain()
{
    // The Settings toggle is Auto -> Off -> Auto: a wrapper that was shut
    // down must deliver events again after the next start().
    auto fake = std::make_unique<FakeGameInputApi>();
    FakeGameInputApi* raw = fake.get();
    GameInputWrapper wrapper(std::move(fake));
    QSignalSpy delivered(&wrapper, &GameInputWrapper::eventsReady);
    QString error;

    for (int cycle = 0; cycle < 2; ++cycle) {
        QVERIFY2(wrapper.start(error), qPrintable(error));
        const int before = delivered.count();
        GameInputEvent event;
        event.kind = GameInputEventKind::SystemButtonPressed;
        event.deviceId = QStringLiteral("pad-restart");
        event.controlId = QStringLiteral("gamepad.capture");
        raw->emitSystem(event, true);
        QTRY_VERIFY_WITH_TIMEOUT(delivered.count() > before, 2000);
        wrapper.shutdown();
        QVERIFY(!wrapper.running());
    }
}

void TestGameInputWrapper::initializationFailureFailsSoft()
{
    auto fake = std::make_unique<FakeGameInputApi>();
    fake->setInitializeFailure(QStringLiteral("runtime missing"));
    GameInputWrapper wrapper(std::move(fake));
    QString error;
    QVERIFY(!wrapper.start(error));
    QCOMPARE(error, QStringLiteral("runtime missing"));
    QVERIFY(!wrapper.running());
}

// GameInput callbacks arrive on runtime threads while the Qt side drains and
// may shut down at any moment. This stress run drives the queue exactly that
// way: four producer threads emitting edges, capability changes and removals
// against a concurrently draining consumer, ending in stopAccepting() racing
// the last pushes. Every ACCEPTED discrete edge must come out exactly once,
// in per-device order, and a stopped queue must reject cleanly — no loss, no
// duplication, no crash.
void TestGameInputWrapper::concurrentProducersDrainAndShutdownStayConsistent()
{
    // Capacity exceeds the whole workload (1600 edges + capability changes +
    // removals) on purpose: this test measures ordering and exactly-once
    // delivery, so no push may hit the overflow path — push() still returns
    // true there after scheduling a resync, and a slow consumer on a loaded CI
    // runner would turn that into a spurious count mismatch. Overflow behavior
    // has its own test (overflowRequestsRecovery).
    GameInputEventQueue queue(4096, 128);
    constexpr int kThreads = 4;
    constexpr int kEdgesPerThread = 400;
    std::array<std::atomic<int>, kThreads> accepted{};
    std::vector<std::unique_ptr<QThread>> producers;

    for (int t = 0; t < kThreads; ++t) {
        producers.push_back(std::unique_ptr<QThread>(QThread::create([&queue, &accepted, t] {
            const QString deviceId = QStringLiteral("stress-pad-%1").arg(t);
            for (int i = 0; i < kEdgesPerThread; ++i) {
                GameInputEvent edge;
                edge.kind = i % 2 ? GameInputEventKind::SystemButtonReleased
                                  : GameInputEventKind::SystemButtonPressed;
                edge.deviceId = deviceId;
                edge.controlId = QStringLiteral("gamepad.capture");
                if (queue.push(edge))
                    accepted[t].fetch_add(1, std::memory_order_relaxed);
                if (i % 16 == 0) {
                    GameInputEvent capability;
                    capability.kind = GameInputEventKind::CapabilityChanged;
                    capability.deviceId = deviceId;
                    queue.push(capability);
                }
            }
            GameInputEvent removed;
            removed.kind = GameInputEventKind::DeviceRemoved;
            removed.deviceId = deviceId;
            queue.push(removed);
        })));
        producers.back()->start();
    }

    // Drain concurrently with production, tracking per-device edge counts and
    // per-device sequence monotonicity.
    QHash<QString, int> received;
    QHash<QString, quint64> lastSequence;
    const auto drain = [&] {
        const GameInputEventBatch batch = queue.take();
        QVERIFY(!batch.forceResync);
        for (const auto& event : batch.events) {
            if (event.kind != GameInputEventKind::SystemButtonPressed
                && event.kind != GameInputEventKind::SystemButtonReleased)
                continue;
            ++received[event.deviceId];
            QVERIFY(event.sequence > lastSequence.value(event.deviceId));
            lastSequence[event.deviceId] = event.sequence;
        }
    };
    bool producing = true;
    while (producing) {
        producing = false;
        for (const auto& producer : producers)
            producing = producing || !producer->isFinished();
        drain();
    }
    for (const auto& producer : producers)
        QVERIFY(producer->wait(5000));
    drain();
    QCOMPARE(queue.pendingCount(), 0);

    for (int t = 0; t < kThreads; ++t) {
        QCOMPARE(received.value(QStringLiteral("stress-pad-%1").arg(t)),
                 accepted[t].load(std::memory_order_relaxed));
    }

    // Shutdown contract: a stopped queue refuses new events outright.
    queue.stopAccepting();
    GameInputEvent late;
    late.kind = GameInputEventKind::SystemButtonPressed;
    late.deviceId = QStringLiteral("stress-pad-0");
    late.controlId = QStringLiteral("gamepad.capture");
    QVERIFY(!queue.push(late));
}

QTEST_GUILESS_MAIN(TestGameInputWrapper)
#include "tst_gameinputwrapper.moc"

