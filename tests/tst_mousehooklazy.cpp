#include "input/MouseHookDevice.h"
#include "input/MouseMonitorPolicy.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>

#include <windows.h>

// Lazy mouse-hook activation (GitHub stutter report): the WH_MOUSE_LL hook
// exists only while a mouse binding or a mouse capture can consume it, its
// lifecycle is idempotent, and stopping releases held buttons so a binding
// removed mid-hold cannot leave a gesture armed forever. The policy is pure
// logic; the lifecycle drives the real hook, so those cases skip when the
// session refuses hook installs (e.g. a non-interactive service context).
class MouseHookLazyTest : public QObject
{
    Q_OBJECT

private slots:
    // ---- MouseMonitorPolicy (pure logic) ----

    void noBindingsNoCaptureMeansNoHook()
    {
        // The GitHub reporter's configuration: default bindings, editor idle.
        QVERIFY(!MouseMonitorPolicy::needed(false, QStringLiteral("controller"),
                                            false, QStringLiteral("idle")));
        QVERIFY(!MouseMonitorPolicy::needed(false, QStringLiteral("mouse"),
                                            false, QStringLiteral("idle")));
    }

    void anyMouseBindingRequiresTheHook()
    {
        QVERIFY(MouseMonitorPolicy::needed(true, QStringLiteral("controller"),
                                           false, QStringLiteral("idle")));
    }

    void mouseCaptureRequiresTheHookBeforeAnyBindingExists()
    {
        QVERIFY(MouseMonitorPolicy::needed(false, QStringLiteral("mouse"),
                                           true, QStringLiteral("idle")));
        QVERIFY(MouseMonitorPolicy::needed(false, QStringLiteral("mouse"),
                                           false, QStringLiteral("record")));
    }

    void captureOnAnotherGroupDoesNotRequireTheHook()
    {
        QVERIFY(!MouseMonitorPolicy::needed(false, QStringLiteral("controller"),
                                            true, QStringLiteral("record")));
    }

    // ---- MouseHookDevice lifecycle (real hook) ----

    void startStopIsIdempotentAndRestartable()
    {
        MouseHookDevice device;
        if (!device.start())
            QSKIP("WH_MOUSE_LL install refused in this session");
        QVERIFY(device.isRunning());
        QVERIFY(device.start());   // second start is a no-op success
        QVERIFY(device.isRunning());

        device.stop();
        QVERIFY(!device.isRunning());
        device.stop();             // second stop is a no-op
        QVERIFY(!device.isRunning());

        QVERIFY(device.start());   // the hook can come back after a stop
        QVERIFY(device.isRunning());
        device.stop();
    }

    void stopReleasesHeldButtons()
    {
        MouseHookDevice device;
        if (!device.start())
            QSKIP("WH_MOUSE_LL install refused in this session");

        QSignalSpy pressed(&device, &MouseHookDevice::buttonPressed);
        QSignalSpy released(&device, &MouseHookDevice::buttonReleased);

        // Back (XBUTTON1) goes down and never up before the hook stops.
        device.simulateEventForTest(WM_XBUTTONDOWN,
                                    static_cast<DWORD>(XBUTTON1) << 16);
        device.stop();

        QTRY_COMPARE(pressed.count(), 1);
        QTRY_COMPARE(released.count(), 1);
        QCOMPARE(released.constFirst().constFirst().toString(),
                 MouseHookDevice::ButtonBack);
    }

    // The stop→restart race an isRunning() check cannot close: a press
    // queued (cross-thread) under hook lifetime N arrives only after the
    // hook stopped AND restarted, when isRunning() is true again. The
    // generation stamp must expose it as stale; a fresh press in the new
    // lifetime must still match.
    void staleQueuedPressAcrossRestartIsRejected()
    {
        MouseHookDevice device;
        if (!device.start())
            QSKIP("WH_MOUSE_LL install refused in this session");

        QString lastCode;
        int lastGeneration = -1;
        int deliveries = 0;
        connect(&device, &MouseHookDevice::buttonPressed, &device,
                [&](const QString& code, int generation) {
                    lastCode = code;
                    lastGeneration = generation;
                    ++deliveries;
                },
                Qt::QueuedConnection);   // same delivery shape as hook-thread emits

        device.simulateEventForTest(WM_XBUTTONDOWN,
                                    static_cast<DWORD>(XBUTTON1) << 16);
        device.stop();
        QVERIFY(device.start());   // restart before the queued press lands

        QCoreApplication::processEvents();
        QCOMPARE(deliveries, 1);
        QCOMPARE(lastCode, MouseHookDevice::ButtonBack);
        // The receiver contract (InputEngine): a press whose generation is
        // not current is from a previous hook lifetime — reject it.
        QVERIFY(lastGeneration != device.generation());

        // A genuine press in the new lifetime carries the current stamp.
        device.simulateEventForTest(WM_XBUTTONDOWN,
                                    static_cast<DWORD>(XBUTTON1) << 16);
        QCoreApplication::processEvents();
        QCOMPARE(deliveries, 2);
        QCOMPARE(lastGeneration, device.generation());

        device.stop();
    }

    void rapidStartStopCyclesStayHealthy()
    {
        MouseHookDevice device;
        if (!device.start())
            QSKIP("WH_MOUSE_LL install refused in this session");
        device.stop();
        // Bounded by the suite timeout: any deadlock, leaked thread or
        // failed re-install surfaces as a hang or a false start() here.
        for (int i = 0; i < 25; ++i) {
            QVERIFY(device.start());
            QVERIFY(device.isRunning());
            device.stop();
            QVERIFY(!device.isRunning());
        }
    }

    void balancedPressReleaseLeavesNothingHeld()
    {
        MouseHookDevice device;
        if (!device.start())
            QSKIP("WH_MOUSE_LL install refused in this session");

        QSignalSpy released(&device, &MouseHookDevice::buttonReleased);
        device.simulateEventForTest(WM_MBUTTONDOWN, 0);
        device.simulateEventForTest(WM_MBUTTONUP, 0);
        device.stop();

        // Exactly the real release — stop() had nothing left to synthesize.
        QTRY_COMPARE(released.count(), 1);
        QCOMPARE(released.constFirst().constFirst().toString(),
                 MouseHookDevice::ButtonMiddle);
    }
};

QTEST_GUILESS_MAIN(MouseHookLazyTest)
#include "tst_mousehooklazy.moc"
