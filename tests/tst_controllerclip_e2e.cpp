#include "input/InputEngine.h"
#include "input/XInputDevice.h"
#include "input/BindingRuntime.h"
#include "capture/FramePumpService.h"
#include "config/ConfigManager.h"
#include "config/ConfigKeys.h"
#include "storage/CaptureDatabase.h"
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace ModernInput;

// Use the production XInput edge publisher without loading the system DLL.
class SyntheticXInput final : public XInputDevice
{
public:
    void edge(bool down) { setSlotState(0, down ? (1u << Gamepad::Share) : 0, true); }
    void secondEdge(bool down) { setSlotState(1, down ? (1u << Gamepad::Share) : 0, true); }
};

class ControllerClipE2ETest : public QObject
{
    Q_OBJECT
    QTemporaryDir dir;
    std::unique_ptr<ConfigManager> config;
    std::unique_ptr<CaptureDatabase> db;
    std::unique_ptr<InputEngine> engine;
    SyntheticXInput* pad = nullptr;
    QString profile;

    bool bind(const QString& target, const QString& activation,
              const QString& control = ControlId::ViewBack,
              const QString& action = QStringLiteral("global.save_replay"))
    {
        const bool ok = db->upsertBindingOverride(
            {"controller", target, action, 1, control, activation,
             activation == "hold" ? 250 : 0, false, 1});
        engine->m_runtime->reload();
        return ok;
    }

private slots:
    void init()
    {
        QVERIFY(dir.isValid());
        config = std::make_unique<ConfigManager>(dir.filePath("config.json"));
        config->setValue(ConfigKeys::InputModernControllerSupport, "off");
        db = std::make_unique<CaptureDatabase>(dir.filePath("test.db"));
        QVERIFY(db->open());
        QVERIFY(db->clearAllBindingOverrides());
        engine = std::make_unique<InputEngine>(config.get(), db.get(), nullptr);
        auto synthetic = std::make_unique<SyntheticXInput>();
        pad = synthetic.get();
        engine->m_xinputPad = pad;
        engine->attachGamepad(std::move(synthetic), "Synthetic XInput");
        pad->edge(false);
        pad->setKnownDeviceIdentity(0, "endpoint-a", "2dc8:6001");
        engine->observeLegacyBackend(pad, pad->profile());
        profile = engine->canonicalProfile(pad, "xinput.slot0");
        QVERIFY(!profile.isEmpty());
        engine->m_runtime->reload();
    }

    void cleanup()
    {
        engine.reset();
        db.reset();
        config.reset();
    }

    void tapUsesRealDispatchAndKeepsControllerSource()
    {
        QVERIFY(bind(profile, "tap"));
        QSignalSpy replay(engine.get(), &InputEngine::replayRequested);
        pad->edge(true);
        QCOMPARE(replay.count(), 0);
        pad->edge(false);
        QCOMPARE(replay.count(), 1);
        const auto request = qvariant_cast<CaptureRequest>(replay.first().first());
        QVERIFY(request.id != 0);
        QCOMPARE(request.source, CaptureRequest::Source::Controller);
        pad->edge(false);
        QCOMPARE(replay.count(), 1);
    }

    void holdThreshold_data()
    {
        QTest::addColumn<int>("duration");
        QTest::addColumn<int>("expected");
        QTest::newRow("below") << 100 << 0;
        QTest::newRow("at") << 250 << 1;
        QTest::newRow("above") << 350 << 1;
    }

    void holdThreshold()
    {
        QFETCH(int, duration);
        QFETCH(int, expected);
        QVERIFY(bind(profile, "hold"));
        QSignalSpy replay(engine.get(), &InputEngine::replayRequested);
        pad->edge(true);
        QCOMPARE(replay.count(), 0);
        // Deliver the timer at the configured threshold before the release.
        // OS scheduling may overshoot; exact clock arithmetic is not claimed.
        if (duration) QTest::qSleep(duration);
        QCoreApplication::processEvents();
        if (expected) QTRY_COMPARE_WITH_TIMEOUT(replay.count(), expected, 100);
        pad->edge(false);
        QCOMPARE(replay.count(), expected);
        QTest::qWait(30);
        QCOMPARE(replay.count(), expected); // no delayed duplicate on release
    }

    void correlatedProviderAliasAndIsolatedIdentity()
    {
        const auto capabilities = ControllerCapability::StandardControls;
        const QString correlated = engine->m_providers.observeLegacy(
            ControllerProvider::WinMM, "winmm-correlated", "2dc8:6001", "Fixture",
            capabilities, nullptr, "endpoint-a");
        QCOMPARE(correlated, profile);
        engine->configureLogicalProfile(profile);
        QVERIFY(bind("winmm-correlated", "tap"));
        QSignalSpy replay(engine.get(), &InputEngine::replayRequested);
        pad->edge(true);
        pad->edge(false);
        QCOMPARE(replay.count(), 1);

        // A distinct endpoint of the same model must not inherit this
        // provider-specific override. Existing model aliases stay untouched.
        pad->setKnownDeviceIdentity(1, "endpoint-b", "2dc8:6001");
        engine->observeLegacyBackend(pad, pad->profileForSlot(1));
        QVERIFY(engine->canonicalProfile(pad, "xinput.slot1") != profile);
        pad->secondEdge(true);
        pad->secondEdge(false);
        QTest::qWait(350);
        QCOMPARE(replay.count(), 1);
    }

    void holdFiresWhilePressedAndDoesNotRepeat()
    {
        QVERIFY(bind(profile, "hold"));
        QSignalSpy replay(engine.get(), &InputEngine::replayRequested);
        pad->edge(true);
        QTRY_COMPARE_WITH_TIMEOUT(replay.count(), 1, 700);
        QTest::qWait(100);
        QCOMPARE(replay.count(), 1);
        pad->edge(false);
        QCOMPARE(replay.count(), 1);
    }

    void legacyFallbackAndExplicitPrecedence()
    {
        QVERIFY(bind(profile, "hold", ControlId::Capture));
        QSignalSpy replay(engine.get(), &InputEngine::replayRequested);
        QSignalSpy screenshots(engine.get(), &InputEngine::screenshotRequested);
        pad->edge(true);
        QTRY_COMPARE_WITH_TIMEOUT(replay.count(), 1, 700);
        pad->edge(false);
        QCOMPARE(screenshots.count(), 0);
        QVERIFY(bind(profile, "tap", ControlId::ViewBack, "global.screenshot"));
        pad->edge(true);
        pad->edge(false);
        QTRY_COMPARE_WITH_TIMEOUT(screenshots.count(), 1, 700);
        QTest::qWait(350);
        QCOMPARE(replay.count(), 1);
        QCOMPARE(screenshots.count(), 1);
    }

    void receiptAndRejectionKeepRequestId_data()
    {
        QTest::addColumn<bool>("busy");
        QTest::newRow("not-ready") << false;
        QTest::newRow("busy") << true;
    }

    void receiptAndRejectionKeepRequestId()
    {
        QFETCH(bool, busy);
        QVERIFY(bind(profile, "tap"));
        FramePumpService clips(nullptr, nullptr);
        // Seed only external readiness/ownership facts, never the outcome.
        // Starting avoids foreground discovery and capture hardware entirely.
        clips.m_buffer.requestStart("Synthetic game");
        if (busy) {
            const auto owner = clips.m_owners.acquireManual(0, 90);
            QVERIFY(clips.m_owners.beginSave(owner));
        }
        QSignalSpy requests(engine.get(), &InputEngine::replayRequested);
        QSignalSpy accepted(&clips, &FramePumpService::requestAccepted);
        QSignalSpy failed(&clips, &FramePumpService::clipFailed);
        QStringList order;
        connect(&clips, &FramePumpService::requestAccepted, this,
                [&] { order << "accepted"; });
        connect(&clips, &FramePumpService::clipFailed, this,
                [&] { order << "failed"; });
        connect(engine.get(), &InputEngine::replayRequested, &clips,
                qOverload<const CaptureRequest&>(&FramePumpService::saveReplay));
        pad->edge(true);
        pad->edge(false);
        QCOMPARE(requests.count(), 1);
        QCOMPARE(accepted.count(), 1);
        QCOMPARE(failed.count(), 1);
        QCOMPARE(order, QStringList({"accepted", "failed"}));
        const auto request = qvariant_cast<CaptureRequest>(requests.first().first());
        const auto receipt = qvariant_cast<CaptureRequest>(accepted.first().first());
        QCOMPARE(receipt.id, request.id);
        QCOMPARE(receipt.source, CaptureRequest::Source::Controller);
        QCOMPARE(failed.first().at(2).toULongLong(), request.id);
        QVERIFY(!failed.first().at(1).toString().isEmpty());
        QCOMPARE(failed.first().at(1).toString().contains("already in progress"), busy);
    }
};

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    ControllerClipE2ETest test;
    // Keep diagnostics available even on Windows runners without stdout.
    QStringList args = app.arguments();
    if (!args.contains("-o"))
        args << "-o" << QCoreApplication::applicationDirPath() + "/results.txt,txt";
    return QTest::qExec(&test, args);
}
#include "tst_controllerclip_e2e.moc"
