#include "input/InputEngine.h"
#include "input/XInputDevice.h"
#include "input/WinMMDevice.h"
#include "input/BindingRuntime.h"
#include "input/InputDiagnostics.h"
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

    // The provider vanishes while the button is still physically down: the
    // device reports no release edge at all, so only the disconnect path can
    // close or cancel whatever cycle the press opened (cpo-c04).
    void vanishWhileHeld(int slot = 0) { setSlotState(slot, 1u << Gamepad::Share, false); }
    // A release arriving from a provider that is already gone.
    void lateRelease(int slot = 0) { setSlotState(slot, 0, false); }
    void reconnect(int slot = 0) { setSlotState(slot, 0, true); }
};

// The low-priority legacy provider path: InputEngine::providerFor() maps any
// pad that is neither the Sony nor the XInput backend onto WinMM. A real
// WinMMDevice needs a physical joystick, so the test drives the same Gamepad
// edge contract directly.
class SyntheticWinMM final : public WinMMDevice
{
public:
    using WinMMDevice::WinMMDevice;

    ControlId::DeviceProfile profile() const override
    {
        return {QStringLiteral("WinMM joystick"), QStringLiteral("winmm.slot0"),
                ControlId::ControllerFamily::Xbox,
                QStringLiteral("Synthetic WinMM (test)"), {}, {}};
    }

    void arrive() { m_arrived = true; emit connected(true); }
    // The provider disappears mid-press too: no release edge is invented.
    void vanish() { m_arrived = false; emit connected(false); }
    bool arrived() const { return m_arrived; }

    void edge(bool down)
    {
        if (down)
            publishControlPressed(ControlId::ViewBack, profile());
        else
            publishControlReleased(ControlId::ViewBack, profile());
    }

private:
    bool m_arrived = false;
};

class ControllerClipE2ETest : public QObject
{
    Q_OBJECT
    QTemporaryDir dir;
    std::unique_ptr<ConfigManager> config;
    std::unique_ptr<CaptureDatabase> db;
    std::unique_ptr<InputEngine> engine;
    SyntheticXInput* pad = nullptr;
    SyntheticWinMM* winmmPad = nullptr;
    QString profile;
    QString winmmProfile;

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

        // Second backend on the legacy provider path: connected but quiet, so
        // the XInput pad owns the active role until a test moves it.
        auto winmm = std::make_unique<SyntheticWinMM>();
        winmmPad = winmm.get();
        engine->m_winmmPad = winmmPad;
        engine->attachGamepad(std::move(winmm), "Synthetic WinMM");
        winmmPad->arrive();
        engine->observeLegacyBackend(winmmPad, winmmPad->profile());
        winmmProfile = engine->canonicalProfile(winmmPad, "winmm.slot0");
        QVERIFY(!winmmProfile.isEmpty());
        QVERIFY(winmmProfile != profile);
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
        const QString diagnostic = InputDiagnostics::instance().exportBetaText("test", "windows", {}, {});
        QVERIFY(diagnostic.contains("activation=tap"));
        QVERIFY(diagnostic.contains("trigger=" + ControlId::ViewBack));
        QVERIFY(diagnostic.contains("Resolved controller profile: sha256:"));
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

    // cpo-c04: route and gesture lifetimes across a provider that disappears
    // mid-gesture. The press cycle is closed by the removal itself and never
    // fires late once the pad is gone.
    void providerRemovalMidHoldCancelsArmedGesture()
    {
        QVERIFY(bind(profile, "hold"));
        QSignalSpy replay(engine.get(), &InputEngine::replayRequested);
        pad->edge(true);            // hold armed on the active backend
        QTest::qWait(80);           // well below the 250 ms hold threshold
        pad->vanishWhileHeld();     // provider gone, button still down
        QTest::qWait(400);          // past the threshold: nothing may fire late
        QCOMPARE(replay.count(), 0);
    }

    void lateReleaseFromRemovedProviderIsIgnored()
    {
        QVERIFY(bind(profile, "tap"));
        QSignalSpy replay(engine.get(), &InputEngine::replayRequested);
        pad->edge(true);
        QTest::qWait(30);
        pad->vanishWhileHeld();     // press cycle closed by the removal
        QTest::qWait(50);
        pad->lateRelease();         // release from a provider that is gone
        QTest::qWait(200);
        QCOMPARE(replay.count(), 0);   // cannot fire, reopen or leave it stuck
    }

    void rearmAfterProviderRemovalFiresOnce()
    {
        QVERIFY(bind(profile, "tap"));
        QSignalSpy replay(engine.get(), &InputEngine::replayRequested);
        pad->edge(true);
        pad->vanishWhileHeld();
        pad->lateRelease();
        QTest::qWait(50);
        QCOMPARE(replay.count(), 0);

        pad->reconnect();           // the same route comes back
        pad->edge(true);            // a fresh press re-arms it
        pad->edge(false);
        QCOMPARE(replay.count(), 1);
        QTest::qWait(300);          // exactly one action, nothing stuck behind
        QCOMPARE(replay.count(), 1);
    }

    // A mirrored press on another provider inside the duplicate window is a
    // trailing copy: it must be dropped, not held and replayed later.
    void mirroredPressInsideDuplicateWindowIsDropped()
    {
        QVERIFY(bind(profile, "tap"));
        QVERIFY(bind(winmmProfile, "tap", ControlId::ViewBack, "global.screenshot"));
        QSignalSpy replay(engine.get(), &InputEngine::replayRequested);
        QSignalSpy screenshots(engine.get(), &InputEngine::screenshotRequested);

        pad->edge(true);            // the active backend delivers the real press
        QTest::qWait(30);
        winmmPad->edge(true);       // the mirror arrives inside the 100 ms window
        winmmPad->edge(false);
        QTest::qWait(30);
        pad->edge(false);
        QCOMPARE(replay.count(), 1);
        QCOMPARE(screenshots.count(), 0);
        QTest::qWait(350);          // and nothing is replayed from a held run
        QCOMPARE(screenshots.count(), 0);
        QCOMPARE(replay.count(), 1);
    }

    // A candidate provider that carries the input alone is promoted, and the
    // switch cancels the stale gesture on the old route before the new route
    // becomes authoritative: exactly one action survives.
    void sustainedCandidatePromotesWithExactlyOneAction()
    {
        QVERIFY(bind(profile, "tap"));
        QVERIFY(bind(winmmProfile, "tap", ControlId::ViewBack, "global.screenshot"));
        QSignalSpy replay(engine.get(), &InputEngine::replayRequested);
        QSignalSpy screenshots(engine.get(), &InputEngine::screenshotRequested);

        pad->edge(true);            // active route press, armed and still held
        QTest::qWait(150);          // past the mirror window, inside the silence
        winmmPad->edge(true);       // candidate press: held for confirmation

        // The candidate path must actually be entered: the press is buffered
        // against WinMM, XInput still owns the active role and nothing has
        // fired yet. A scheduler overshoot (immediate takeover or a mirror
        // drop) fails here instead of quietly passing through the end state.
        QCOMPARE(engine->m_pending.source, static_cast<Gamepad*>(winmmPad));
        QCOMPARE(engine->m_activeBackend, static_cast<Gamepad*>(pad));
        QCOMPARE(screenshots.count(), 0);

        winmmPad->edge(false);      // released while still pending
        QTRY_COMPARE_WITH_TIMEOUT(screenshots.count(), 1, 900);

        // Promotion went through the confirmation window: the buffer is empty
        // and WinMM owns the active role now.
        QVERIFY(!engine->m_pending.source);
        QCOMPARE(engine->m_activeBackend, static_cast<Gamepad*>(winmmPad));
        QCOMPARE(screenshots.count(), 1);

        QCOMPARE(replay.count(), 0);   // the stale active gesture was cancelled
        pad->edge(false);              // late release of the old route
        QCOMPARE(replay.count(), 0);
        QCOMPARE(screenshots.count(), 1);
    }

    // A pending candidate press whose provider disappears before confirmation
    // is never replayed, and the surviving route still closes normally.
    void pendingCandidatePressDiesWithItsProvider()
    {
        QVERIFY(bind(profile, "tap"));
        QVERIFY(bind(winmmProfile, "tap", ControlId::ViewBack, "global.screenshot"));
        QSignalSpy replay(engine.get(), &InputEngine::replayRequested);
        QSignalSpy screenshots(engine.get(), &InputEngine::screenshotRequested);

        pad->edge(true);            // real press on the active route
        QTest::qWait(150);
        winmmPad->edge(true);       // candidate press, awaiting confirmation

        // The press must really be buffered as the pending candidate, and it
        // must not have moved the active role already (the overshoot path).
        QCOMPARE(engine->m_pending.source, static_cast<Gamepad*>(winmmPad));
        QCOMPARE(engine->m_activeBackend, static_cast<Gamepad*>(pad));

        QTest::qWait(60);
        winmmPad->vanish();         // provider disappears before it confirms
        QTest::qWait(500);          // past the confirmation timer

        // The candidate died with its provider: nothing is pending, it never
        // became authoritative, and the wait cannot replay it.
        QVERIFY(!engine->m_pending.source);
        QCOMPARE(engine->m_activeBackend, static_cast<Gamepad*>(pad));
        QCOMPARE(screenshots.count(), 0);

        pad->edge(false);           // the surviving route closes on its own
        QCOMPARE(replay.count(), 1);
        QTest::qWait(300);
        QCOMPARE(screenshots.count(), 0);
        QCOMPARE(replay.count(), 1);
    }

    // Two routes on the same pad: a second slot pressing mid-hold fires its own
    // action exactly once and never disturbs the held route's cycle.
    void twoRoutesMidHoldDoNotCrossTalk()
    {
        QVERIFY(bind(profile, "hold"));
        const QString secondProfile = engine->canonicalProfile(pad, "xinput.slot1");
        QVERIFY(!secondProfile.isEmpty());
        QVERIFY(secondProfile != profile);
        QVERIFY(bind(secondProfile, "tap", ControlId::ViewBack, "global.screenshot"));
        QSignalSpy replay(engine.get(), &InputEngine::replayRequested);
        QSignalSpy screenshots(engine.get(), &InputEngine::screenshotRequested);

        pad->edge(true);            // slot 0 hold armed
        QTest::qWait(80);
        pad->secondEdge(true);      // slot 1 presses mid-hold
        pad->secondEdge(false);
        QCOMPARE(screenshots.count(), 1);   // its own tap, exactly once
        QCOMPARE(replay.count(), 0);        // the held route has not fired yet
        QTRY_COMPARE_WITH_TIMEOUT(replay.count(), 1, 700);   // fires once at threshold
        pad->edge(false);
        QTest::qWait(100);
        QCOMPARE(replay.count(), 1);        // nothing stuck, nothing duplicated
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
