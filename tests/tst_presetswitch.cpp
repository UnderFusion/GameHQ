#include "input/InputEngine.h"
#include "input/XInputDevice.h"
#include "input/WinMMDevice.h"
#include "input/DualSenseDevice.h"
#include "gameinput/GameInputRouter.h"
#include "input/BindingRuntime.h"
#include "input/BindingResolver.h"
#include "input/MappingAssignmentResolver.h"
#include "input/ControllerArbitration.h"
#include "config/ConfigManager.h"
#include "config/ConfigKeys.h"
#include "storage/CaptureDatabase.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace ModernInput;

namespace {

const QString kController = QStringLiteral("controller");
const QString kKeyboard = QStringLiteral("keyboard");
const QString kMouse = QStringLiteral("mouse");
const QChar kUnit(0x1f);

QString chainKey(const QString& group, const QString& profile)
{
    return group + kUnit + profile;
}

QString gateKey(const QString& group, const QString& profile, const QString& control)
{
    return group + kUnit + profile + kUnit + control;
}

quint32 buttonBit(Gamepad::Button button)
{
    return 1u << button;
}

MappingPresetRow presetRow(const QString& actionId, const QString& trigger,
                           const QString& activation = QStringLiteral("press"),
                           int tapCount = 1, int slot = 1)
{
    MappingPresetRow row;
    row.actionId = actionId;
    row.slot = slot;
    row.triggerCode = trigger;
    row.activation = activation;
    row.tapCount = tapCount;
    return row;
}

} // namespace

// Drives the production XInput edge publisher without loading the system DLL.
class SyntheticXInput final : public XInputDevice
{
public:
    using XInputDevice::XInputDevice;
    void buttons(quint32 mask) { setSlotState(0, mask, true); }
};

// The low-priority legacy provider path, driven through the same Gamepad edge
// contract the real backend uses.
class SyntheticWinMM final : public WinMMDevice
{
public:
    using WinMMDevice::WinMMDevice;

    ControlId::DeviceProfile profile() const override
    {
        return {QStringLiteral("WinMM joystick"), QStringLiteral("winmm.slot0"),
                ControlId::ControllerFamily::Xbox, QStringLiteral("Synthetic WinMM (test)"),
                {}, {}};
    }

    void arrive() { emit connected(true); }

    void edge(bool down, const QString& control = ControlId::FaceSouth)
    {
        if (down)
            publishControlPressed(control, profile());
        else
            publishControlReleased(control, profile());
    }
};

// cpo-p05: changing the resolved preset must never let an input that began
// under the old mapping complete under the new one, and a refresh that resolves
// to the same winner with the same content must not disturb an active gesture.
class PresetSwitchTest : public QObject
{
    Q_OBJECT
private slots:
    void init();
    void cleanup();

    void holdAcrossSwitchIsCancelledAndGated();
    void tapCandidateAcrossSwitchNeverArrives();
    void chordPartlyHeldAcrossSwitchCannotComplete();
    void pendingProviderPressIsClearedAndNeverReplays();
    void pendingProviderPressAlreadyReleasedLeavesNoGate();
    void customPresetThenBuiltinThenCustom();
    void controllerAssignmentChangeSwitchesTable();
    void gameKeySeamStartChangeExit();
    void gamePresetOverridesBridgeAndBridgeReturns();
    void materializedChainIsNotFlattenedByGroupDefault();
    void emptyPresetIsTheDefaultsOnlyOptOut();
    void sameWinnerSameTableIsATrueNoOp();
    void rewrittenPresetRowsAreARealSwitch();
    void repeatedIdenticalRefreshesAreIdempotent();
    void releaseGatesArePerLogicalRoute();
    void invalidPresetRowsKeepExistingValidation();
    void unrelatedOverlayStateSurvivesSwitch();
    void gatedReleaseClearsThePhysicalHold();
    void otherRouteGestureSurvivesSwitch();
    void keyboardGestureSurvivesControllerSwitch();
    void providerCandidateOnAnotherRouteSurvives();
    void inheritedSourceTransitionsAreRealSwitches();
    void materializedFallbackIsARealSwitch();
    void builtinChainRefreshesAreNoOps();
    void firstGameInputPressSeesItsAssignedPreset();
    void firstRawHidPressSeesItsAssignedPreset();
    void knownRoutePressDoesNotRePlan();

private:
    BindingRuntime& rt() { return *engine->m_runtime; }

    QString makePreset(const QString& group, const QString& name,
                       const QVector<MappingPresetRow>& rows)
    {
        return db->createMappingPreset(group, name, rows);
    }

    void assignGroupDefault(const QString& group, const QString& presetId)
    {
        QVERIFY(db->setMappingAssignment(group, QStringLiteral("group_default"), QString(), presetId));
    }

    void changeGroupDefault(const QString& group, const QString& presetId)
    {
        assignGroupDefault(group, presetId);
        engine->refreshResolvedPreset();
    }

    void addDeviceRow(const QString& group, const QString& profile, const QString& actionId,
                      const QString& trigger, const QString& activation = QStringLiteral("press"),
                      int tapCount = 1)
    {
        QVERIFY(db->upsertBindingOverride({group, profile, actionId, 1, trigger, activation, 0,
                                           false, tapCount}));
        rt().reload();
    }

    bool tableBinds(const QString& group, const QString& profile, const QString& actionId,
                    const QString& trigger)
    {
        for (const BindingResolver::Binding& binding : rt().effectiveBindings(group, profile)) {
            if (binding.actionId == actionId && binding.triggerCode == trigger)
                return true;
        }
        return false;
    }

    bool gateArmed(const QString& group, const QString& profile, const QString& control)
    {
        return engine->mappingArmedReleaseGates().contains(gateKey(group, profile, control));
    }

    // A real release edge for a route driven directly through deliverPress().
    void releaseRoute(const QString& routeProfile, const QString& control)
    {
        rt().release(kController, routeProfile, control);
    }

    std::unique_ptr<ConfigManager> config;
    std::unique_ptr<CaptureDatabase> db;
    std::unique_ptr<InputEngine> engine;
    SyntheticXInput* pad = nullptr;
    SyntheticWinMM* winmmPad = nullptr;
    QString profile;
    QString winmmProfile;
    // One fresh sandbox per test: assignments and presets must never leak from
    // one case into the next.
    std::unique_ptr<QTemporaryDir> dir;
};

void PresetSwitchTest::init()
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
    engine->attachGamepad(std::move(synthetic), "Synthetic XInput");
    pad->buttons(0);
    pad->setKnownDeviceIdentity(0, "endpoint-a", "2dc8:6001");
    engine->observeLegacyBackend(pad, pad->profile());
    profile = engine->canonicalProfile(pad, "xinput.slot0");
    QVERIFY(!profile.isEmpty());

    auto winmm = std::make_unique<SyntheticWinMM>();
    winmmPad = winmm.get();
    engine->m_winmmPad = winmmPad;
    engine->attachGamepad(std::move(winmm), "Synthetic WinMM");
    winmmPad->arrive();
    engine->observeLegacyBackend(winmmPad, winmmPad->profile());
    winmmProfile = engine->canonicalProfile(winmmPad, "winmm.slot0");
    QVERIFY(!winmmProfile.isEmpty());
    QVERIFY(winmmProfile != profile);

    engine->activateBackend(pad, QStringLiteral("test setup"));
    QCOMPARE(engine->m_activeBackend, static_cast<Gamepad*>(pad));

    rt().reload();
    rt().setDefaultHoldMs(250);
    rt().setTiming({60, 120, 250});   // multi-tap, chord window, default hold
    engine->m_lastControllerRoute = profile;
}

void PresetSwitchTest::cleanup()
{
    engine.reset();
    db.reset();
    config.reset();
    dir.reset();
}

// A hold that began under table A never fires once B is selected, and the
// control that was physically down is inert under B until it is released and
// pressed again.
void PresetSwitchTest::holdAcrossSwitchIsCancelledAndGated()
{
    const QString holdPreset = makePreset(kController, QStringLiteral("Hold A"),
                                          {presetRow(QStringLiteral("global.save_replay"),
                                                     ControlId::FaceSouth,
                                                     QStringLiteral("hold"))});
    const QString pressPreset = makePreset(kController, QStringLiteral("Press B"),
                                           {presetRow(QStringLiteral("global.screenshot"),
                                                      ControlId::FaceSouth)});
    QVERIFY(!holdPreset.isEmpty());
    QVERIFY(!pressPreset.isEmpty());

    assignGroupDefault(kController, holdPreset);
    engine->refreshResolvedPreset();
    QCOMPARE(engine->mappingInstalledPresetId(kController, profile), holdPreset);
    QCOMPARE(engine->mappingEffectiveSource(kController, profile), QStringLiteral("group_default"));

    QSignalSpy replay(engine.get(), &InputEngine::replayRequested);
    QSignalSpy shots(engine.get(), &InputEngine::screenshotRequested);

    pad->buttons(buttonBit(Gamepad::Cross));
    QTest::qWait(20);
    QCOMPARE(replay.count(), 0);

    // Switch while the button is still physically down.
    changeGroupDefault(kController, pressPreset);
    QCOMPARE(engine->mappingInstalledPresetId(kController, profile), pressPreset);
    QVERIFY(gateArmed(kController, profile, ControlId::FaceSouth));

    QTest::qWait(350);
    QCoreApplication::processEvents();
    QCOMPARE(replay.count(), 0);   // the old hold never fires

    // A mirrored duplicate press for the held control must not act under B.
    engine->deliverPress(pad, ControlId::FaceSouth, 0, QStringLiteral("xinput.slot0"));
    QCoreApplication::processEvents();
    QCOMPARE(shots.count(), 0);

    // A real release clears the gate, and only then does the control work again.
    pad->buttons(0);
    QVERIFY(!gateArmed(kController, profile, ControlId::FaceSouth));
    pad->buttons(buttonBit(Gamepad::Cross));
    QCOMPARE(shots.count(), 1);
    pad->buttons(0);
}

// A pending tap candidate from the old table is invalidated by the switch; the
// control was already released, so it must not be release-gated either.
void PresetSwitchTest::tapCandidateAcrossSwitchNeverArrives()
{
    const QString tapPreset = makePreset(kController, QStringLiteral("Tap A"),
                                         {presetRow(QStringLiteral("global.save_replay"),
                                                    ControlId::FaceSouth,
                                                    QStringLiteral("tap"), 2)});
    const QString pressPreset = makePreset(kController, QStringLiteral("Press B"),
                                           {presetRow(QStringLiteral("global.screenshot"),
                                                      ControlId::FaceSouth)});
    QVERIFY(!tapPreset.isEmpty());
    QVERIFY(!pressPreset.isEmpty());

    assignGroupDefault(kController, tapPreset);
    engine->refreshResolvedPreset();

    QSignalSpy replay(engine.get(), &InputEngine::replayRequested);
    QSignalSpy shots(engine.get(), &InputEngine::screenshotRequested);

    pad->buttons(buttonBit(Gamepad::Cross));
    pad->buttons(0);   // tap candidate is now waiting for a possible second tap

    changeGroupDefault(kController, pressPreset);

    QTest::qWait(200);
    QCoreApplication::processEvents();
    QCOMPARE(replay.count(), 0);                          // delayed A action never arrives
    QVERIFY(!gateArmed(kController, profile, ControlId::FaceSouth));   // released before the switch

    pad->buttons(buttonBit(Gamepad::Cross));
    QCOMPARE(shots.count(), 1);   // the new table acts on the next real press
    pad->buttons(0);
}

// The first constituent of a chord is held across the switch: the old chord is
// dead, the new one cannot complete from a stale held state, and both work
// again after a clean release and press.
void PresetSwitchTest::chordPartlyHeldAcrossSwitchCannotComplete()
{
    const QString chord = TriggerSpec::orderedChord(ControlId::FaceSouth,
                                                    ControlId::ShoulderLeft).serialize();
    const QString chordA = makePreset(kController, QStringLiteral("Chord A"),
                                      {presetRow(QStringLiteral("global.save_replay"), chord)});
    const QString chordB = makePreset(kController, QStringLiteral("Chord B"),
                                      {presetRow(QStringLiteral("global.screenshot"), chord)});
    QVERIFY(!chordA.isEmpty());
    QVERIFY(!chordB.isEmpty());

    assignGroupDefault(kController, chordA);
    engine->refreshResolvedPreset();

    QSignalSpy replay(engine.get(), &InputEngine::replayRequested);
    QSignalSpy shots(engine.get(), &InputEngine::screenshotRequested);

    pad->buttons(buttonBit(Gamepad::Cross));   // opens the chord candidate in A
    changeGroupDefault(kController, chordB);
    QVERIFY(gateArmed(kController, profile, ControlId::FaceSouth));

    pad->buttons(buttonBit(Gamepad::Cross) | buttonBit(Gamepad::L1));
    QTest::qWait(200);
    QCoreApplication::processEvents();
    QCOMPARE(replay.count(), 0);
    QCOMPARE(shots.count(), 0);

    pad->buttons(0);   // clean release: gate cleared for both constituents
    QVERIFY(!gateArmed(kController, profile, ControlId::FaceSouth));

    pad->buttons(buttonBit(Gamepad::Cross));
    pad->buttons(buttonBit(Gamepad::Cross) | buttonBit(Gamepad::L1));
    QCOMPARE(shots.count(), 1);
    pad->buttons(0);
}

// A buffered provider-candidate press born before the switch is cancelled, and
// it can never be replayed into the new table.
void PresetSwitchTest::pendingProviderPressIsClearedAndNeverReplays()
{
    const QString presetA = makePreset(kController, QStringLiteral("Pending A"),
                                       {presetRow(QStringLiteral("global.save_replay"),
                                                  ControlId::FaceSouth)});
    const QString presetB = makePreset(kController, QStringLiteral("Pending B"),
                                       {presetRow(QStringLiteral("global.screenshot"),
                                                  ControlId::FaceSouth)});
    QVERIFY(!presetA.isEmpty());
    QVERIFY(!presetB.isEmpty());
    assignGroupDefault(kController, presetA);
    engine->refreshResolvedPreset();

    // Keep the active backend alive, then let a second backend produce a press
    // outside the mirror window but inside the takeover silence: the press is
    // buffered as a candidate, not delivered.
    pad->buttons(buttonBit(Gamepad::Cross));
    pad->buttons(0);
    QTest::qWait(ControllerArbitration::BackendDuplicateWindowMs + 40);
    winmmPad->edge(true);
    QVERIFY(engine->m_pending.source == static_cast<Gamepad*>(winmmPad));
    QVERIFY(!engine->m_pending.controlId.isEmpty());

    QSignalSpy replay(engine.get(), &InputEngine::replayRequested);
    QSignalSpy shots(engine.get(), &InputEngine::screenshotRequested);

    changeGroupDefault(kController, presetB);

    QVERIFY(!engine->m_pending.source);   // buffered press cancelled by the switch
    QVERIFY(gateArmed(kController, winmmProfile, ControlId::FaceSouth));

    QTest::qWait(ControllerArbitration::BackendCandidateConfirmMs + 120);
    QCoreApplication::processEvents();
    QCOMPARE(replay.count(), 0);   // never replayed into the new table
    QCOMPARE(shots.count(), 0);
    QCOMPARE(engine->m_activeBackend, static_cast<Gamepad*>(pad));

    winmmPad->edge(false);
    QVERIFY(!gateArmed(kController, winmmProfile, ControlId::FaceSouth));
}

// The release of a buffered press was already observed: the switch clears the
// candidate without arming a gate, because nothing is physically down.
void PresetSwitchTest::pendingProviderPressAlreadyReleasedLeavesNoGate()
{
    const QString presetA = makePreset(kController, QStringLiteral("Released A"),
                                       {presetRow(QStringLiteral("global.save_replay"),
                                                  ControlId::FaceSouth)});
    const QString presetB = makePreset(kController, QStringLiteral("Released B"),
                                       {presetRow(QStringLiteral("global.screenshot"),
                                                  ControlId::FaceSouth)});
    QVERIFY(!presetA.isEmpty());
    QVERIFY(!presetB.isEmpty());
    assignGroupDefault(kController, presetA);
    engine->refreshResolvedPreset();

    pad->buttons(buttonBit(Gamepad::Cross));
    pad->buttons(0);
    QTest::qWait(ControllerArbitration::BackendDuplicateWindowMs + 40);
    winmmPad->edge(true);
    QVERIFY(engine->m_pending.source != nullptr);
    winmmPad->edge(false);
    QVERIFY(engine->m_pending.released);

    QSignalSpy replay(engine.get(), &InputEngine::replayRequested);
    changeGroupDefault(kController, presetB);

    QVERIFY(!engine->m_pending.source);
    QVERIFY(engine->mappingArmedReleaseGates().isEmpty());
    QTest::qWait(ControllerArbitration::BackendCandidateConfirmMs + 120);
    QCoreApplication::processEvents();
    QCOMPARE(replay.count(), 0);
}

// Custom preset -> shipped defaults -> the same custom preset again.
void PresetSwitchTest::customPresetThenBuiltinThenCustom()
{
    const QString preset = makePreset(kController, QStringLiteral("Custom"),
                                      {presetRow(QStringLiteral("global.save_replay"),
                                                 ControlId::FaceSouth,
                                                 QStringLiteral("hold"))});
    QVERIFY(!preset.isEmpty());
    QVERIFY(db->setMappingAssignment(kController, QStringLiteral("group_default"), QString(), preset));
    engine->refreshResolvedPreset();
    QCOMPARE(engine->mappingInstalledPresetId(kController, profile), preset);
    QVERIFY(tableBinds(kController, profile, QStringLiteral("global.save_replay"),
                       ControlId::FaceSouth));

    QVERIFY(db->clearMappingAssignment(kController, QStringLiteral("group_default"), QString()));
    engine->refreshResolvedPreset();
    QVERIFY(engine->mappingInstalledPresetId(kController, profile).isEmpty());
    QCOMPARE(engine->mappingEffectiveSource(kController, profile), QStringLiteral("builtin"));
    QVERIFY(!tableBinds(kController, profile, QStringLiteral("global.save_replay"),
                        ControlId::FaceSouth));

    assignGroupDefault(kController, preset);
    engine->refreshResolvedPreset();
    QCOMPARE(engine->mappingInstalledPresetId(kController, profile), preset);
    QVERIFY(tableBinds(kController, profile, QStringLiteral("global.save_replay"),
                       ControlId::FaceSouth));
}

// Changing a typed controller assignment changes the table that serves.
void PresetSwitchTest::controllerAssignmentChangeSwitchesTable()
{
    const QString first = makePreset(kController, QStringLiteral("Assigned 1"),
                                    {presetRow(QStringLiteral("global.save_replay"),
                                               ControlId::FaceSouth)});
    const QString second = makePreset(kController, QStringLiteral("Assigned 2"),
                                      {presetRow(QStringLiteral("global.screenshot"),
                                                 ControlId::FaceSouth)});
    QVERIFY(!first.isEmpty());
    QVERIFY(!second.isEmpty());

    QVERIFY(db->setMappingAssignment(kController, QStringLiteral("legacy_slot"),
                                     QStringLiteral("xinput.slot0"), first));
    engine->refreshResolvedPreset();
    QCOMPARE(engine->mappingInstalledPresetId(kController, profile), first);
    QCOMPARE(engine->mappingEffectiveSource(kController, profile), QStringLiteral("controller"));
    QVERIFY(tableBinds(kController, profile, QStringLiteral("global.save_replay"),
                       ControlId::FaceSouth));

    QVERIFY(db->setMappingAssignment(kController, QStringLiteral("legacy_slot"),
                                     QStringLiteral("xinput.slot0"), second));
    engine->refreshResolvedPreset();
    QCOMPARE(engine->mappingInstalledPresetId(kController, profile), second);
    QVERIFY(tableBinds(kController, profile, QStringLiteral("global.screenshot"),
                       ControlId::FaceSouth));
    QVERIFY(!tableBinds(kController, profile, QStringLiteral("global.save_replay"),
                        ControlId::FaceSouth));
}

// The game seam: start -> change -> exit restores the previous effective source.
void PresetSwitchTest::gameKeySeamStartChangeExit()
{
    const QString groupPreset = makePreset(kController, QStringLiteral("Group"),
                                           {presetRow(QStringLiteral("global.save_replay"),
                                                      ControlId::FaceSouth)});
    const QString gameOne = makePreset(kController, QStringLiteral("Game One"),
                                       {presetRow(QStringLiteral("global.screenshot"),
                                                  ControlId::FaceSouth)});
    const QString gameTwo = makePreset(kController, QStringLiteral("Game Two"),
                                       {presetRow(QStringLiteral("global.toggle_overlay"),
                                                  ControlId::FaceSouth)});
    QVERIFY(!groupPreset.isEmpty());
    QVERIFY(!gameOne.isEmpty());
    QVERIFY(!gameTwo.isEmpty());

    assignGroupDefault(kController, groupPreset);
    engine->refreshResolvedPreset();
    QCOMPARE(engine->mappingInstalledPresetId(kController, profile), groupPreset);

    const QString alpha = QStringLiteral("C:\\Games\\Alpha\\alpha.exe");
    const QString beta = QStringLiteral("C:\\Games\\Beta\\beta.exe");
    QVERIFY(db->setMappingAssignment(kController, QStringLiteral("game"),
                                     MappingAssignmentResolver::canonicalGameKey(alpha), gameOne));
    QVERIFY(db->setMappingAssignment(kController, QStringLiteral("game"),
                                     MappingAssignmentResolver::canonicalGameKey(beta), gameTwo));

    engine->setRunningGameKey(alpha);
    QCOMPARE(engine->mappingInstalledPresetId(kController, profile), gameOne);
    QCOMPARE(engine->mappingEffectiveSource(kController, profile), QStringLiteral("game"));
    QVERIFY(tableBinds(kController, profile, QStringLiteral("global.screenshot"),
                       ControlId::FaceSouth));

    engine->setRunningGameKey(beta);
    QCOMPARE(engine->mappingInstalledPresetId(kController, profile), gameTwo);

    engine->setRunningGameKey(QString());
    QCOMPARE(engine->mappingInstalledPresetId(kController, profile), groupPreset);
    QCOMPARE(engine->mappingEffectiveSource(kController, profile), QStringLiteral("group_default"));
    QVERIFY(tableBinds(kController, profile, QStringLiteral("global.save_replay"),
                       ControlId::FaceSouth));
}

// An explicit game preset temporarily overrides an unresolved migration bridge;
// when the game exits the bridge is the effective source again.
void PresetSwitchTest::gamePresetOverridesBridgeAndBridgeReturns()
{
    // Device-specific legacy rows keep this chain bridge-owned.
    addDeviceRow(kController, profile, QStringLiteral("global.screenshot"),
                 ControlId::FaceSouth);
    QVERIFY(rt().resolver().hasUnretiredSpecificLegacy(kController, profile));

    const QString groupPreset = makePreset(kController, QStringLiteral("Bridge Group"),
                                           {presetRow(QStringLiteral("global.toggle_overlay"),
                                                      ControlId::FaceSouth)});
    const QString gamePreset = makePreset(kController, QStringLiteral("Bridge Game"),
                                          {presetRow(QStringLiteral("global.save_replay"),
                                                     ControlId::FaceSouth)});
    QVERIFY(!groupPreset.isEmpty());
    QVERIFY(!gamePreset.isEmpty());

    // A group default must not flatten unretired device-specific behavior.
    assignGroupDefault(kController, groupPreset);
    engine->refreshResolvedPreset();
    QVERIFY(engine->mappingInstalledPresetId(kController, profile).isEmpty());
    QCOMPARE(engine->mappingEffectiveSource(kController, profile),
             QStringLiteral("migration_bridge"));
    QVERIFY(tableBinds(kController, profile, QStringLiteral("global.screenshot"),
                       ControlId::FaceSouth));

    const QString alpha = QStringLiteral("C:\\Games\\Alpha\\alpha.exe");
    QVERIFY(db->setMappingAssignment(kController, QStringLiteral("game"),
                                     MappingAssignmentResolver::canonicalGameKey(alpha),
                                     gamePreset));
    engine->setRunningGameKey(alpha);
    QCOMPARE(engine->mappingInstalledPresetId(kController, profile), gamePreset);
    QCOMPARE(engine->mappingEffectiveSource(kController, profile), QStringLiteral("game"));

    engine->setRunningGameKey(QString());
    QVERIFY(engine->mappingInstalledPresetId(kController, profile).isEmpty());
    QCOMPARE(engine->mappingEffectiveSource(kController, profile),
             QStringLiteral("migration_bridge"));
    QVERIFY(tableBinds(kController, profile, QStringLiteral("global.screenshot"),
                       ControlId::FaceSouth));
}

// A proven (materialized) chain serves its own table: a group default does not
// flatten it, and the preset still wins once it is the explicit assignment.
void PresetSwitchTest::materializedChainIsNotFlattenedByGroupDefault()
{
    addDeviceRow(kController, profile, QStringLiteral("global.screenshot"),
                 ControlId::FaceSouth);
    const QString migration = makePreset(kController, QStringLiteral("Migration Fold"),
                                         {presetRow(QStringLiteral("global.screenshot"),
                                                    ControlId::FaceSouth)});
    const QString groupDefault = makePreset(kController, QStringLiteral("Group Default"),
                                            {presetRow(QStringLiteral("global.save_replay"),
                                                       ControlId::FaceSouth)});
    QVERIFY(!migration.isEmpty());
    QVERIFY(!groupDefault.isEmpty());

    // The materializer proves a chain equal to its stored rows and activates it;
    // doing that directly pins the promoted state this test is about.
    QVERIFY(rt().resolver().activateMaterializedChain(
        kController, profile, migration,
        {presetRow(QStringLiteral("global.screenshot"), ControlId::FaceSouth)},
        rt().resolver().aliasesFor(profile)));

    assignGroupDefault(kController, groupDefault);
    engine->refreshResolvedPreset();
    QVERIFY(engine->mappingInstalledPresetId(kController, profile).isEmpty());
    QCOMPARE(engine->mappingEffectiveSource(kController, profile), QStringLiteral("materialized"));
    QVERIFY(tableBinds(kController, profile, QStringLiteral("global.screenshot"),
                       ControlId::FaceSouth));

    // The promoted durable assignment, once written, no longer needs the bridge
    // fallback: the preset itself serves.
    QVERIFY(db->setMappingAssignment(kController, QStringLiteral("legacy_slot"),
                                     QStringLiteral("xinput.slot0"), migration));
    engine->refreshResolvedPreset();
    QCOMPARE(engine->mappingInstalledPresetId(kController, profile), migration);
    QCOMPARE(engine->mappingEffectiveSource(kController, profile), QStringLiteral("controller"));
}

// An explicit EMPTY preset is the supported "built-in defaults for this chain"
// choice: the preset exists and contains no rows, so the shipped defaults serve
// and every lower compatibility layer is masked. Clearing an assignment is a
// DIFFERENT operation: it reveals the next currently-authoritative fallback,
// which - while the binding editor still writes plain binding_overrides rows -
// is local_legacy, not builtin. A chain with no local rows resolves to builtin,
// which the switch layer owns as an explicitly prepared shipped-defaults table
// (ownership is never inferred from a non-empty preset id).
void PresetSwitchTest::emptyPresetIsTheDefaultsOnlyOptOut()
{
    addDeviceRow(kController, QString(), QStringLiteral("global.screenshot"),
                 ControlId::FaceSouth);
    QVERIFY(!rt().resolver().hasUnretiredSpecificLegacy(kController, profile));
    const QString empty = makePreset(kController, QStringLiteral("Defaults Only"), {});
    QVERIFY(!empty.isEmpty());

    assignGroupDefault(kController, empty);
    engine->refreshResolvedPreset();
    QCOMPARE(engine->mappingEffectiveSource(kController, profile), QStringLiteral("group_default"));
    QVERIFY(!tableBinds(kController, profile, QStringLiteral("global.screenshot"),
                        ControlId::FaceSouth));   // masked by the empty preset

    // Clearing the assignment reveals the live fallback: the retained local row
    // is still behavior, so the source is local_legacy and the inherited table
    // serves again - clearing never rewrites editor rows into migration records.
    QVERIFY(db->clearMappingAssignment(kController, QStringLiteral("group_default"), QString()));
    engine->refreshResolvedPreset();
    QCOMPARE(engine->mappingEffectiveSource(kController, profile), QStringLiteral("local_legacy"));
    QVERIFY(!rt().hasInstalledPresetTable(kController, profile));   // inherited, not owned
    QVERIFY(tableBinds(kController, profile, QStringLiteral("global.screenshot"),
                       ControlId::FaceSouth));

    // A chain with nothing local at all is builtin: shipped defaults only, and
    // the switch layer OWNS that table even though it has no preset identity.
    engine->refreshResolvedPreset();
    QCOMPARE(engine->mappingEffectiveSource(kKeyboard, QString()), QStringLiteral("builtin"));
    QVERIFY(engine->mappingInstalledPresetId(kKeyboard, QString()).isEmpty());
    QVERIFY(rt().hasInstalledPresetTable(kKeyboard, QString()));
    QCOMPARE(rt().installedPresetSource(kKeyboard, QString()), QStringLiteral("builtin"));
}

// Same winner, same content: a true no-op that leaves an active gesture alone.
void PresetSwitchTest::sameWinnerSameTableIsATrueNoOp()
{
    const QString preset = makePreset(kController, QStringLiteral("Stable"),
                                      {presetRow(QStringLiteral("global.save_replay"),
                                                 ControlId::FaceSouth,
                                                 QStringLiteral("tap")),
                                       presetRow(QStringLiteral("global.toggle_overlay"),
                                                 ControlId::FaceSouth,
                                                 QStringLiteral("tap"), 2)});
    QVERIFY(!preset.isEmpty());
    assignGroupDefault(kController, preset);
    engine->refreshResolvedPreset();
    QCOMPARE(engine->mappingInstalledPresetId(kController, profile), preset);

    QSignalSpy replay(engine.get(), &InputEngine::replayRequested);
    pad->buttons(buttonBit(Gamepad::Cross));
    pad->buttons(0);   // single tap waits for the possible second one

    engine->refreshResolvedPreset();   // identical resolution
    QVERIFY(engine->mappingArmedReleaseGates().isEmpty());

    QTest::qWait(200);
    QCoreApplication::processEvents();
    QCOMPARE(replay.count(), 1);   // the pending tap survived: nothing was invalidated
}

// The same preset id with rewritten rows is a real switch: the new content
// serves and old gesture state is invalidated.
void PresetSwitchTest::rewrittenPresetRowsAreARealSwitch()
{
    const QString preset = makePreset(kController, QStringLiteral("Rewritten"),
                                      {presetRow(QStringLiteral("global.save_replay"),
                                                 ControlId::FaceSouth,
                                                 QStringLiteral("tap")),
                                       presetRow(QStringLiteral("global.toggle_overlay"),
                                                 ControlId::FaceSouth,
                                                 QStringLiteral("tap"), 2)});
    QVERIFY(!preset.isEmpty());
    assignGroupDefault(kController, preset);
    engine->refreshResolvedPreset();

    QSignalSpy replay(engine.get(), &InputEngine::replayRequested);
    QSignalSpy shots(engine.get(), &InputEngine::screenshotRequested);
    pad->buttons(buttonBit(Gamepad::Cross));
    pad->buttons(0);   // tap candidate pending under the old content

    QVERIFY(db->replaceMappingPresetRows(
        preset, {presetRow(QStringLiteral("global.screenshot"), ControlId::FaceSouth)}));
    engine->refreshResolvedPreset();

    QTest::qWait(200);
    QCoreApplication::processEvents();
    QCOMPARE(replay.count(), 0);   // the pending tap was invalidated
    QVERIFY(tableBinds(kController, profile, QStringLiteral("global.screenshot"),
                       ControlId::FaceSouth));

    pad->buttons(buttonBit(Gamepad::Cross));
    QCOMPARE(shots.count(), 1);
    pad->buttons(0);
}

void PresetSwitchTest::repeatedIdenticalRefreshesAreIdempotent()
{
    const QString preset = makePreset(kController, QStringLiteral("Repeat"),
                                      {presetRow(QStringLiteral("global.save_replay"),
                                                 ControlId::FaceSouth,
                                                 QStringLiteral("tap")),
                                       presetRow(QStringLiteral("global.toggle_overlay"),
                                                 ControlId::FaceSouth,
                                                 QStringLiteral("tap"), 2)});
    QVERIFY(!preset.isEmpty());
    assignGroupDefault(kController, preset);
    engine->refreshResolvedPreset();

    QSignalSpy replay(engine.get(), &InputEngine::replayRequested);
    pad->buttons(buttonBit(Gamepad::Cross));
    pad->buttons(0);

    engine->refreshResolvedPreset();
    engine->refreshResolvedPreset();
    engine->refreshResolvedPreset();

    QTest::qWait(200);
    QCoreApplication::processEvents();
    QCOMPARE(replay.count(), 1);
}

// Two controller routes never share a release gate.
void PresetSwitchTest::releaseGatesArePerLogicalRoute()
{
    const QString first = makePreset(kController, QStringLiteral("Route A"),
                                     {presetRow(QStringLiteral("global.save_replay"),
                                                ControlId::FaceSouth)});
    const QString second = makePreset(kController, QStringLiteral("Route B"),
                                      {presetRow(QStringLiteral("global.screenshot"),
                                                 ControlId::FaceSouth)});
    QVERIFY(!first.isEmpty());
    QVERIFY(!second.isEmpty());
    assignGroupDefault(kController, first);
    engine->refreshResolvedPreset();
    QCOMPARE(engine->mappingInstalledPresetId(kController, profile), first);

    QSignalSpy shots(engine.get(), &InputEngine::screenshotRequested);
    pad->buttons(buttonBit(Gamepad::Cross));   // held on the XInput route
    QVERIFY(rt().downControls(kController, profile).contains(ControlId::FaceSouth));

    changeGroupDefault(kController, second);
    QCOMPARE(engine->mappingInstalledPresetId(kController, profile), second);
    QVERIFY(gateArmed(kController, profile, ControlId::FaceSouth));

    // The other route still acts: its gate is its own.
    engine->deliverPress(winmmPad, ControlId::FaceSouth, 0, QStringLiteral("winmm.slot0"));
    QCOMPARE(shots.count(), 1);

    // The gated route stays inert until its own release arrives.
    engine->deliverPress(pad, ControlId::FaceSouth, 0, QStringLiteral("xinput.slot0"));
    QCOMPARE(shots.count(), 1);
    pad->buttons(0);
    QVERIFY(!gateArmed(kController, profile, ControlId::FaceSouth));
}

// Invalid content keeps the existing storage validation semantics; nothing new
// interprets preset rows.
void PresetSwitchTest::invalidPresetRowsKeepExistingValidation()
{
    // A malformed gesture is rejected by the one canonical validator storage and
    // the runtime already share.
    MappingPresetRow broken = presetRow(QStringLiteral("global.save_replay"),
                                        ControlId::FaceSouth,
                                        QStringLiteral("sideways"));
    QVERIFY(makePreset(kController, QStringLiteral("Broken"), {broken}).isEmpty());
    QVERIFY(db->listMappingPresets(kController).isEmpty());

    // A well-formed row naming an action this build has no entry for is accepted
    // by storage and skipped by the existing composition, so it can never fire
    // something nobody asked for.
    const QString unknown = makePreset(kController, QStringLiteral("Unknown Action"),
                                       {presetRow(QStringLiteral("no.such.action"),
                                                  ControlId::FaceSouth),
                                        presetRow(QStringLiteral("global.screenshot"),
                                                  ControlId::ShoulderLeft)});
    QVERIFY(!unknown.isEmpty());
    assignGroupDefault(kController, unknown);
    engine->refreshResolvedPreset();
    QVERIFY(!tableBinds(kController, profile, QStringLiteral("no.such.action"),
                        ControlId::FaceSouth));
    QVERIFY(tableBinds(kController, profile, QStringLiteral("global.screenshot"),
                       ControlId::ShoulderLeft));
}

// A mapping switch must not tear down unrelated overlay/navigation state, and a
// navigation repeat is a press of ONE table: it keeps running when another
// route's table is replaced, and ends the moment its own route changes.
void PresetSwitchTest::unrelatedOverlayStateSurvivesSwitch()
{
    const QString first = makePreset(kController, QStringLiteral("Overlay A"),
                                     {presetRow(QStringLiteral("global.save_replay"),
                                                ControlId::FaceSouth)});
    const QString second = makePreset(kController, QStringLiteral("Overlay B"),
                                      {presetRow(QStringLiteral("global.screenshot"),
                                                 ControlId::FaceSouth)});
    const QString keyboardPreset = makePreset(kKeyboard, QStringLiteral("Keyboard nav"),
                                              {presetRow(QStringLiteral("overlay.navigate_left"),
                                                         QStringLiteral("F9"))});
    QVERIFY(!first.isEmpty());
    QVERIFY(!second.isEmpty());
    QVERIFY(!keyboardPreset.isEmpty());
    // The controller route is switched by an assignment keyed to THAT route, so
    // the keyboard chain provably keeps its own state below.
    QVERIFY(db->setMappingAssignment(kController, QStringLiteral("legacy_slot"),
                                     QStringLiteral("xinput.slot0"), first));
    engine->refreshResolvedPreset();
    QCOMPARE(engine->mappingInstalledPresetId(kController, profile), first);

    engine->setOverlayVisible(true);
    QSignalSpy toggles(engine.get(), &InputEngine::overlayToggleRequested);
    QSignalSpy navigates(engine.get(), &InputEngine::overlayNavigate);
    QVERIFY(engine->m_overlayVisible);
    const bool desktopFocused = engine->m_desktopFocused;

    // The keyboard navigation binding is a plain local row, so the keyboard
    // chain serves it as local_legacy and the repeat below is provably that
    // route's. Production writes go through InputEngine::reloadBindings(); a
    // raw storage write needs the explicit refresh that seam performs.
    addDeviceRow(kKeyboard, QString(), QStringLiteral("overlay.navigate_left"),
                 QStringLiteral("F9"));
    engine->refreshResolvedPreset();

    // A keyboard navigation repeat is running (its route is the keyboard chain).
    QVERIFY(engine->handleKeyPressed(Qt::Key_F9, 0));
    QVERIFY(engine->m_repeatTick->isActive());
    QCOMPARE(engine->m_repeatRouteGroup, QStringLiteral("keyboard"));

    // Replacing the CONTROLLER route's table leaves it running: that repeat is
    // a press of the keyboard table, which did not move.
    QVERIFY(db->setMappingAssignment(kController, QStringLiteral("legacy_slot"),
                                     QStringLiteral("xinput.slot0"), second));
    engine->refreshResolvedPreset();
    QCOMPARE(engine->mappingInstalledPresetId(kController, profile), second);
    QVERIFY(engine->m_repeatTick->isActive());

    QVERIFY(engine->m_overlayVisible);              // overlay stays open
    QCOMPARE(engine->m_desktopFocused, desktopFocused);
    QCOMPARE(toggles.count(), 0);

    // The keyboard chain itself changing is a different matter: the table this
    // repeat belongs to is gone, so the repeat ends with it.
    assignGroupDefault(kKeyboard, keyboardPreset);
    engine->refreshResolvedPreset();
    QCOMPARE(engine->mappingEffectiveSource(kKeyboard, QString()), QStringLiteral("group_default"));
    QVERIFY(!engine->m_repeatTick->isActive());

    engine->setOverlayVisible(false);
}

// A gated release must also clear the recognizer's physical bookkeeping. Before
// it does, a control released after a switch stays "physically down" forever and
// the NEXT switch arms a phantom gate for a button nobody is holding.
void PresetSwitchTest::gatedReleaseClearsThePhysicalHold()
{
    const QString holdPreset = makePreset(kController, QStringLiteral("Hold A"),
                                          {presetRow(QStringLiteral("global.save_replay"),
                                                     ControlId::FaceSouth,
                                                     QStringLiteral("hold"))});
    const QString pressPreset = makePreset(kController, QStringLiteral("Press B"),
                                           {presetRow(QStringLiteral("global.screenshot"),
                                                      ControlId::FaceSouth)});
    const QString thirdPreset = makePreset(kController, QStringLiteral("Press C"),
                                           {presetRow(QStringLiteral("global.save_replay"),
                                                      ControlId::FaceSouth)});
    QVERIFY(!holdPreset.isEmpty());
    QVERIFY(!pressPreset.isEmpty());
    QVERIFY(!thirdPreset.isEmpty());
    assignGroupDefault(kController, holdPreset);
    engine->refreshResolvedPreset();

    pad->buttons(buttonBit(Gamepad::Cross));           // hold -> switch -> release
    QVERIFY(rt().downControls(kController, profile).contains(ControlId::FaceSouth));
    changeGroupDefault(kController, pressPreset);
    QVERIFY(gateArmed(kController, profile, ControlId::FaceSouth));

    pad->buttons(0);   // the gated release closes the gate and completes nothing
    QVERIFY(!gateArmed(kController, profile, ControlId::FaceSouth));
    QVERIFY(rt().downControls(kController, profile).isEmpty());   // up again, for real

    // No repress: another switch must not snapshot this control as down.
    changeGroupDefault(kController, thirdPreset);
    QVERIFY(engine->mappingArmedReleaseGates().isEmpty());
    QVERIFY(rt().downControls(kController, profile).isEmpty());
}

// Route-scoped invalidation: switching controller A's table must leave the
// gesture controller B is in the middle of completely untouched.
void PresetSwitchTest::otherRouteGestureSurvivesSwitch()
{
    const QString routeAHold = makePreset(kController, QStringLiteral("A hold"),
                                          {presetRow(QStringLiteral("global.save_replay"),
                                                     ControlId::FaceSouth,
                                                     QStringLiteral("hold"))});
    const QString routeAPress = makePreset(kController, QStringLiteral("A press"),
                                           {presetRow(QStringLiteral("global.screenshot"),
                                                      ControlId::FaceSouth)});
    const QString routeBHold = makePreset(kController, QStringLiteral("B hold"),
                                          {presetRow(QStringLiteral("global.screenshot"),
                                                     ControlId::FaceSouth,
                                                     QStringLiteral("hold"))});
    QVERIFY(!routeAHold.isEmpty());
    QVERIFY(!routeAPress.isEmpty());
    QVERIFY(!routeBHold.isEmpty());
    QVERIFY(db->setMappingAssignment(kController, QStringLiteral("legacy_slot"),
                                     QStringLiteral("xinput.slot0"), routeAHold));
    QVERIFY(db->setMappingAssignment(kController, QStringLiteral("legacy_slot"),
                                     QStringLiteral("winmm.slot0"), routeBHold));
    engine->refreshResolvedPreset();
    QCOMPARE(engine->mappingInstalledPresetId(kController, profile), routeAHold);

    QSignalSpy replay(engine.get(), &InputEngine::replayRequested);
    QSignalSpy shots(engine.get(), &InputEngine::screenshotRequested);

    // Both pads are holding the same button under their own tables.
    pad->buttons(buttonBit(Gamepad::Cross));
    engine->deliverPress(winmmPad, ControlId::FaceSouth, 0, QStringLiteral("winmm.slot0"));
    QCOMPARE(engine->mappingInstalledPresetId(kController, winmmProfile), routeBHold);
    QVERIFY(rt().downControls(kController, winmmProfile).contains(ControlId::FaceSouth));

    // Route A's table is replaced; route B's is not.
    QVERIFY(db->setMappingAssignment(kController, QStringLiteral("legacy_slot"),
                                     QStringLiteral("xinput.slot0"), routeAPress));
    engine->refreshResolvedPreset();
    QCOMPARE(engine->mappingInstalledPresetId(kController, profile), routeAPress);
    QCOMPARE(engine->mappingInstalledPresetId(kController, winmmProfile), routeBHold);
    QVERIFY(gateArmed(kController, profile, ControlId::FaceSouth));
    QVERIFY(!gateArmed(kController, winmmProfile, ControlId::FaceSouth));
    QVERIFY(rt().downControls(kController, winmmProfile).contains(ControlId::FaceSouth));

    // Route B's hold completes on its own threshold, exactly as if nothing had
    // happened; route A's hold never fires.
    QTest::qWait(350);
    QCoreApplication::processEvents();
    QCOMPARE(shots.count(), 1);
    QCOMPARE(replay.count(), 0);

    pad->buttons(0);
    releaseRoute(winmmProfile, ControlId::FaceSouth);
}

// Route-scoped invalidation across groups: a controller-only switch leaves a
// pending keyboard gesture alone.
void PresetSwitchTest::keyboardGestureSurvivesControllerSwitch()
{
    const QString first = makePreset(kController, QStringLiteral("Controller A"),
                                     {presetRow(QStringLiteral("global.save_replay"),
                                                ControlId::FaceSouth)});
    const QString second = makePreset(kController, QStringLiteral("Controller B"),
                                      {presetRow(QStringLiteral("global.screenshot"),
                                                 ControlId::FaceSouth)});
    QVERIFY(!first.isEmpty());
    QVERIFY(!second.isEmpty());
    addDeviceRow(kKeyboard, QString(), QStringLiteral("global.screenshot"),
                 QStringLiteral("F9"), QStringLiteral("tap"));
    assignGroupDefault(kController, first);
    engine->refreshResolvedPreset();

    QSignalSpy shots(engine.get(), &InputEngine::screenshotRequested);
    QVERIFY(engine->handleKeyPressed(Qt::Key_F9, 0));
    QVERIFY(engine->handleKeyReleased(Qt::Key_F9, 0));   // the tap candidate pends now

    changeGroupDefault(kController, second);             // a controller-only switch

    QTest::qWait(150);   // past the multi-tap window
    QCoreApplication::processEvents();
    QCOMPARE(shots.count(), 1);   // the keyboard tap still completed
}

// The buffered provider candidate is resolved per route too: another route's
// switch leaves it held.
void PresetSwitchTest::providerCandidateOnAnotherRouteSurvives()
{
    const QString first = makePreset(kController, QStringLiteral("A first"),
                                     {presetRow(QStringLiteral("global.save_replay"),
                                                ControlId::FaceSouth,
                                                QStringLiteral("hold"))});
    const QString second = makePreset(kController, QStringLiteral("A second"),
                                      {presetRow(QStringLiteral("global.screenshot"),
                                                 ControlId::FaceSouth)});
    QVERIFY(!first.isEmpty());
    QVERIFY(!second.isEmpty());
    QVERIFY(db->setMappingAssignment(kController, QStringLiteral("legacy_slot"),
                                     QStringLiteral("xinput.slot0"), first));
    engine->refreshResolvedPreset();

    // Keep the active backend alive, then let a second backend produce a press
    // outside the mirror window but inside the takeover silence: the press is
    // buffered as a candidate, not delivered.
    pad->buttons(buttonBit(Gamepad::Cross));
    pad->buttons(0);
    QTest::qWait(ControllerArbitration::BackendDuplicateWindowMs + 40);
    winmmPad->edge(true);   // buffered on route B while route A is active
    QVERIFY(engine->m_pending.source == static_cast<Gamepad*>(winmmPad));

    // Route A only: the candidate belongs to route B and is not touched.
    QVERIFY(db->setMappingAssignment(kController, QStringLiteral("legacy_slot"),
                                     QStringLiteral("xinput.slot0"), second));
    engine->refreshResolvedPreset();
    QVERIFY(engine->m_pending.source == static_cast<Gamepad*>(winmmPad));
    QVERIFY(!gateArmed(kController, winmmProfile, ControlId::FaceSouth));
    QVERIFY(engine->mappingArmedReleaseGates().isEmpty());

    winmmPad->edge(false);
}

// Defect E: any difference in the complete effective state - ownership, source,
// preset id or table content - is a real switch, including between two inherited
// states. The proof is the same rows served by a chain that is no longer the
// same: local_legacy -> builtin must gate a control held across it (the old
// "no preset installed" comparison saw both sides as no-ops), while an
// unchanged complete state stays a true no-op.
void PresetSwitchTest::inheritedSourceTransitionsAreRealSwitches()
{
    addDeviceRow(kController, QString(), QStringLiteral("global.screenshot"),
                 ControlId::FaceSouth);
    engine->refreshResolvedPreset();
    QCOMPARE(engine->mappingEffectiveSource(kController, profile), QStringLiteral("local_legacy"));

    pad->buttons(buttonBit(Gamepad::Cross));
    QVERIFY(rt().downControls(kController, profile).contains(ControlId::FaceSouth));

    // The last local row goes away: the chain is builtin now, which the switch
    // layer owns as an explicitly prepared shipped-defaults table.
    QVERIFY(db->clearAllBindingOverrides());
    rt().reload();
    engine->refreshResolvedPreset();
    QCOMPARE(engine->mappingEffectiveSource(kController, profile), QStringLiteral("builtin"));
    QVERIFY(gateArmed(kController, profile, ControlId::FaceSouth));   // recognized as a switch
    pad->buttons(0);
    QVERIFY(!gateArmed(kController, profile, ControlId::FaceSouth));

    // The same state again is a true no-op: a held control keeps its gesture.
    pad->buttons(buttonBit(Gamepad::Cross));
    engine->refreshResolvedPreset();
    QVERIFY(!gateArmed(kController, profile, ControlId::FaceSouth));
    QVERIFY(rt().downControls(kController, profile).contains(ControlId::FaceSouth));
    pad->buttons(0);
}

// The materialized -> bridge fallback is the other inherited transition: the
// same rows, a different authority behind them.
void PresetSwitchTest::materializedFallbackIsARealSwitch()
{
    addDeviceRow(kController, profile, QStringLiteral("global.screenshot"),
                 ControlId::FaceSouth);
    const QString migration = makePreset(kController, QStringLiteral("Migration Fold"),
                                         {presetRow(QStringLiteral("global.screenshot"),
                                                    ControlId::FaceSouth)});
    QVERIFY(!migration.isEmpty());
    QVERIFY(rt().resolver().activateMaterializedChain(
        kController, profile, migration,
        {presetRow(QStringLiteral("global.screenshot"), ControlId::FaceSouth)},
        rt().resolver().aliasesFor(profile)));
    engine->refreshResolvedPreset();
    QCOMPARE(engine->mappingEffectiveSource(kController, profile), QStringLiteral("materialized"));

    // The proof is withdrawn (no input in between, so the migration promotion
    // path stays out of this): the chain falls back to the bridge and the switch
    // is real, not a silent re-label.
    rt().resolver().deactivateMaterializedChain(kController, profile);
    engine->refreshResolvedPreset();
    QCOMPARE(engine->mappingEffectiveSource(kController, profile), QStringLiteral("migration_bridge"));
    QVERIFY(tableBinds(kController, profile, QStringLiteral("global.screenshot"),
                       ControlId::FaceSouth));
}

// A chain that resolves to builtin is owned by the switch layer, and a refresh
// that resolves to the same complete state must still be a true no-op.
void PresetSwitchTest::builtinChainRefreshesAreNoOps()
{
    engine->refreshResolvedPreset();
    QCOMPARE(engine->mappingEffectiveSource(kController, profile), QStringLiteral("builtin"));
    QVERIFY(rt().hasInstalledPresetTable(kController, profile));
    QVERIFY(engine->mappingInstalledPresetId(kController, profile).isEmpty());

    pad->buttons(buttonBit(Gamepad::Cross));
    QVERIFY(rt().downControls(kController, profile).contains(ControlId::FaceSouth));
    engine->refreshResolvedPreset();
    QVERIFY(!gateArmed(kController, profile, ControlId::FaceSouth));
    QVERIFY(rt().downControls(kController, profile).contains(ControlId::FaceSouth));
    pad->buttons(0);
}

// Defect C: the first press of a GameInput route must already act under its
// assigned winner - the route is planned before the press is dispatched, through
// the same shared seam the legacy pad path uses.
void PresetSwitchTest::firstGameInputPressSeesItsAssignedPreset()
{
    ModernInput::ProviderObservation observation;
    observation.provider = ControllerProvider::GameInput;
    observation.providerDeviceId = QStringLiteral("gi-route-1");
    observation.endpointId = QStringLiteral("gi-route-1");
    observation.displayName = QStringLiteral("GameInput pad");
    const QString logicalId = engine->m_providers.registry().observe(observation);
    QVERIFY(!logicalId.isEmpty());

    const QString preset = makePreset(kController, QStringLiteral("GameInput assigned"),
                                      {presetRow(QStringLiteral("global.screenshot"),
                                                 ControlId::FaceSouth)});
    QVERIFY(!preset.isEmpty());
    QVERIFY(db->setMappingAssignment(kController, QStringLiteral("controller"),
                                     logicalId, preset));
    QVERIFY(engine->mappingEffectiveSource(kController, logicalId).isEmpty());   // untracked

    QSignalSpy shots(engine.get(), &InputEngine::screenshotRequested);
    QVERIFY(QMetaObject::invokeMethod(engine->m_gameInput.get(), "systemControlPressed",
                                      Qt::DirectConnection,
                                      Q_ARG(QString, ControlId::FaceSouth),
                                      Q_ARG(QString, logicalId),
                                      Q_ARG(QString, QStringLiteral("GameInput pad"))));
    QCOMPARE(shots.count(), 1);
    QCOMPARE(engine->mappingEffectiveSource(kController, logicalId), QStringLiteral("controller"));
    QCOMPARE(engine->mappingInstalledPresetId(kController, logicalId), preset);
}

// Defect C, the selective Raw-HID path: same shared seam, same guarantee.
void PresetSwitchTest::firstRawHidPressSeesItsAssignedPreset()
{
    const QString identity = QStringLiteral("hidraw-route-1");
    ModernInput::ProviderObservation observation;
    observation.provider = ControllerProvider::RawHid;
    observation.providerDeviceId = identity;
    observation.endpointId = identity.toLower();
    observation.displayName = QStringLiteral("Raw HID controller");
    observation.capabilities = ControllerCapability::ExtraControls;
    observation.controls.insert(ControlId::PaddleLeft1);
    const QString logicalId = engine->m_providers.registry().observe(observation);
    QVERIFY(!logicalId.isEmpty());

    const QString preset = makePreset(kController, QStringLiteral("Raw HID assigned"),
                                      {presetRow(QStringLiteral("global.screenshot"),
                                                 ControlId::PaddleLeft1)});
    QVERIFY(!preset.isEmpty());
    QVERIFY(db->setMappingAssignment(kController, QStringLiteral("controller"),
                                     logicalId, preset));
    QVERIFY(engine->mappingEffectiveSource(kController, logicalId).isEmpty());

    QSignalSpy shots(engine.get(), &InputEngine::screenshotRequested);
    QVERIFY(QMetaObject::invokeMethod(engine->m_sonyPad, "rawHidControl", Qt::DirectConnection,
                                      Q_ARG(QString, identity),
                                      Q_ARG(QString, ControlId::PaddleLeft1),
                                      Q_ARG(bool, true)));
    QCOMPARE(shots.count(), 1);
    QCOMPARE(engine->mappingEffectiveSource(kController, logicalId), QStringLiteral("controller"));
}

// ...and it stays cheap: an already-tracked route is never re-resolved per
// event. A silent storage write is only picked up by an explicit refresh.
void PresetSwitchTest::knownRoutePressDoesNotRePlan()
{
    const QString planned = makePreset(kController, QStringLiteral("Planned"),
                                       {presetRow(QStringLiteral("global.save_replay"),
                                                  ControlId::FaceSouth)});
    const QString silent = makePreset(kController, QStringLiteral("Silent"),
                                      {presetRow(QStringLiteral("global.screenshot"),
                                                 ControlId::FaceSouth)});
    QVERIFY(!planned.isEmpty());
    QVERIFY(!silent.isEmpty());
    QVERIFY(db->setMappingAssignment(kController, QStringLiteral("legacy_slot"),
                                     QStringLiteral("winmm.slot0"), planned));

    engine->deliverPress(winmmPad, ControlId::FaceSouth, 0, QStringLiteral("winmm.slot0"));
    QCOMPARE(engine->mappingEffectiveSource(kController, winmmProfile), QStringLiteral("controller"));
    QCOMPARE(engine->mappingInstalledPresetId(kController, winmmProfile), planned);
    releaseRoute(winmmProfile, ControlId::FaceSouth);

    // Same route, new assignment, no refresh: the tracked route keeps serving
    // the table it was planned with.
    QVERIFY(db->setMappingAssignment(kController, QStringLiteral("legacy_slot"),
                                     QStringLiteral("winmm.slot0"), silent));
    engine->deliverPress(winmmPad, ControlId::FaceSouth, 0, QStringLiteral("winmm.slot0"));
    QCOMPARE(engine->mappingInstalledPresetId(kController, winmmProfile), planned);
    releaseRoute(winmmProfile, ControlId::FaceSouth);

    // An explicit refresh is what applies the write.
    engine->refreshResolvedPreset();
    QCOMPARE(engine->mappingInstalledPresetId(kController, winmmProfile), silent);
}

QTEST_GUILESS_MAIN(PresetSwitchTest)
#include "tst_presetswitch.moc"
