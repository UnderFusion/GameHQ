// t40/A10: what per-controller state outlives a controller.
//
// A controller with no stable identity (no endpoint, container, topology or
// app-local ID) gets a fresh, session-local logical route every time it
// connects. pruneStaleControllers() retires such a route from the registry and
// its mapping chain once nothing can reach it again. These cases drive
// thousands of weak connect/press/disconnect cycles through the real engine
// and count what the binding runtime still holds afterwards, and pin the
// gesture guarantees a cleanup must keep.
#include "config/ConfigManager.h"
#include "input/BindingRuntime.h"
#include "input/InputPatternRecognizer.h"
#include "input/DualSenseDevice.h"
#include "input/InputEngine.h"
#include "input/WinMMDevice.h"
#include "input/XInputDevice.h"
#include "storage/CaptureDatabase.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

namespace {
quint32 buttonBit(Gamepad::Button button) { return 1u << button; }
}  // namespace

class LifetimeXInput final : public XInputDevice
{
public:
    using XInputDevice::XInputDevice;
    void buttons(quint32 mask) { setSlotState(0, mask, true); }
    void press(const QString& controlId) { publishControlPressed(controlId, profile()); }
    void release(const QString& controlId) { publishControlReleased(controlId, profile()); }
};

// A pad that reports no stable identity at all: every connection is a new route.
class WeakWinMM final : public WinMMDevice
{
public:
    QString endpoint;
    ControlId::DeviceProfile profile() const override
    {
        return {QStringLiteral("WinMM joystick"), QStringLiteral("winmm.slot0"),
                ControlId::ControllerFamily::Generic, QStringLiteral("Weak WinMM"),
                QString(), endpoint};
    }
    void arrive() { emit connected(true); }
    void leave() { emit connected(false); }
    void edge(bool down, const QString& control)
    {
        if (down) publishControlPressed(control, profile());
        else publishControlReleased(control, profile());
    }
};

class ControllerStateLifetimeTest : public QObject
{
    Q_OBJECT

    std::unique_ptr<QTemporaryDir> dir;
    std::unique_ptr<ConfigManager> config;
    std::unique_ptr<CaptureDatabase> db;
    std::unique_ptr<InputEngine> engine;
    LifetimeXInput* pad = nullptr;
    WeakWinMM* weak = nullptr;

    BindingRuntime& rt() { return *engine->m_runtime; }

    QString weakRoute() const
    {
        return engine->canonicalProfile(weak, weak->profile().fingerprint);
    }

    // One full lifetime of a weak controller: connect, a tap, a hold that is
    // released, a chord-ish pair, disconnect.
    void weakCycle()
    {
        weak->arrive();
        // The only controller in use, as when it is the one the user holds.
        engine->activateBackend(weak, QStringLiteral("weak cycle"));
        weak->edge(true, ControlId::FaceSouth);
        weak->edge(false, ControlId::FaceSouth);
        weak->edge(true, ControlId::ShoulderLeft);
        weak->edge(true, ControlId::FaceEast);
        weak->edge(false, ControlId::FaceEast);
        weak->edge(false, ControlId::ShoulderLeft);
        weak->leave();
    }

private slots:
    void init()
    {
        dir = std::make_unique<QTemporaryDir>();
        QVERIFY(dir->isValid());
        config = std::make_unique<ConfigManager>(dir->filePath("config.json"));
        db = std::make_unique<CaptureDatabase>(dir->filePath("test.db"));
        QVERIFY(db->open());
        QVERIFY(db->clearAllBindingOverrides());
        engine = std::make_unique<InputEngine>(config.get(), db.get(), nullptr);

        auto x = std::make_unique<LifetimeXInput>();
        pad = x.get();
        engine->m_xinputPad = pad;
        engine->attachGamepad(std::move(x), QStringLiteral("Synthetic XInput"));
        pad->buttons(0);
        pad->setKnownDeviceIdentity(0, "endpoint-strong", "2dc8:6001");
        engine->observeLegacyBackend(pad, pad->profile());
        engine->activateBackend(pad, QStringLiteral("test setup"));
        rt().reload();

        auto w = std::make_unique<WeakWinMM>();
        weak = w.get();
        engine->m_winmmPad = weak;
        engine->attachGamepad(std::move(w), QStringLiteral("Weak WinMM"));
    }

    void cleanup()
    {
        engine.reset();
        db.reset();
        config.reset();
        dir.reset();
    }

    void weakReconnectsDoNotAccumulateState_data()
    {
        QTest::addColumn<int>("cycles");
        QTest::newRow("100") << 100;
        QTest::newRow("2000") << 2000;
    }

    void weakReconnectsDoNotAccumulateState()
    {
        QFETCH(int, cycles);
        // The strong pad is in use throughout: its state must survive.
        pad->press(ControlId::FaceNorth);
        pad->release(ControlId::FaceNorth);
        weakCycle();
        weakCycle();
        const BindingRuntime::RetainedState baseline = rt().retainedState();
        QSet<QString> routes;
        for (int i = 0; i < cycles; ++i) {
            weak->arrive();
            routes.insert(weakRoute());
            weak->leave();
            weakCycle();
        }
        const BindingRuntime::RetainedState after = rt().retainedState();
        qInfo().noquote() << QStringLiteral(
            "%1 weak cycles (%2 distinct routes): recognizer states %3 -> %4, timers %5 -> %6, "
            "press contexts %7 -> %8, aliases %9 -> %10, registry controllers %11")
            .arg(cycles).arg(routes.size())
            .arg(baseline.recognizerStates).arg(after.recognizerStates)
            .arg(baseline.recognizerTimers).arg(after.recognizerTimers)
            .arg(baseline.pressContexts).arg(after.pressContexts)
            .arg(baseline.profileAliases).arg(after.profileAliases)
            .arg(engine->m_providers.registry().controllers().size());
        QVERIFY(routes.size() > cycles / 2);   // weak identity: new routes, as designed
        QVERIFY2(after.total() <= baseline.total(),
                 qPrintable(QStringLiteral("retained %1 -> %2").arg(baseline.total()).arg(after.total())));
        QCOMPARE(after.recognizerTimers, after.recognizerStates * 3);
    }

    // ---- Recognizer: forgetting a route never takes a live gesture -------
    void forgettingARouteKeepsPendingAndOtherRoutes()
    {
        InputPatternRecognizer rec;
        InputPatternRecognizer::Timing timing;
        timing.multiTapIntervalMs = 80;
        rec.setTiming(timing);
        rec.setFactsProvider([](const InputPatternRecognizer::Context&, const QString& control) {
            InputPatternRecognizer::TriggerFacts f;
            f.tapCountMask = (1 << 1) | (1 << 2);          // x1 waits for a possible x2
            if (control == QLatin1String("hold"))
                f.holdThresholdsMs = {60};
            return f;
        });
        int taps = 0, holds = 0;
        connect(&rec, &InputPatternRecognizer::recognized, &rec,
                [&](const InputPatternRecognizer::Context&, const TriggerSpec&, const GestureSpec& g) {
                    (g.kind == GestureSpec::Kind::Hold ? holds : taps)++;
                });
        const InputPatternRecognizer::Context a{QStringLiteral("controller"), QStringLiteral("route-a")};
        const InputPatternRecognizer::Context b{QStringLiteral("controller"), QStringLiteral("route-b")};
        // route-a: one idle state, one tap waiting in its window, one held.
        rec.press(a, QStringLiteral("idle"));
        rec.release(a, QStringLiteral("idle"));
        QTRY_COMPARE(taps, 1);                              // window closed: idle now
        rec.press(a, QStringLiteral("tap"));
        rec.release(a, QStringLiteral("tap"));               // pending in its window
        rec.press(a, QStringLiteral("held"));                // physically down
        // route-b: a hold in progress on another controller.
        rec.press(b, QStringLiteral("hold"));
        QCOMPARE(rec.stateCount(), 4);
        QVERIFY(rec.hasPendingState(a.deviceGroup, a.deviceProfile));
        QCOMPARE(rec.forgetRoute(a.deviceGroup, a.deviceProfile), 1);   // only the idle one
        QCOMPARE(rec.stateCount(), 3);
        QTRY_COMPARE(taps, 2);                               // a's pending tap still fires once
        QTRY_COMPARE(holds, 1);                              // b's hold untouched
        rec.release(b, QStringLiteral("hold"));
        rec.release(a, QStringLiteral("held"));
        QTRY_COMPARE(taps, 3);                               // a's held control: a normal tap
        QTest::qWait(150);
        QCOMPARE(taps, 3);
        QCOMPARE(holds, 1);
        QCOMPARE(rec.forgetRoute(a.deviceGroup, a.deviceProfile), 2);   // now all idle
        QVERIFY(!rec.hasPendingState(a.deviceGroup, a.deviceProfile));
        QCOMPARE(rec.stateCount(), 1);                       // route-b's own state stays
    }

    // ---- Engine: the strong pad keeps working, exactly once --------------
    void theStrongPadStillActsOnceAfterManyWeakCycles()
    {
        for (int i = 0; i < 500; ++i)
            weakCycle();
        engine->activateBackend(pad, QStringLiteral("back to the strong pad"));
        QSignalSpy toggles(engine.get(), &InputEngine::overlayToggleRequested);
        pad->press(ControlId::Guide);
        pad->release(ControlId::Guide);
        QTRY_COMPARE(toggles.count(), 1);
        QTest::qWait(400);   // past every tap/chord window: no late duplicate
        QCOMPARE(toggles.count(), 1);
        QCOMPARE(rt().downControls(QStringLiteral("controller"),
                                   engine->canonicalProfile(pad, pad->profile().fingerprint)),
                 QStringList());
    }

    void aWeakPadLostMidHoldFiresNothingLaterAndIsFreed()
    {
        weakCycle();
        const BindingRuntime::RetainedState baseline = rt().retainedState();
        QSignalSpy actions(&rt(), &BindingRuntime::actionTriggered);
        weak->arrive();
        engine->activateBackend(weak, QStringLiteral("mid-hold"));
        const QString route = weakRoute();
        weak->edge(true, ControlId::Guide);   // held: Guide carries tap and hold bindings
        const int beforeLeave = actions.count();
        weak->leave();                        // gone while held
        QVERIFY(rt().downControls(QStringLiteral("controller"), route).isEmpty());
        QTest::qWait(2500);                   // past the default hold threshold
        QCOMPARE(actions.count(), beforeLeave);   // nothing fires for a gone controller
        weakCycle();                          // the next lifecycle retires it
        QVERIFY(rt().retainedState().total() <= baseline.total());
    }
};

QTEST_MAIN(ControllerStateLifetimeTest)
#include "tst_controllerstatelifetime.moc"
