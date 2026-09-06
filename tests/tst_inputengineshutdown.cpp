#include "input/InputEngine.h"
#include "input/MouseHookDevice.h"
#include "input/BindingRuntime.h"
#include "config/ConfigManager.h"
#include "config/ConfigKeys.h"
#include "storage/CaptureDatabase.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <thread>

class InputEngineShutdownTest : public QObject
{
    Q_OBJECT
private slots:
    void destroyOwner_data()
    {
        QTest::addColumn<QString>("button");
        QTest::addColumn<QString>("modernMode");
        for (const auto& mode : {QStringLiteral("off"), QStringLiteral("auto")}) {
            for (const auto& button : {QString(), MouseHookDevice::ButtonBack,
                                      MouseHookDevice::ButtonForward,
                                      MouseHookDevice::ButtonMiddle}) {
                const QByteArray name = (mode + ':' + button).toUtf8();
                QTest::newRow(name.constData()) << button << mode;
            }
        }
    }

    void destroyOwner()
    {
        QFETCH(QString, button);
        QFETCH(QString, modernMode);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ConfigManager config(dir.filePath("config.json"));
        config.setValue(ConfigKeys::InputModernControllerSupport, modernMode);
        CaptureDatabase db(dir.filePath("test.db"));
        QVERIFY(db.open());
        if (!button.isEmpty())
            QVERIFY(db.upsertBindingOverride({"mouse", {}, "global.screenshot", 1,
                                             button, "tap"}));
        auto engine = std::make_unique<InputEngine>(&config, &db, nullptr);
        engine->m_runtime->reload();
        auto* mouse = engine->m_mouse.get();
        if (!mouse->start())
            QSKIP("WH_MOUSE_LL install refused in this session");
        QSignalSpy actions(engine.get(), &InputEngine::screenshotRequested);
        QSignalSpy releases(mouse, &MouseHookDevice::buttonReleased);
        if (!button.isEmpty()) {
            const WPARAM message = button == MouseHookDevice::ButtonMiddle
                ? WM_MBUTTONDOWN : WM_XBUTTONDOWN;
            const DWORD data = button == MouseHookDevice::ButtonBack
                ? DWORD(XBUTTON1) << 16 : DWORD(XBUTTON2) << 16;
            mouse->simulateEventForTest(message, data);
        }
        QCOMPARE(actions.count(), 0);
        engine.reset(); // exercise actual member destruction, not just device.stop()
        QCoreApplication::processEvents();
        QCOMPARE(releases.count(), button.isEmpty() ? 0 : 1);
        QCOMPARE(actions.count(), 0); // synthesized release must never complete a tap
        MouseHookDevice replacement;
        QVERIFY(replacement.start()); // producer was joined and singleton released
        replacement.stop();
    }

    void restartAndShutdownCancelQueuedInputAndRepeat()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ConfigManager config(dir.filePath("config.json"));
        config.setValue(ConfigKeys::InputModernControllerSupport, "off");
        CaptureDatabase db(dir.filePath("test.db"));
        QVERIFY(db.open());
        QVERIFY(db.upsertBindingOverride({"mouse", {}, "global.screenshot", 1,
                                         MouseHookDevice::ButtonBack, "press"}));
        auto engine = std::make_unique<InputEngine>(&config, &db, nullptr);
        engine->m_runtime->reload();
        auto* mouse = engine->m_mouse.get();
        if (!mouse->start())
            QSKIP("WH_MOUSE_LL install refused in this session");
        QSignalSpy actions(engine.get(), &InputEngine::screenshotRequested);
        // Same cross-thread delivery as the native hook, without injecting OS input.
        std::thread oldPress([mouse] {
            mouse->simulateEventForTest(WM_XBUTTONDOWN, DWORD(XBUTTON1) << 16);
        });
        oldPress.join();
        mouse->stop();
        QVERIFY(mouse->start());
        QCoreApplication::processEvents();
        QCOMPARE(actions.count(), 0);
        mouse->simulateEventForTest(WM_XBUTTONDOWN, DWORD(XBUTTON1) << 16);
        QCOMPARE(actions.count(), 1);
        mouse->simulateEventForTest(WM_XBUTTONUP, DWORD(XBUTTON1) << 16);
        int repeats = 0;
        engine->startNavRepeat(MouseHookDevice::ButtonBack, 1, [&](int) { ++repeats; });
        const int before = repeats;
        const int generation = engine->m_pendingGeneration;
        std::thread queuedPress([mouse] {
            mouse->simulateEventForTest(WM_XBUTTONDOWN, DWORD(XBUTTON1) << 16);
        });
        queuedPress.join();
        engine->shutdown();
        engine->shutdown(); // idempotent
        QVERIFY(!mouse->isRunning());
        QVERIFY(!engine->m_repeatTick->isActive());
        QVERIFY(engine->m_repeatTrigger.isEmpty());
        QVERIFY(!engine->m_repeatEmitter);
        QVERIFY(engine->m_pendingGeneration > generation);
        QCoreApplication::processEvents(); // disconnect does not discard queued signals
        QCOMPARE(actions.count(), 1);
        QCOMPARE(repeats, before);
        engine.reset();
        QCoreApplication::processEvents();
        QCOMPARE(actions.count(), 1);
        QCOMPARE(repeats, before);
    }
};

QTEST_GUILESS_MAIN(InputEngineShutdownTest)
#include "tst_inputengineshutdown.moc"
