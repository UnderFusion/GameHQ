// cpo-o06e at the input layer: the release handoff's SOURCE.
//
// The overlay never reads a device. It asks InputEngine, and these cases pin down
// what that answer is made of:
//
//   * "held" is tracked from the raw edges the backends publish, not from what
//     routed — a press that fired no action still counts, because the physical
//     state is what the game would inherit;
//   * a device that goes away takes its held controls with it, so a handoff can
//     never wait for a controller that is no longer able to deliver the release;
//   * while a close waits (setOverlayReleaseActive), presses stop producing
//     actions — no navigation, no capture — and the physical state keeps being
//     tracked, because the wait is observing exactly that release.
//
// What this does NOT claim: anything about a real game's reception. That is the
// physical acceptance work; here the engine's half of the contract is fixed.
#include "config/ConfigManager.h"
#include "input/BindingRuntime.h"
#include "input/InputEngine.h"
#include "input/XInputDevice.h"
#include "storage/CaptureDatabase.h"

#include <QTemporaryDir>
#include <QTest>

using namespace ModernInput;

namespace {
quint32 buttonBit(Gamepad::Button button)
{
    return 1u << button;
}
}  // namespace

// The production XInput edge publisher without loading the system DLL — the same
// double the preset-switch suite drives — plus the raw press/release seam every
// backend publishes through, so the test can name the control it is holding.
class SyntheticXInput final : public XInputDevice
{
public:
    using XInputDevice::XInputDevice;
    void buttons(quint32 mask) { setSlotState(0, mask, true); }
    void press(const QString& controlId) { publishControlPressed(controlId, profile()); }
    void release(const QString& controlId) { publishControlReleased(controlId, profile()); }
};

class InputReleaseHandoffTest : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void rawEdgesDecideWhatIsHeld();
    void aDeviceThatGoesAwayCannotHoldAnything();
    void suppressionStopsActionsAndKeepsTracking();

private:
    BindingRuntime& rt() { return *engine->m_runtime; }

    std::unique_ptr<QTemporaryDir> dir;
    std::unique_ptr<ConfigManager> config;
    std::unique_ptr<CaptureDatabase> db;
    std::unique_ptr<InputEngine> engine;
    SyntheticXInput* pad = nullptr;
    QString profile;
};

void InputReleaseHandoffTest::init()
{
    dir = std::make_unique<QTemporaryDir>();
    QVERIFY(dir->isValid());
    config = std::make_unique<ConfigManager>(dir->filePath("config.json"));
    db = std::make_unique<CaptureDatabase>(dir->filePath("test.db"));
    QVERIFY(db->open());
    QVERIFY(db->clearAllBindingOverrides());
    engine = std::make_unique<InputEngine>(config.get(), db.get(), nullptr);

    auto synthetic = std::make_unique<SyntheticXInput>();
    pad = synthetic.get();
    engine->m_xinputPad = pad;
    engine->attachGamepad(std::move(synthetic), QStringLiteral("Synthetic XInput"));
    pad->buttons(0);
    pad->setKnownDeviceIdentity(0, "endpoint-a", "2dc8:6001");
    engine->observeLegacyBackend(pad, pad->profile());
    profile = engine->canonicalProfile(pad, QStringLiteral("xinput.slot0"));
    QVERIFY(!profile.isEmpty());

    engine->activateBackend(pad, QStringLiteral("test setup"));
    QCOMPARE(engine->m_activeBackend, static_cast<Gamepad*>(pad));
    rt().reload();
    engine->m_lastControllerRoute = profile;

    pad->buttons(0);   // settle: nothing held when a case starts
    QCOMPARE(engine->sampleNeutralPadState().heldControls, 0);
}

void InputReleaseHandoffTest::cleanup()
{
    engine.reset();
    db.reset();
    config.reset();
    dir.reset();
}

void InputReleaseHandoffTest::rawEdgesDecideWhatIsHeld()
{
    ModernInput::NeutralHandoffSample sample = engine->sampleNeutralPadState();
    QCOMPARE(sample.heldControls, 0);
    QVERIFY(sample.heldSummary.isEmpty());
    QVERIFY(!sample.overlayActionsQuiesced);
    QVERIFY(sample.attachedDevices >= 1);

    // A press through the production XInput publisher: the engine sees the edge
    // whoever published it.
    pad->buttons(buttonBit(Gamepad::Cross));
    sample = engine->sampleNeutralPadState();
    QCOMPARE(sample.heldControls, 1);
    const QString publishedId = sample.heldSummary;
    // Canonical GameHQ control id — never a device path or a serial.
    QVERIFY2(publishedId.startsWith(QStringLiteral("gamepad.")), qPrintable(publishedId));
    // Neutral by the shared definition only once it is released — and only for
    // the sample the handoff's own poll would take (quiesced input path).
    ModernInput::NeutralHandoffSample asHandoffSees = sample;
    asHandoffSees.overlayActionsQuiesced = true;
    const ModernInput::NeutralHandoffDecision held = decideNeutralHandoff(asHandoffSees);
    QVERIFY(!held.neutral);
    QVERIFY2(held.reason.contains(publishedId), qPrintable(held.reason));
    pad->buttons(0);
    QCOMPARE(engine->sampleNeutralPadState().heldControls, 0);

    // Two held at once, published by name so the ids are spelled out: the
    // handoff waits for BOTH releases, and the summary names what it waits for.
    pad->press(ControlId::FaceSouth);
    pad->press(ControlId::ShoulderLeft);
    sample = engine->sampleNeutralPadState();
    QCOMPARE(sample.heldControls, 2);
    QVERIFY2(sample.heldSummary.contains(ControlId::FaceSouth), qPrintable(sample.heldSummary));
    QVERIFY2(sample.heldSummary.contains(ControlId::ShoulderLeft), qPrintable(sample.heldSummary));

    pad->release(ControlId::ShoulderLeft);
    sample = engine->sampleNeutralPadState();
    QCOMPARE(sample.heldControls, 1);
    QCOMPARE(sample.heldSummary, ControlId::FaceSouth);

    pad->release(ControlId::FaceSouth);
    sample = engine->sampleNeutralPadState();
    QCOMPARE(sample.heldControls, 0);
    sample.overlayActionsQuiesced = true;   // what the handoff's next poll sees
    QVERIFY(decideNeutralHandoff(sample).neutral);
}

void InputReleaseHandoffTest::aDeviceThatGoesAwayCannotHoldAnything()
{
    pad->buttons(buttonBit(Gamepad::Cross));
    QCOMPARE(engine->sampleNeutralPadState().heldControls, 1);

    // The controller disappears mid-hold. It can no longer deliver the release,
    // so a close waiting for neutral must not be made to wait for a ghost.
    engine->removeLegacyBackend(pad, QString());

    const ModernInput::NeutralHandoffSample sample = engine->sampleNeutralPadState();
    QCOMPARE(sample.heldControls, 0);
    QVERIFY(sample.heldSummary.isEmpty());
}

void InputReleaseHandoffTest::suppressionStopsActionsAndKeepsTracking()
{
    // Baseline: with no close in flight a press is delivered as usual.
    const QString idle = engine->lastInput();
    pad->buttons(buttonBit(Gamepad::Cross));
    const QString delivered = engine->lastInput();
    QVERIFY2(delivered != idle, qPrintable(delivered));
    pad->buttons(0);

    // A close starts waiting: the overlay input path goes quiet.
    engine->setOverlayReleaseActive(true);
    QVERIFY(engine->sampleNeutralPadState().overlayActionsQuiesced);

    pad->buttons(buttonBit(Gamepad::Cross));
    QCOMPARE(engine->lastInput(), delivered);   // no action fired ...
    QCOMPARE(engine->sampleNeutralPadState().heldControls, 1);   // ... but it IS held
    QVERIFY(!decideNeutralHandoff(engine->sampleNeutralPadState()).neutral);

    // The release still arrives, which is the whole point: the wait can see it.
    pad->buttons(0);
    const ModernInput::NeutralHandoffSample released = engine->sampleNeutralPadState();
    QCOMPARE(released.heldControls, 0);
    QVERIFY(decideNeutralHandoff(released).neutral);

    // Delivery resumes once the close is done with the pad. A DIFFERENT control,
    // so the assertion cannot pass by repetition.
    engine->setOverlayReleaseActive(false);
    QVERIFY(!engine->sampleNeutralPadState().overlayActionsQuiesced);
    pad->buttons(buttonBit(Gamepad::L1));
    QVERIFY2(engine->lastInput() != delivered, qPrintable(engine->lastInput()));
    pad->buttons(0);
}

QTEST_MAIN(InputReleaseHandoffTest)
#include "tst_inputreleasehandoff.moc"
