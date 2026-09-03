#include "input/ActionCatalog.h"
#include "input/BindingEditorModel.h"
#include "input/BindingRelation.h"
#include "input/BindingRuntime.h"
#include "input/ControlId.h"
#include "input/InputDiagnostics.h"
#include "input/RawHidBindingCatalog.h"
#include "input/SelectiveRawHidFallback.h"
#include "storage/CaptureDatabase.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTranslator>
#include <QVariantMap>

namespace
{
QVariantMap rowFor(const BindingEditorModel &editor, const QString &actionId)
{
    for (const QVariant &entry : editor.rows()) {
        const QVariantMap row = entry.toMap();
        if (row.value(QStringLiteral("actionId")).toString() == actionId)
            return row;
    }
    return {};
}

bool hasBinding(const BindingRuntime &runtime, const QString &group, const QString &profile,
                const QString &actionId, int slot, const QString &trigger)
{
    for (const auto &binding : runtime.effectiveBindings(group, profile)) {
        if (binding.actionId == actionId && binding.slot == slot
            && binding.triggerCode == trigger)
            return true;
    }
    return false;
}

BindingResolver::Binding bindingFor(const BindingRuntime &runtime, const QString &group,
                                    const QString &profile, const QString &actionId, int slot)
{
    for (const auto &binding : runtime.effectiveBindings(group, profile)) {
        if (binding.actionId == actionId && binding.slot == slot)
            return binding;
    }
    return {};
}

}

class BindingEditorTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    CaptureDatabase *m_database = nullptr;
    BindingRuntime *m_runtime = nullptr;
    BindingEditorModel *m_editor = nullptr;

    // The press→tap conversion tests predate Guide's tap/hold default pair;
    // each seeds an explicit press override on Guide so the conversion
    // machinery they exercise still has a press row to convert. Not a slot —
    // QTest must not run it as a test of its own.
    void seedGuidePressOverride()
    {
        QVERIFY(m_database->upsertBindingOverride(
            {QStringLiteral("controller"), {}, QStringLiteral("global.toggle_overlay"), 1,
             ControlId::Guide, QStringLiteral("press"), 0, false, 1}));
        m_runtime->reload();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_database = new CaptureDatabase(m_dir.filePath(QStringLiteral("gamehq.db")), this);
        QVERIFY(m_database->open());
        m_runtime = new BindingRuntime(m_database, this);
        m_runtime->reload();
        m_editor = new BindingEditorModel(m_database, m_runtime, [this] {
            m_runtime->reload();
        }, this);
    }

    void init()
    {
        QVERIFY(m_database->clearAllBindingOverrides());
        m_runtime->reload();
        m_runtime->cancelAll();
        m_runtime->setTiming({300, 300, 2000});
        m_editor->cancelCapture();
        m_editor->dismissConflict();
        m_editor->dismissCompatibility();
        m_editor->setDeviceGroup(QStringLiteral("controller"));
        m_editor->setControllerProfile({});
        m_editor->setControllerSpecific(false);
    }

    void cachedPresentationRetranslatesWithoutChangingActionIdentity()
    {
        ActionCatalog::retranslate();
        m_editor->retranslate();
        const auto* screenshot = ActionCatalog::find(QStringLiteral("global.screenshot"));
        QVERIFY(screenshot);
        const int actionCount = ActionCatalog::all().size();
        m_editor->setLastFiredAction(QStringLiteral("global.screenshot"));
        m_editor->openAssignmentEditor(QStringLiteral("global.screenshot"), 1);
        QSignalSpy rowsSpy(m_editor, &BindingEditorModel::rowsChanged);

        const auto verifyLanguage = [&](const QString& catalog, const QString& label,
                                        const QString& description, const QString& scope) {
            QTranslator translator;
            QVERIFY(translator.load(QStringLiteral(":/i18n/") + catalog));
            QVERIFY(QCoreApplication::installTranslator(&translator));
            ActionCatalog::retranslate();
            m_editor->retranslate();

            QCOMPARE(ActionCatalog::all().size(), actionCount);
            QCOMPARE(ActionCatalog::find(QStringLiteral("global.screenshot")), screenshot);
            QCOMPARE(screenshot->label, label);
            QCOMPARE(screenshot->description, description);
            const QVariantMap row = rowFor(*m_editor, QStringLiteral("global.screenshot"));
            QCOMPARE(row.value(QStringLiteral("label")).toString(), screenshot->label);
            QCOMPARE(row.value(QStringLiteral("description")).toString(),
                     screenshot->description);
            QCOMPARE(row.value(QStringLiteral("scope")).toString(), scope);
            QCOMPARE(m_editor->editorActionLabel(), screenshot->label);
            QCOMPARE(m_editor->editorScopeLabel(), scope);
            QCOMPARE(m_editor->lastFiredAction(), screenshot->label);
            QVERIFY(QCoreApplication::removeTranslator(&translator));
        };

        verifyLanguage(QStringLiteral("gamehq_manager_pl_PL.qm"),
                       QStringLiteral("Zrzut ekranu"),
                       QStringLiteral("Zrób zrzut ekranu bieżącej gry."),
                       QStringLiteral("Globalny"));
        verifyLanguage(QStringLiteral("gamehq_manager_zh_Hans.qm"),
                       QStringLiteral("屏幕截图"),
                       QStringLiteral("捕获当前游戏的屏幕截图。"),
                       QStringLiteral("全局"));
        ActionCatalog::retranslate();
        m_editor->retranslate();
        QCOMPARE(screenshot->label, QStringLiteral("Screenshot"));
        QCOMPARE(m_editor->editorActionLabel(), QStringLiteral("Screenshot"));
        QCOMPARE(m_editor->editorScopeLabel(), QStringLiteral("Global"));
        QCOMPARE(m_editor->lastFiredAction(), QStringLiteral("Screenshot"));
        QCOMPARE(rowsSpy.size(), 3);

        m_editor->closeAssignmentEditor();
        m_editor->beginCapture(QStringLiteral("global.screenshot"), 1);
        QTranslator translator;
        QVERIFY(translator.load(QStringLiteral(":/i18n/gamehq_manager_pl_PL.qm")));
        QVERIFY(QCoreApplication::installTranslator(&translator));
        ActionCatalog::retranslate();
        m_editor->retranslate();
        QVERIFY(m_editor->capturePrompt().startsWith(
            QStringLiteral("Naciśnij przycisk kontrolera dla Zrzut ekranu")));
        QVERIFY(QCoreApplication::removeTranslator(&translator));
        ActionCatalog::retranslate();
        m_editor->retranslate();
    }

    void editsBothSlotsPersistsAndResets()
    {
        m_editor->setDeviceGroup(QStringLiteral("keyboard"));
        QVERIFY(!rowFor(*m_editor, QStringLiteral("global.screenshot"))
                      .value(QStringLiteral("modified")).toBool());
        QCOMPARE(rowFor(*m_editor, QStringLiteral("global.screenshot"))
                     .value(QStringLiteral("primaryChangeState")).toString(),
                 QStringLiteral("default"));
        m_editor->beginCapture(QStringLiteral("global.screenshot"), 2);
        QVERIFY(m_editor->captureInput(QStringLiteral("keyboard"),
                                       QStringLiteral("Ctrl+Alt+P"),
                                       QStringLiteral("Ctrl+Alt+P")));

        const QVariantMap edited = rowFor(*m_editor, QStringLiteral("global.screenshot"));
        QCOMPARE(edited.value(QStringLiteral("primary")).toString(),
                 QStringLiteral("Ctrl+Shift+S"));
        QCOMPARE(edited.value(QStringLiteral("secondary")).toString(),
                 QStringLiteral("Ctrl+Alt+P"));
        QVERIFY(edited.value(QStringLiteral("modified")).toBool());
        QCOMPARE(edited.value(QStringLiteral("secondaryChangeState")).toString(),
                 QStringLiteral("added"));
        QCOMPARE(edited.value(QStringLiteral("secondaryStatusLabel")).toString(),
                 QStringLiteral("Added"));

        BindingRuntime reloaded(m_database);
        reloaded.reload();
        QVERIFY(hasBinding(reloaded, QStringLiteral("keyboard"), {},
                           QStringLiteral("global.screenshot"), 2,
                           QStringLiteral("Ctrl+Alt+P")));

        m_editor->clearBinding(QStringLiteral("global.screenshot"), 1);
        QVERIFY(!hasBinding(*m_runtime, QStringLiteral("keyboard"), {},
                            QStringLiteral("global.screenshot"), 1,
                            QStringLiteral("Ctrl+Shift+S")));
        m_editor->resetAction(QStringLiteral("global.screenshot"));
        QVERIFY(hasBinding(*m_runtime, QStringLiteral("keyboard"), {},
                           QStringLiteral("global.screenshot"), 1,
                           QStringLiteral("Ctrl+Shift+S")));
        QCOMPARE(rowFor(*m_editor, QStringLiteral("global.screenshot"))
                     .value(QStringLiteral("secondary")).toString(),
                 QStringLiteral("Unassigned"));
        QVERIFY(!rowFor(*m_editor, QStringLiteral("global.screenshot"))
                      .value(QStringLiteral("modified")).toBool());
    }

    void rawHidAllowlistSurvivesDeviceSpecificReloadAndChords()
    {
        const QString identityA = QStringLiteral("hid.endpoint.persisted-a");
        const QString identityB = QStringLiteral("hid.endpoint.persisted-b");
        const QString identityC = QStringLiteral("hid.endpoint.persisted-c");
        const QString rawA = ControlId::rawHidUsage(identityA, 0x09, 0x15);
        const QString rawB = ControlId::rawHidUsage(identityB, 0x09, 0x16);
        const QString rawC = ControlId::rawHidUsage(identityC, 0x0c, 0x00b5);

        QVERIFY(m_database->upsertBindingOverride(
            {QStringLiteral("controller"), {}, QStringLiteral("global.screenshot"), 2,
             rawA, QStringLiteral("press"), 0, false, 1}));
        QVERIFY(m_database->upsertBindingOverride(
            {QStringLiteral("controller"), QStringLiteral("controller-logical-b"),
             QStringLiteral("global.save_replay"), 1,
             TriggerSpec::orderedChord(rawB, rawC).serialize(),
             QStringLiteral("press"), 0, false, 1}));

        // A fresh runtime models application restart: the catalog must read
        // every profile row, not only the empty-profile effective table.
        BindingRuntime restarted(m_database);
        restarted.reload();
        const QStringList controls =
            ModernInput::persistedRawHidControls(restarted, m_database);
        const QSet<QString> allowlist(controls.cbegin(), controls.cend());
        QVERIFY(allowlist.contains(rawA));
        QVERIFY(allowlist.contains(rawB));
        QVERIFY(allowlist.contains(rawC));

        auto& fallback = ModernInput::SelectiveRawHidFallback::instance();
        fallback.setBoundControls(controls);
        QCOMPARE(fallback.observeUsage(identityA, 0x09, 0x15, true), rawA);
        QCOMPARE(fallback.observeUsage(identityB, 0x09, 0x16, true), rawB);
        QCOMPARE(fallback.observeUsage(identityC, 0x0c, 0x00b5, true), rawC);
        fallback.setBoundControls({});
    }

    void slotStatesUseTheCorrectSharedAndControllerBaseline()
    {
        const QString action = QStringLiteral("desktop.favorite");
        const QString sharedTrigger = ControlId::genericButton(30);
        QVERIFY(m_database->upsertBindingOverride(
            {QStringLiteral("controller"), {}, action, 2, sharedTrigger,
             QStringLiteral("press"), 0, false, 1}));
        m_runtime->reload();

        const ControlId::DeviceProfile pad{
            QStringLiteral("XInput"), QStringLiteral("3537:1004"),
            ControlId::ControllerFamily::Xbox, QStringLiteral("Xbox-compatible controller")};
        m_editor->setControllerProfile(pad);
        m_editor->setControllerSpecific(true);

        QVariantMap row = rowFor(*m_editor, action);
        QCOMPARE(row.value(QStringLiteral("secondaryTrigger")).toString(),
                 QStringLiteral("Button 30"));
        QCOMPARE(row.value(QStringLiteral("secondaryChangeState")).toString(),
                 QStringLiteral("default"));
        QVERIFY(!row.value(QStringLiteral("modified")).toBool());

        const QString ownTrigger = ControlId::genericButton(31);
        QVERIFY(m_database->upsertBindingOverride(
            {QStringLiteral("controller"), pad.fingerprint, action, 2, ownTrigger,
             QStringLiteral("press"), 0, false, 1}));
        m_runtime->reload();
        m_editor->setControllerSpecific(false);
        m_editor->setControllerSpecific(true);
        row = rowFor(*m_editor, action);
        QCOMPARE(row.value(QStringLiteral("secondaryChangeState")).toString(),
                 QStringLiteral("modified"));

        m_editor->clearBinding(action, 2);
        row = rowFor(*m_editor, action);
        QCOMPARE(row.value(QStringLiteral("secondaryChangeState")).toString(),
                 QStringLiteral("removed"));
        QVERIFY(!row.value(QStringLiteral("secondaryAssigned")).toBool());

        m_editor->resetBinding(action, 2);
        row = rowFor(*m_editor, action);
        QCOMPARE(row.value(QStringLiteral("secondaryChangeState")).toString(),
                 QStringLiteral("default"));
        QCOMPARE(row.value(QStringLiteral("secondaryTrigger")).toString(),
                 QStringLiteral("Button 30"));
    }

    void conflictCanBeCanceledOrReplaced()
    {
        m_editor->setDeviceGroup(QStringLiteral("keyboard"));
        m_editor->beginCapture(QStringLiteral("desktop.navigate_down"), 2);
        QVERIFY(m_editor->captureInput(QStringLiteral("keyboard"), QStringLiteral("Up"),
                                       QStringLiteral("Up")));
        QVERIFY(m_editor->conflictPending());
        m_editor->dismissConflict();
        QVERIFY(hasBinding(*m_runtime, QStringLiteral("keyboard"), {},
                           QStringLiteral("desktop.navigate_up"), 1, QStringLiteral("Up")));
        QVERIFY(!hasBinding(*m_runtime, QStringLiteral("keyboard"), {},
                            QStringLiteral("desktop.navigate_down"), 2, QStringLiteral("Up")));

        m_editor->beginCapture(QStringLiteral("desktop.navigate_down"), 2);
        QVERIFY(m_editor->captureInput(QStringLiteral("keyboard"), QStringLiteral("Up"),
                                       QStringLiteral("Up")));
        QVERIFY(m_editor->conflictPending());
        m_editor->confirmConflict();
        QVERIFY(!hasBinding(*m_runtime, QStringLiteral("keyboard"), {},
                            QStringLiteral("desktop.navigate_up"), 1, QStringLiteral("Up")));
        QVERIFY(hasBinding(*m_runtime, QStringLiteral("keyboard"), {},
                           QStringLiteral("desktop.navigate_down"), 2, QStringLiteral("Up")));
    }

    void deviceSpecificOverrideDoesNotLeakAndProfileResetIsScoped()
    {
        const ControlId::DeviceProfile dualSense{
            QStringLiteral("Sony Raw Input"), QStringLiteral("054C:0CE6"),
            ControlId::ControllerFamily::PlayStation, QStringLiteral("DualSense")
        };
        m_editor->setControllerProfile(dualSense);
        m_editor->setControllerSpecific(true);
        QVERIFY(m_editor->controllerSpecific());

        const QString custom = ControlId::genericButton(17);
        m_editor->beginCapture(QStringLiteral("desktop.favorite"), 2);
        QVERIFY(m_editor->captureInput(QStringLiteral("controller"), custom,
                                       QStringLiteral("Button 17")));
        QVERIFY(hasBinding(*m_runtime, QStringLiteral("controller"),
                           QStringLiteral("054C:0CE6"),
                           QStringLiteral("desktop.favorite"), 2, custom));
        QVERIFY(!hasBinding(*m_runtime, QStringLiteral("controller"), {},
                            QStringLiteral("desktop.favorite"), 2, custom));

        m_editor->resetCurrentProfile();
        QVERIFY(!hasBinding(*m_runtime, QStringLiteral("controller"),
                            QStringLiteral("054C:0CE6"),
                            QStringLiteral("desktop.favorite"), 2, custom));
    }

    void mouseInputsPersistWithFriendlyLabels()
    {
        m_editor->setDeviceGroup(QStringLiteral("mouse"));
        m_editor->beginCapture(QStringLiteral("global.screenshot"), 1);
        QVERIFY(m_editor->captureInput(QStringLiteral("mouse"),
                                       QStringLiteral("mouse.middle"),
                                       QStringLiteral("Middle Mouse")));
        QCOMPARE(rowFor(*m_editor, QStringLiteral("global.screenshot"))
                     .value(QStringLiteral("primary")).toString(),
                 QStringLiteral("Middle Mouse"));

        BindingRuntime reloaded(m_database);
        reloaded.reload();
        QVERIFY(hasBinding(reloaded, QStringLiteral("mouse"), {},
                           QStringLiteral("global.screenshot"), 1,
                           QStringLiteral("mouse.middle")));
        m_editor->resetAllBindings();
        QVERIFY(m_database->listBindingOverrides().isEmpty());
    }

    void fixedBackActionsCannotBeCapturedOrCleared()
    {
        const QVariantMap overlayBack = rowFor(*m_editor, QStringLiteral("overlay.back"));
        const QVariantMap desktopBack = rowFor(*m_editor, QStringLiteral("desktop.back"));
        QCOMPARE(overlayBack.value(QStringLiteral("bindable")).toBool(), false);
        QCOMPARE(desktopBack.value(QStringLiteral("bindable")).toBool(), false);

        m_editor->beginCapture(QStringLiteral("desktop.back"), 1);
        QVERIFY(!m_editor->captureActive());
        m_editor->clearBinding(QStringLiteral("desktop.back"), 1);
        QVERIFY(m_database->listBindingOverrides().isEmpty());
        QVERIFY(hasBinding(*m_runtime, QStringLiteral("controller"), {},
                           QStringLiteral("desktop.back"), 1, ControlId::FaceEast));
    }

    void runtimeSeparatesTapHoldAndDoubleTap()
    {
        m_runtime->setDefaultHoldMs(250);
        m_runtime->reload();
        QSignalSpy spy(m_runtime, &BindingRuntime::actionTriggered);

        QVERIFY(m_runtime->press(QStringLiteral("controller"), {}, ControlId::Capture,
                                 ActionCatalog::Scope::Desktop,
                                 ActionCatalog::Scope::Overlay));
        QVERIFY(m_runtime->release(QStringLiteral("controller"), {}, ControlId::Capture));
        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 700);
        QCOMPARE(spy.takeFirst().at(0).toString(), QStringLiteral("global.screenshot"));

        QVERIFY(m_runtime->press(QStringLiteral("controller"), {}, ControlId::Capture,
                                 ActionCatalog::Scope::Desktop,
                                 ActionCatalog::Scope::Overlay));
        QTest::qWait(350);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.takeFirst().at(0).toString(), QStringLiteral("global.save_replay"));
        QVERIFY(m_runtime->release(QStringLiteral("controller"), {}, ControlId::Capture));
        QTest::qWait(350);
        QCOMPARE(spy.count(), 0);

        QVERIFY(m_runtime->press(QStringLiteral("controller"), {}, ControlId::Capture,
                                 ActionCatalog::Scope::Desktop,
                                 ActionCatalog::Scope::Overlay));
        QVERIFY(m_runtime->release(QStringLiteral("controller"), {}, ControlId::Capture));
        QTest::qWait(50);
        QVERIFY(m_runtime->press(QStringLiteral("controller"), {}, ControlId::Capture,
                                 ActionCatalog::Scope::Desktop,
                                 ActionCatalog::Scope::Overlay));
        QVERIFY(m_runtime->release(QStringLiteral("controller"), {}, ControlId::Capture));
        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 300);
        QCOMPARE(spy.takeFirst().at(0).toString(), QStringLiteral("global.toggle_overlay"));
    }

    void playbackCaptureTapDoesNotDoubleFire()
    {
        // When the overlay is open and a clip is focused, the primary scope
        // is Playback.  A Capture tap should emit ONLY playback.frame_grab,
        // not global.screenshot — the contextual Playback binding is an
        // explicit override of the Global one.
        m_runtime->setDefaultHoldMs(250);
        m_runtime->reload();
        QSignalSpy spy(m_runtime, &BindingRuntime::actionTriggered);

        QVERIFY(m_runtime->press(QStringLiteral("controller"), {}, ControlId::Capture,
                                 ActionCatalog::Scope::Playback,
                                 ActionCatalog::Scope::Overlay));
        QVERIFY(m_runtime->release(QStringLiteral("controller"), {}, ControlId::Capture));
        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 700);

        // Settle past the double-tap window so a late second emission would be
        // caught rather than raced past.
        QTest::qWait(350);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.takeFirst().at(0).toString(), QStringLiteral("playback.frame_grab"));
    }

    void playbackCrossOwnsTheWholePressCycle()
    {
        // Playback binds Cross as an immediate Play/Pause press. Desktop also
        // binds the same physical trigger as Confirm tap and Bulk Select hold.
        // The primary scope must own the trigger across gesture kinds so one
        // press cannot pause and then resume on release, or enter bulk mode
        // when held behind the lightbox.
        m_runtime->setDefaultHoldMs(250);
        m_runtime->reload();
        QSignalSpy spy(m_runtime, &BindingRuntime::actionTriggered);

        QVERIFY(m_runtime->press(QStringLiteral("controller"), {}, ControlId::FaceSouth,
                                 ActionCatalog::Scope::Playback,
                                 ActionCatalog::Scope::Desktop));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.takeFirst().at(0).toString(),
                 QStringLiteral("playback.play_pause"));
        // A press-only pattern has already completed and reset, so its release
        // is deliberately unhandled. What matters is that no fallback tap was
        // armed to act on that edge.
        QVERIFY(!m_runtime->release(QStringLiteral("controller"), {},
                                    ControlId::FaceSouth));
        QTest::qWait(350);
        QCOMPARE(spy.count(), 0);

        QVERIFY(m_runtime->press(QStringLiteral("controller"), {}, ControlId::FaceSouth,
                                 ActionCatalog::Scope::Playback,
                                 ActionCatalog::Scope::Desktop));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.takeFirst().at(0).toString(),
                 QStringLiteral("playback.play_pause"));
        QTest::qWait(1100);
        QCOMPARE(spy.count(), 0);
        QVERIFY(!m_runtime->release(QStringLiteral("controller"), {},
                                    ControlId::FaceSouth));
        QTest::qWait(350);
        QCOMPARE(spy.count(), 0);
    }

    // The replay half of the held-press rule (InputEngine::replayPendingPress):
    // a press buffered while a candidate backend was being confirmed is
    // delivered late, with its release immediately behind it. The runtime must
    // still read that as a tap — a delayed press that turned into a hold would
    // save a replay when the user asked for a screenshot.
    void replayedPressAndReleasePairStaysATap()
    {
        m_runtime->setDefaultHoldMs(250);
        m_runtime->reload();
        QSignalSpy spy(m_runtime, &BindingRuntime::actionTriggered);

        // Exactly what the replay does: press, then release with nothing in
        // between, however long the press waited to be delivered.
        QVERIFY(m_runtime->press(QStringLiteral("controller"), {}, ControlId::Capture,
                                 ActionCatalog::Scope::Global));
        QVERIFY(m_runtime->release(QStringLiteral("controller"), {}, ControlId::Capture));
        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 700);

        QTest::qWait(400);   // past both the double-tap and the hold threshold
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.takeFirst().at(0).toString(), QStringLiteral("global.screenshot"));
    }

    void playbackCaptureHoldStillSavesReplay()
    {
        // The tap override must not swallow the other gestures on the same
        // button: holding Capture during playback still saves the replay,
        // because the override is declared for the tap activation only.
        m_runtime->setDefaultHoldMs(250);
        m_runtime->reload();
        QSignalSpy spy(m_runtime, &BindingRuntime::actionTriggered);

        QVERIFY(m_runtime->press(QStringLiteral("controller"), {}, ControlId::Capture,
                                 ActionCatalog::Scope::Playback,
                                 ActionCatalog::Scope::Overlay));
        QTest::qWait(350);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.takeFirst().at(0).toString(), QStringLiteral("global.save_replay"));
        QVERIFY(m_runtime->release(QStringLiteral("controller"), {}, ControlId::Capture));
        QTest::qWait(350);
        QCOMPARE(spy.count(), 0);
    }

    void playbackGuideStillTogglesOverlay()
    {
        // Guide is Global-only and has no Playback counterpart, so it must keep
        // firing while a clip is focused — proof the fix suppresses only the
        // one declared pair, not Global bindings in general.
        m_runtime->reload();
        QSignalSpy spy(m_runtime, &BindingRuntime::actionTriggered);

        QVERIFY(m_runtime->press(QStringLiteral("controller"), {}, ControlId::Guide,
                                 ActionCatalog::Scope::Playback,
                                 ActionCatalog::Scope::Overlay));
        // Guide is a tap binding since the 2 s hold summons the desktop
        // window: the overlay toggle fires on release, not on press.
        QVERIFY(m_runtime->release(QStringLiteral("controller"), {}, ControlId::Guide));
        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 300);
        QCOMPARE(spy.takeFirst().at(0).toString(), QStringLiteral("global.toggle_overlay"));
    }

    void hardConflictOpensDialogAndCanRetryCapture()
    {
        m_editor->setDeviceGroup(QStringLiteral("keyboard"));
        m_editor->beginCapture(QStringLiteral("desktop.navigate_down"), 2);
        QVERIFY(m_editor->captureInput(QStringLiteral("keyboard"), QStringLiteral("Up"),
                                       QStringLiteral("Up")));
        QVERIFY(m_editor->conflictPending());
        QCOMPARE(m_editor->relationKind(), QStringLiteral("hard_conflict"));
        // The blocking tier must not be duplicated into the quiet notice
        // channel; the dialog owns it.
        QVERIFY(m_editor->validationError().isEmpty());

        // "Choose another" re-arms the same action and slot rather than
        // dropping the edit.
        m_editor->retryConflictCapture();
        QVERIFY(!m_editor->conflictPending());
        QVERIFY(m_editor->captureActive());
        m_editor->cancelCapture();
        // The original binding survived the aborted edit.
        QVERIFY(hasBinding(*m_runtime, QStringLiteral("keyboard"), {},
                           QStringLiteral("desktop.navigate_up"), 1, QStringLiteral("Up")));
    }

    void contextOverrideOutranksSharedGestureNotice()
    {
        // Re-confirming Screenshot on the Share button keeps its tap gesture and
        // meets three existing bindings at once: save_replay (hold) and
        // toggle_overlay (double tap) are shared gestures, playback.frame_grab
        // is the declared override. The override is the one that changes what
        // the button does, so it must win regardless of hash iteration order.
        m_editor->setDeviceGroup(QStringLiteral("controller"));
        m_editor->beginCapture(QStringLiteral("global.screenshot"), 1);
        QVERIFY(m_editor->captureInput(QStringLiteral("controller"), ControlId::Capture,
                                       QStringLiteral("Share")));
        QVERIFY(!m_editor->conflictPending());
        QCOMPARE(m_editor->relationKind(), QStringLiteral("context_override"));
    }

    void sharedGestureIsSavedWithAQuietNotice()
    {
        // Same edit with the override binding cleared: what is left on the
        // button are complementary gestures, which are legal and must save with
        // an explanation rather than be refused.
        m_editor->setDeviceGroup(QStringLiteral("controller"));
        m_editor->clearBinding(QStringLiteral("playback.frame_grab"), 1);

        m_editor->beginCapture(QStringLiteral("global.screenshot"), 1);
        QVERIFY(m_editor->captureInput(QStringLiteral("controller"), ControlId::Capture,
                                       QStringLiteral("Share")));
        QVERIFY(!m_editor->conflictPending());
        QCOMPARE(m_editor->relationKind(), QStringLiteral("shared_gesture"));
        QVERIFY(m_editor->relationNotice().contains(QStringLiteral("Tap")));
        QVERIFY(hasBinding(*m_runtime, QStringLiteral("controller"), {},
                           QStringLiteral("global.screenshot"), 1, ControlId::Capture));

        m_editor->dismissRelationNotice();
        QCOMPARE(m_editor->relationKind(), QStringLiteral("none"));
        QVERIFY(m_editor->relationNotice().isEmpty());
    }

    void unsupportedInputIsItsOwnTier()
    {
        // The controlled fixture for the tier no production backend can emit
        // yet: only a GameInput-class backend will know that a control exists
        // but cannot be delivered. Hotkey refusals and failed writes have
        // their own kinds and must never stand in for this one.
        m_editor->reportUnsupportedInput(QStringLiteral("Capture"));
        QCOMPARE(m_editor->relationKind(), QStringLiteral("unsupported_input"));
        // The three failure kinds populate validationError, so QML can style
        // them as errors without inspecting the message text.
        QVERIFY(m_editor->validationError().contains(QStringLiteral("not exposed")));
        m_editor->dismissRelationNotice();
        QVERIFY(m_editor->validationError().isEmpty());
    }

    void refusedChordIsNeverPersisted()
    {
        // Windows owning the chord must abort the whole change: nothing written,
        // the previous shortcut still live, and the reason on screen.
        m_editor->setDeviceGroup(QStringLiteral("keyboard"));
        int applyCalls = 0;
        m_editor->setHotkeyApply([&](const QString &, int, const QString &chord,
                                     QString *reason) {
            ++applyCalls;
            if (chord == QLatin1String("Ctrl+Alt+P")) {
                if (reason)
                    *reason = QStringLiteral("This shortcut is already used by Windows or another application.");
                return false;
            }
            return true;
        });

        m_editor->beginCapture(QStringLiteral("global.screenshot"), 2);
        QVERIFY(m_editor->captureInput(QStringLiteral("keyboard"),
                                       QStringLiteral("Ctrl+Alt+P"),
                                       QStringLiteral("Ctrl+Alt+P")));
        QCOMPARE(applyCalls, 1);
        QVERIFY(m_database->listBindingOverrides().isEmpty());
        QVERIFY(!hasBinding(*m_runtime, QStringLiteral("keyboard"), {},
                            QStringLiteral("global.screenshot"), 2,
                            QStringLiteral("Ctrl+Alt+P")));
        QVERIFY(m_editor->validationError().contains(QStringLiteral("already used")));
        // A chord Windows owns is its own kind. It is NOT unsupported_input:
        // that tier means the controller backend cannot expose a control, and
        // conflating the two would let a hotkey refusal masquerade as proof
        // that the unsupported-control path works.
        QCOMPARE(m_editor->relationKind(), QStringLiteral("hotkey_unavailable"));
        // The chord that was live before is untouched.
        QVERIFY(hasBinding(*m_runtime, QStringLiteral("keyboard"), {},
                           QStringLiteral("global.screenshot"), 1,
                           QStringLiteral("Ctrl+Shift+S")));
        m_editor->setHotkeyApply(nullptr);
    }

    void failedWriteRollsTheHotkeyBack()
    {
        // The other half: Windows accepted the chord but the database refused
        // the row. The OS registration must be handed back to the previous
        // chord rather than left pointing at a binding nobody saved.
        m_editor->setDeviceGroup(QStringLiteral("keyboard"));
        QStringList applied;
        m_editor->setHotkeyApply([&](const QString &, int, const QString &chord, QString *) {
            applied.append(chord.isEmpty() ? QStringLiteral("<cleared>") : chord);
            return true;
        });
        m_editor->setPersistRow([](const BindingOverrideRow &) { return false; });

        m_editor->beginCapture(QStringLiteral("global.screenshot"), 1);
        QVERIFY(m_editor->captureInput(QStringLiteral("keyboard"),
                                       QStringLiteral("Ctrl+Alt+K"),
                                       QStringLiteral("Ctrl+Alt+K")));

        // Claimed the new chord, then restored the one that was live.
        QCOMPARE(applied, QStringList({QStringLiteral("Ctrl+Alt+K"),
                                       QStringLiteral("Ctrl+Shift+S")}));
        QVERIFY(m_database->listBindingOverrides().isEmpty());
        QVERIFY(hasBinding(*m_runtime, QStringLiteral("keyboard"), {},
                           QStringLiteral("global.screenshot"), 1,
                           QStringLiteral("Ctrl+Shift+S")));
        QVERIFY(!m_editor->validationError().isEmpty());
        // A failed write is a third, separate kind — not a hotkey problem and
        // not an unsupported control.
        QCOMPARE(m_editor->relationKind(), QStringLiteral("persistence_error"));

        m_editor->setPersistRow(nullptr);
        m_editor->setHotkeyApply(nullptr);
    }

    void controllerBindingsSkipTheHotkeyLayer()
    {
        // Only keyboard Global press bindings own a Win32 hotkey. A controller
        // edit must not be gated on RegisterHotKey.
        m_editor->setDeviceGroup(QStringLiteral("controller"));
        bool called = false;
        m_editor->setHotkeyApply([&](const QString &, int, const QString &, QString *) {
            called = true;
            return false;
        });
        m_editor->beginCapture(QStringLiteral("desktop.favorite"), 2);
        QVERIFY(m_editor->captureInput(QStringLiteral("controller"),
                                       ControlId::genericButton(19),
                                       QStringLiteral("Button 19")));
        QVERIFY(!called);
        QVERIFY(hasBinding(*m_runtime, QStringLiteral("controller"), {},
                           QStringLiteral("desktop.favorite"), 2,
                           ControlId::genericButton(19)));
        m_editor->setHotkeyApply(nullptr);
    }

    void shippedDefaultsContainNoHardConflicts()
    {
        // The defaults are the one binding set every user starts from. If the
        // policy ever calls a shipped pair a hard conflict, either a default is
        // wrong or the policy is — both need a human, so fail loudly and name
        // the offending pair.
        m_runtime->reload();
        for (const QString &group : {QStringLiteral("controller"), QStringLiteral("keyboard"),
                                     QStringLiteral("mouse")}) {
            for (const auto &relation : m_runtime->relations(group, {})) {
                if (relation.kind != BindingRelation::Kind::HardConflict)
                    continue;
                QFAIL(qPrintable(QStringLiteral("default hard conflict: %1 vs %2 on %3")
                                     .arg(relation.left.actionId, relation.right.actionId,
                                          relation.left.triggerCode)));
            }
        }
    }

    void runtimeRelationsMatchDirectClassification()
    {
        // The editor trusts the precompiled table instead of walking the
        // binding list itself; it must agree with the policy pair for pair.
        m_runtime->reload();
        const auto bindings = m_runtime->effectiveBindings(QStringLiteral("controller"), {});
        int expected = 0;
        for (int i = 0; i < bindings.size(); ++i) {
            for (int j = i + 1; j < bindings.size(); ++j) {
                if (BindingRelation::classify(bindings.at(i), bindings.at(j))
                    != BindingRelation::Kind::None)
                    ++expected;
            }
        }
        QCOMPARE(m_runtime->relations(QStringLiteral("controller"), {}).size(), expected);
        // The Share tap pair is the one declared override, so it must be present
        // and classified as such rather than dropped.
        bool sawOverride = false;
        for (const auto &relation : m_runtime->relations(QStringLiteral("controller"), {})) {
            if (relation.kind == BindingRelation::Kind::ContextOverride)
                sawOverride = true;
        }
        QVERIFY(sawOverride);
    }

    void desktopCaptureTapStillFiresScreenshot()
    {
        // In Desktop scope (no clip focused), a Capture tap should still
        // emit global.screenshot as before — the contextual-override fix
        // must not break the Desktop → Global fallback path.
        m_runtime->setDefaultHoldMs(250);
        m_runtime->reload();
        QSignalSpy spy(m_runtime, &BindingRuntime::actionTriggered);

        QVERIFY(m_runtime->press(QStringLiteral("controller"), {}, ControlId::Capture,
                                 ActionCatalog::Scope::Desktop,
                                 ActionCatalog::Scope::Overlay));
        QVERIFY(m_runtime->release(QStringLiteral("controller"), {}, ControlId::Capture));
        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 700);

        QCOMPARE(spy.takeFirst().at(0).toString(), QStringLiteral("global.screenshot"));
    }

    void emptySecondaryScreenshotCapturesAsTap()
    {
        // An empty slot inherits the action's real gesture instead of "press".
        // Screenshot's only controller default is a slot-1 tap, so a fresh
        // secondary assignment is a tap too — and the prompt says so.
        m_editor->beginCapture(QStringLiteral("global.screenshot"), 2);
        QVERIFY(m_editor->capturePrompt().contains(QStringLiteral("· Tap")));
        QVERIFY(m_editor->captureInput(QStringLiteral("controller"),
                                       ControlId::genericButton(20),
                                       QStringLiteral("Button 20")));
        QVERIFY(!m_editor->conflictPending());
        const auto rows = m_database->listBindingOverrides();
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.first().activation, QStringLiteral("tap"));
    }

    void emptySecondarySaveReplayCapturesAsHoldWithHoldMs()
    {
        m_runtime->setDefaultHoldMs(2000);
        m_runtime->reload();
        m_editor->beginCapture(QStringLiteral("global.save_replay"), 2);
        QVERIFY(m_editor->capturePrompt().contains(QStringLiteral("· Hold")));
        QVERIFY(m_editor->captureInput(QStringLiteral("controller"),
                                       ControlId::genericButton(21),
                                       QStringLiteral("Button 21")));
        QVERIFY(!m_editor->conflictPending());
        const auto rows = m_database->listBindingOverrides();
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.first().activation, QStringLiteral("hold"));
        // 0 means "use the configured hold duration" — the built-in hold
        // defaults deliberately do not copy the setting into every row.
        QCOMPARE(rows.first().holdMs, 0);
    }

    void captureOntoSharedTriggerIsNotAnArtificialConflict()
    {
        // Capture already carries tap/hold/double_tap defaults. A secondary
        // Screenshot landing there as a tap is legitimate multiplexing; only
        // the old "press" guess made this a HardConflict on every button that
        // carried a timed gesture.
        m_editor->beginCapture(QStringLiteral("global.screenshot"), 2);
        QVERIFY(m_editor->captureInput(QStringLiteral("controller"),
                                       ControlId::Capture,
                                       QStringLiteral("Share")));
        QVERIFY(!m_editor->conflictPending());
        QVERIFY(hasBinding(*m_runtime, QStringLiteral("controller"), {},
                           QStringLiteral("global.screenshot"), 2, ControlId::Capture));
    }

    void clearKeepsTheSlotGesture()
    {
        // Toggle Overlay's secondary default is a Capture double-tap. Clearing
        // it must not degrade the slot to press — the unbound row carries the
        // gesture, and the next capture into the slot inherits it.
        m_editor->clearBinding(QStringLiteral("global.toggle_overlay"), 2);
        auto rows = m_database->listBindingOverrides();
        QCOMPARE(rows.size(), 1);
        QVERIFY(rows.first().unbound);
        QCOMPARE(rows.first().activation, QStringLiteral("tap"));
        QCOMPARE(rows.first().tapCount, 2);

        m_editor->beginCapture(QStringLiteral("global.toggle_overlay"), 2);
        QVERIFY(m_editor->capturePrompt().contains(QStringLiteral("· Double tap")));
        QVERIFY(m_editor->captureInput(QStringLiteral("controller"),
                                       ControlId::genericButton(22),
                                       QStringLiteral("Button 22")));
        QVERIFY(!m_editor->conflictPending());
        rows = m_database->listBindingOverrides();
        QCOMPARE(rows.size(), 1);
        QVERIFY(!rows.first().unbound);
        QCOMPARE(rows.first().activation, QStringLiteral("tap"));
        QCOMPARE(rows.first().tapCount, 2);
    }

    void clearRebindRoundTripPreservesHoldMs()
    {
        m_runtime->setDefaultHoldMs(2000);
        m_runtime->reload();
        m_editor->clearBinding(QStringLiteral("global.save_replay"), 1);
        m_editor->beginCapture(QStringLiteral("global.save_replay"), 1);
        QVERIFY(m_editor->captureInput(QStringLiteral("controller"),
                                       ControlId::genericButton(23),
                                       QStringLiteral("Button 23")));
        const auto rows = m_database->listBindingOverrides();
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.first().activation, QStringLiteral("hold"));
        QCOMPARE(rows.first().holdMs, 0);
    }

    void legacyClearSentinelDoesNotResurrectPress()
    {
        // Rows cleared before 0.7.3 carry the old press/0 sentinel. They hold
        // no real gesture information, so the resolution falls through to the
        // action's live slot instead of trusting them.
        QVERIFY(m_database->upsertBindingOverride(
            {QStringLiteral("controller"), {}, QStringLiteral("global.screenshot"), 2,
             {}, QStringLiteral("press"), 0, true}));
        m_runtime->reload();
        m_editor->beginCapture(QStringLiteral("global.screenshot"), 2);
        QVERIFY(m_editor->captureInput(QStringLiteral("controller"),
                                       ControlId::genericButton(24),
                                       QStringLiteral("Button 24")));
        QVERIFY(!m_editor->conflictPending());
        const auto rows = m_database->listBindingOverrides();
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.first().activation, QStringLiteral("tap"));
    }

    void replaceRollbackRestoresTheDisplacedOverride()
    {
        // Replace displaces the conflicting binding, then writes the new one.
        // If that second write fails, the displaced action must get back the
        // custom binding it had before the transaction — not its shipped
        // default, which is what a bare row delete would restore.
        m_editor->setDeviceGroup(QStringLiteral("keyboard"));
        m_editor->beginCapture(QStringLiteral("desktop.navigate_up"), 1);
        QVERIFY(m_editor->captureInput(QStringLiteral("keyboard"), QStringLiteral("J"),
                                       QStringLiteral("J")));
        QVERIFY(!m_editor->conflictPending());

        m_editor->setPersistRow([this](const BindingOverrideRow &row) {
            if (row.actionId == QLatin1String("desktop.navigate_down"))
                return false;
            return m_database->upsertBindingOverride(row);
        });
        m_editor->beginCapture(QStringLiteral("desktop.navigate_down"), 2);
        QVERIFY(m_editor->captureInput(QStringLiteral("keyboard"), QStringLiteral("J"),
                                       QStringLiteral("J")));
        QVERIFY(m_editor->conflictPending());
        m_editor->confirmConflict();
        m_editor->setPersistRow(nullptr);

        QVERIFY(hasBinding(*m_runtime, QStringLiteral("keyboard"), {},
                           QStringLiteral("desktop.navigate_up"), 1, QStringLiteral("J")));
        QVERIFY(!hasBinding(*m_runtime, QStringLiteral("keyboard"), {},
                            QStringLiteral("desktop.navigate_down"), 2, QStringLiteral("J")));
    }

    void legacySlotRowsApplyThroughAliasOnlyAndCopyIsExplicit()
    {
        // A pre-identity "any pad in slot 0" override.
        QVERIFY(m_database->upsertBindingOverride(
            {QStringLiteral("controller"), QStringLiteral("xinput.slot0"),
             QStringLiteral("desktop.favorite"), 2, ControlId::genericButton(30),
             QStringLiteral("press"), 0, false}));
        m_runtime->reload();
        const QString pad = QStringLiteral("3537:1004");

        // A stable-identity profile does not inherit the slot row by itself...
        QVERIFY(!hasBinding(*m_runtime, QStringLiteral("controller"), pad,
                            QStringLiteral("desktop.favorite"), 2,
                            ControlId::genericButton(30)));
        // ...until the alias declares this pad currently occupies that slot.
        m_runtime->setProfileAlias(pad, QStringLiteral("xinput.slot0"));
        QVERIFY(hasBinding(*m_runtime, QStringLiteral("controller"), pad,
                           QStringLiteral("desktop.favorite"), 2,
                           ControlId::genericButton(30)));
        // A different pad's identity never sees the slot row.
        QVERIFY(!hasBinding(*m_runtime, QStringLiteral("controller"),
                            QStringLiteral("045e:02ff"),
                            QStringLiteral("desktop.favorite"), 2,
                            ControlId::genericButton(30)));

        // A row saved for the pad itself outranks the aliased slot row.
        QVERIFY(m_database->upsertBindingOverride(
            {QStringLiteral("controller"), pad, QStringLiteral("desktop.favorite"), 2,
             ControlId::genericButton(31), QStringLiteral("press"), 0, false}));
        m_runtime->reload();
        QVERIFY(hasBinding(*m_runtime, QStringLiteral("controller"), pad,
                           QStringLiteral("desktop.favorite"), 2,
                           ControlId::genericButton(31)));
        QVERIFY(!hasBinding(*m_runtime, QStringLiteral("controller"), pad,
                            QStringLiteral("desktop.favorite"), 2,
                            ControlId::genericButton(30)));

        m_runtime->setProfileAlias(pad, {});
    }

    void copyLegacyOverridesKeepsOriginalsAndOwnRows()
    {
        QVERIFY(m_database->upsertBindingOverride(
            {QStringLiteral("controller"), QStringLiteral("xinput.slot0"),
             QStringLiteral("desktop.favorite"), 2, ControlId::genericButton(32),
             QStringLiteral("press"), 0, false}));
        QVERIFY(m_database->upsertBindingOverride(
            {QStringLiteral("controller"), QStringLiteral("xinput.slot0"),
             QStringLiteral("desktop.menu"), 2, ControlId::genericButton(33),
             QStringLiteral("press"), 0, false}));
        const QString pad = QStringLiteral("3537:1004");
        // The pad already customized desktop.favorite slot 2 itself.
        QVERIFY(m_database->upsertBindingOverride(
            {QStringLiteral("controller"), pad, QStringLiteral("desktop.favorite"), 2,
             ControlId::genericButton(34), QStringLiteral("press"), 0, false}));
        m_runtime->reload();

        m_editor->setControllerProfile({QStringLiteral("XInput"), pad,
                                        ControlId::ControllerFamily::Xbox,
                                        QStringLiteral("Xbox-compatible controller")});
        m_editor->setControllerSpecific(true);
        QVERIFY(m_editor->legacyCopyAvailable());
        m_editor->copyLegacyOverridesToController();

        int legacyRows = 0, padMenu = 0, padFavorite34 = 0;
        for (const BindingOverrideRow& row : m_database->listBindingOverrides()) {
            if (row.deviceProfile == QLatin1String("xinput.slot0"))
                ++legacyRows;
            if (row.deviceProfile == pad && row.actionId == QLatin1String("desktop.menu")
                && row.triggerCode == ControlId::genericButton(33))
                ++padMenu;
            if (row.deviceProfile == pad
                && row.actionId == QLatin1String("desktop.favorite")
                && row.triggerCode == ControlId::genericButton(34))
                ++padFavorite34;
        }
        QCOMPARE(legacyRows, 2);      // originals untouched
        QCOMPARE(padMenu, 1);         // promoted
        QCOMPARE(padFavorite34, 1);   // the pad's own row was not clobbered
    }

    void identicalModelsKeepIndependentCanonicalProfiles()
    {
        const QString padA = QStringLiteral("controller-pad-a");
        const QString padB = QStringLiteral("controller-pad-b");
        const QString trigger = ControlId::genericButton(35);
        QVERIFY(m_database->upsertBindingOverride(
            {QStringLiteral("controller"), padA, QStringLiteral("desktop.favorite"), 2,
             trigger, QStringLiteral("press"), 0, false}));
        QVERIFY(m_database->upsertBindingOverride(
            {QStringLiteral("controller"), padB, QStringLiteral("desktop.menu"), 2,
             trigger, QStringLiteral("press"), 0, false}));
        m_runtime->reload();
        m_runtime->setProfileAliases(
            padA, {QStringLiteral("054c:0ce6"), QStringLiteral("xinput.slot0")});
        m_runtime->setProfileAliases(
            padB, {QStringLiteral("054c:0ce6"), QStringLiteral("xinput.slot1")});
        QSignalSpy actions(m_runtime, &BindingRuntime::actionTriggered);

        QVERIFY(m_runtime->press(QStringLiteral("controller"), padB, trigger,
                                 ActionCatalog::Scope::Desktop));
        QCOMPARE(actions.size(), 1);
        QCOMPARE(actions.front().front().toString(), QStringLiteral("desktop.menu"));
        QVERIFY(actions.front().front().toString() != QStringLiteral("desktop.favorite"));
        m_runtime->release(QStringLiteral("controller"), padB, trigger);
    }

    // A row that cannot be a valid pattern — a chord serialization this build
    // does not know, a gesture whose parts contradict each other — must be
    // skipped at load, leaving the action on its default. Executing a guess
    // would fire an action the user never assigned.
    void malformedStoredRowsAreSkippedAndReported()
    {
        InputDiagnostics::instance().clear();

        // Valid, and must survive next to the broken ones.
        QVERIFY(m_database->upsertBindingOverride(
            {QStringLiteral("controller"), {}, QStringLiteral("desktop.favorite"), 2,
             ControlId::genericButton(12), QStringLiteral("press"), 0, false}));
        // Unknown chord serialization version: fails closed.
        QVERIFY(m_database->upsertBindingOverride(
            {QStringLiteral("controller"), {}, QStringLiteral("desktop.menu"), 2,
             QStringLiteral("chord:v2:gamepad.capture>gamepad.guide"),
             QStringLiteral("press"), 0, false}));
        // Same control twice is not an ordered chord.
        QVERIFY(m_database->upsertBindingOverride(
            {QStringLiteral("controller"), {}, QStringLiteral("desktop.tab_next"), 2,
             QStringLiteral("chord:v1:gamepad.capture>gamepad.capture"),
             QStringLiteral("press"), 0, false}));
        // A press gesture carrying a hold duration is a corrupted row.
        QVERIFY(m_database->upsertBindingOverride(
            {QStringLiteral("controller"), {}, QStringLiteral("desktop.tab_prev"), 2,
             ControlId::genericButton(13), QStringLiteral("press"), 750, false}));

        m_runtime->reload();

        QVERIFY(hasBinding(*m_runtime, QStringLiteral("controller"), {},
                           QStringLiteral("desktop.favorite"), 2,
                           ControlId::genericButton(12)));
        for (const auto& binding : m_runtime->effectiveBindings(QStringLiteral("controller"))) {
            QVERIFY(!binding.triggerCode.startsWith(QLatin1String("chord:")));
            QVERIFY2(!(binding.actionId == QLatin1String("desktop.tab_prev")
                       && binding.slot == 2),
                     "the contradictory press/hold row must not become a binding");
        }

        const QString diagnostics = InputDiagnostics::instance().exportText();
        QVERIFY(diagnostics.contains(QStringLiteral("Rejected binding rows")));
        QVERIFY(diagnostics.contains(QStringLiteral("desktop.menu slot 2")));
        QVERIFY(diagnostics.contains(QStringLiteral("desktop.tab_next slot 2")));
        QVERIFY(diagnostics.contains(QStringLiteral("desktop.tab_prev slot 2")));
        QVERIFY(!diagnostics.contains(QStringLiteral("desktop.favorite slot 2")));
    }

    // ----------------------------------------------- Edit Assignment dialog

    void theDialogOpensSeededFromTheSlotItEdits()
    {
        m_editor->openAssignmentEditor(QStringLiteral("global.save_replay"), 1);
        QVERIFY(m_editor->editorOpen());
        QCOMPARE(m_editor->editorSlot(), 1);
        // Save Replay has Share-held as its controller default, so the draft
        // opens on that gesture rather than on a blank form.
        QCOMPARE(m_editor->editorGestureKind(), QStringLiteral("hold"));
        QCOMPARE(m_editor->editorTriggerKind(), QStringLiteral("single"));
        QVERIFY(m_editor->editorCanSave());
        m_editor->closeAssignmentEditor();
        QVERIFY(!m_editor->editorOpen());
    }

    void anEmptySlotInheritsItsGestureIntoTheDraft()
    {
        m_editor->openAssignmentEditor(QStringLiteral("global.screenshot"), 2);
        QVERIFY(m_editor->editorOpen());
        QCOMPARE(m_editor->editorGestureKind(), QStringLiteral("tap"));
        // Nothing captured yet, so there is nothing to save.
        QVERIFY(!m_editor->editorCanSave());
        m_editor->closeAssignmentEditor();
    }

    void captureIsAnExplicitModeAndConsumesTheControl()
    {
        m_editor->openAssignmentEditor(QStringLiteral("global.screenshot"), 2);
        // Not listening yet: a press must not be swallowed, or the pad could
        // not navigate the dialog at all.
        QVERIFY(!m_editor->captureInput(QStringLiteral("controller"),
                                        ControlId::genericButton(25),
                                        QStringLiteral("Button 25")));
        m_editor->beginTriggerCapture(1);
        QCOMPARE(m_editor->editorCaptureStep(), QStringLiteral("first"));
        QVERIFY(m_editor->captureInput(QStringLiteral("controller"),
                                       ControlId::genericButton(25),
                                       QStringLiteral("Button 25")));
        QCOMPARE(m_editor->editorCaptureStep(), QStringLiteral("idle"));
        QVERIFY(m_editor->editorCanSave());

        m_editor->saveAssignment();
        QVERIFY(!m_editor->editorOpen());
        QVERIFY(hasBinding(*m_runtime, QStringLiteral("controller"), {},
                           QStringLiteral("global.screenshot"), 2,
                           ControlId::genericButton(25)));
    }

    void aTripleTapCanBeChosenAndSaved()
    {
        // The gesture the old capture-only flow could never express.
        m_editor->openAssignmentEditor(QStringLiteral("global.screenshot"), 2);
        m_editor->beginTriggerCapture(1);
        QVERIFY(m_editor->captureInput(QStringLiteral("controller"),
                                       ControlId::genericButton(26),
                                       QStringLiteral("Button 26")));
        m_editor->setEditorGesture(QStringLiteral("tap"), 3, 0);
        QCOMPARE(m_editor->editorTapCount(), 3);
        m_editor->saveAssignment();

        const auto rows = m_database->listBindingOverrides();
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.first().activation, QStringLiteral("tap"));
        QCOMPARE(rows.first().tapCount, 3);
        const QVariantMap displayed = rowFor(*m_editor, QStringLiteral("global.screenshot"));
        QVERIFY(displayed.value(QStringLiteral("secondaryAssigned")).toBool());
        QCOMPARE(displayed.value(QStringLiteral("secondaryGesture")).toString(),
                 QStringLiteral("Triple tap"));
    }

    void aCombinationIsCapturedStepwiseAndSavedAsAChord()
    {
        m_editor->openAssignmentEditor(QStringLiteral("global.save_replay"), 2);
        m_editor->setEditorTriggerKind(QStringLiteral("combination"));
        // Combinations are press-only in v1, so the gesture picker is fixed.
        QVERIFY(m_editor->editorGestureLocked());
        QCOMPARE(m_editor->editorGestureKind(), QStringLiteral("press"));

        m_editor->beginTriggerCapture(1);
        QVERIFY(m_editor->captureInput(QStringLiteral("controller"), ControlId::Capture,
                                       QStringLiteral("Share")));
        // One button is not a combination yet.
        QCOMPARE(m_editor->editorCaptureStep(), QStringLiteral("second"));
        QCOMPARE(m_editor->editorFirstControlLabel(), QStringLiteral("Capture"));
        QVERIFY(m_editor->editorSecondControlLabel().isEmpty());
        QVERIFY(!m_editor->editorCanSave());

        // The same button twice is refused without leaving capture.
        QVERIFY(m_editor->captureInput(QStringLiteral("controller"), ControlId::Capture,
                                       QStringLiteral("Share")));
        QCOMPARE(m_editor->editorCaptureStep(), QStringLiteral("second"));

        QVERIFY(m_editor->captureInput(QStringLiteral("controller"), ControlId::Guide,
                                       QStringLiteral("PS")));
        QVERIFY(m_editor->editorCanSave());
        m_editor->saveAssignment();

        const auto rows = m_database->listBindingOverrides();
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.first().triggerCode,
                 QStringLiteral("chord:v1:gamepad.capture>gamepad.guide"));
        QCOMPARE(rows.first().activation, QStringLiteral("press"));
        const QVariantMap displayed = rowFor(*m_editor, QStringLiteral("global.save_replay"));
        QCOMPARE(displayed.value(QStringLiteral("secondaryTrigger")).toString(),
                 QStringLiteral("Capture + Guide"));
        QCOMPARE(displayed.value(QStringLiteral("secondaryGesture")).toString(),
                 QStringLiteral("Combination"));
    }

    void switchingBackToASingleButtonDropsTheSecondControl()
    {
        m_editor->openAssignmentEditor(QStringLiteral("global.save_replay"), 2);
        m_editor->setEditorTriggerKind(QStringLiteral("combination"));
        m_editor->beginTriggerCapture(1);
        // Buttons nothing else is bound to, so the assertion is about the
        // draft dropping its second control, not about a conflict modal.
        m_editor->noteObservedControl(ControlId::genericButton(27));
        m_editor->noteObservedControl(ControlId::genericButton(28));
        m_editor->captureInput(QStringLiteral("controller"), ControlId::genericButton(27),
                               QStringLiteral("Button 27"));
        m_editor->captureInput(QStringLiteral("controller"), ControlId::genericButton(28),
                               QStringLiteral("Button 28"));
        m_editor->setEditorTriggerKind(QStringLiteral("single"));
        QVERIFY(!m_editor->editorGestureLocked());
        QVERIFY(m_editor->editorCanSave());
        m_editor->saveAssignment();

        const auto rows = m_database->listBindingOverrides();
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.first().triggerCode, ControlId::genericButton(27));
    }

    void keyboardSlotsAreNeverOfferedACombination()
    {
        m_editor->setDeviceGroup(QStringLiteral("keyboard"));
        m_editor->openAssignmentEditor(QStringLiteral("global.screenshot"), 2);
        QVERIFY(!m_editor->editorCombinationAvailable());
        m_editor->setEditorTriggerKind(QStringLiteral("combination"));
        QCOMPARE(m_editor->editorTriggerKind(), QStringLiteral("single"));
        m_editor->closeAssignmentEditor();
    }

    void theDraftWarnsAboutAButtonTheControllerHasNeverSent()
    {
        // The honest version of a "Guide may be unavailable" warning: based on
        // what this session actually received, not on a hardware list.
        m_editor->openAssignmentEditor(QStringLiteral("global.screenshot"), 2);
        m_editor->beginTriggerCapture(1);
        m_editor->captureInput(QStringLiteral("controller"), ControlId::Guide,
                               QStringLiteral("PS"));
        QCOMPARE(m_editor->editorNoticeKind(), QStringLiteral("unsupported_input"));
        QVERIFY(m_editor->editorNotice().contains(QStringLiteral("has not received Guide")));

        // Once the pad really delivers it, the warning goes away.
        m_editor->noteObservedControl(ControlId::Guide);
        QVERIFY(m_editor->editorNoticeKind() != QStringLiteral("unsupported_input"));
        m_editor->closeAssignmentEditor();
    }

    void theDraftExplainsTheMultiTapDelayBeforeSaving()
    {
        m_editor->noteObservedControl(ControlId::Capture);
        // Share already carries a screenshot tap; a triple tap on the same
        // button is legal but makes that tap wait.
        m_editor->openAssignmentEditor(QStringLiteral("overlay.favorite"), 2);
        m_editor->beginTriggerCapture(1);
        m_editor->captureInput(QStringLiteral("controller"), ControlId::Capture,
                               QStringLiteral("Share"));
        m_editor->setEditorGesture(QStringLiteral("tap"), 3, 0);
        QCOMPARE(m_editor->editorNoticeKind(), QStringLiteral("higher_tap_count_delay"));
        m_editor->closeAssignmentEditor();
    }

    void pressToTripleTapConversionPreservesBothExactActions()
    {
        seedGuidePressOverride();
        m_editor->noteObservedControl(ControlId::Guide);
        m_editor->openAssignmentEditor(QStringLiteral("overlay.favorite"), 2);
        m_editor->beginTriggerCapture(1);
        QVERIFY(m_editor->captureInput(QStringLiteral("controller"), ControlId::Guide,
                                       QStringLiteral("PS")));
        m_editor->setEditorGesture(QStringLiteral("tap"), 3, 0);
        QCOMPARE(m_editor->editorNoticeKind(), QStringLiteral("conversion_required"));
        m_editor->saveAssignment();
        QVERIFY(m_editor->compatibilityPending());
        QVERIFY(!m_editor->conflictPending());
        QVERIFY(m_editor->compatibilityMessage().contains(QStringLiteral("released")));
        QVERIFY(m_editor->compatibilityMessage().contains(QStringLiteral("300 ms")));

        m_editor->confirmCompatibility();
        QVERIFY(!m_editor->compatibilityPending());
        const auto single = bindingFor(*m_runtime, QStringLiteral("controller"), {},
                                       QStringLiteral("global.toggle_overlay"), 1);
        const auto triple = bindingFor(*m_runtime, QStringLiteral("controller"), {},
                                       QStringLiteral("overlay.favorite"), 2);
        QCOMPARE(single.triggerCode, ControlId::Guide);
        QCOMPARE(single.gesture(), GestureSpec::tap(1));
        QCOMPARE(triple.triggerCode, ControlId::Guide);
        QCOMPARE(triple.gesture(), GestureSpec::tap(3));

        m_runtime->setTiming({20, 300, 2000});
        QSignalSpy fired(m_runtime, &BindingRuntime::actionTriggered);
        QVERIFY(m_runtime->press(QStringLiteral("controller"), {}, ControlId::Guide,
                                 ActionCatalog::Scope::Overlay));
        QVERIFY(m_runtime->release(QStringLiteral("controller"), {}, ControlId::Guide));
        QTRY_COMPARE(fired.count(), 1);
        QCOMPARE(fired.takeFirst().at(0).toString(), QStringLiteral("global.toggle_overlay"));

        for (int i = 0; i < 3; ++i) {
            QVERIFY(m_runtime->press(QStringLiteral("controller"), {}, ControlId::Guide,
                                     ActionCatalog::Scope::Overlay));
            QVERIFY(m_runtime->release(QStringLiteral("controller"), {}, ControlId::Guide));
        }
        QCOMPARE(fired.count(), 1);
        QCOMPARE(fired.takeFirst().at(0).toString(), QStringLiteral("overlay.favorite"));
    }

    void compatibilityCancelAndChooseAnotherLeaveBindingsUntouched()
    {
        seedGuidePressOverride();
        m_editor->noteObservedControl(ControlId::Guide);
        const auto prepare = [this] {
            m_editor->openAssignmentEditor(QStringLiteral("overlay.favorite"), 2);
            m_editor->beginTriggerCapture(1);
            m_editor->captureInput(QStringLiteral("controller"), ControlId::Guide,
                                   QStringLiteral("PS"));
            m_editor->setEditorGesture(QStringLiteral("tap"), 3, 0);
            m_editor->saveAssignment();
            QVERIFY(m_editor->compatibilityPending());
        };

        prepare();
        m_editor->dismissCompatibility();
        QVERIFY(!hasBinding(*m_runtime, QStringLiteral("controller"), {},
                            QStringLiteral("overlay.favorite"), 2, ControlId::Guide));
        QCOMPARE(bindingFor(*m_runtime, QStringLiteral("controller"), {},
                            QStringLiteral("global.toggle_overlay"), 1).gesture(),
                 GestureSpec::press());
        m_editor->closeAssignmentEditor();

        prepare();
        m_editor->retryCompatibilityCapture();
        QVERIFY(!m_editor->compatibilityPending());
        QVERIFY(m_editor->captureActive());
        QCOMPARE(m_editor->editorCaptureStep(), QStringLiteral("first"));
        m_editor->cancelTriggerCapture();
        m_editor->closeAssignmentEditor();
    }

    void conversionPreflightRejectsASecondSingleTap()
    {
        seedGuidePressOverride();
        QVERIFY(m_database->upsertBindingOverride(
            {QStringLiteral("controller"), {}, QStringLiteral("global.screenshot"), 2,
             ControlId::Guide, QStringLiteral("tap"), 0, false, 1}));
        m_runtime->reload();
        m_editor->noteObservedControl(ControlId::Guide);
        m_editor->openAssignmentEditor(QStringLiteral("overlay.favorite"), 2);
        m_editor->beginTriggerCapture(1);
        m_editor->captureInput(QStringLiteral("controller"), ControlId::Guide,
                               QStringLiteral("PS"));
        m_editor->setEditorGesture(QStringLiteral("tap"), 3, 0);
        QCOMPARE(m_editor->editorNoticeKind(), QStringLiteral("hard_conflict"));
        m_editor->saveAssignment();
        QVERIFY(m_editor->conflictPending());
        QVERIFY(!m_editor->compatibilityPending());
        m_editor->dismissConflict();
        m_editor->closeAssignmentEditor();
    }

    void conversionFailureRestoresBothPreviousRows()
    {
        seedGuidePressOverride();
        m_editor->noteObservedControl(ControlId::Guide);
        m_editor->openAssignmentEditor(QStringLiteral("overlay.favorite"), 2);
        m_editor->beginTriggerCapture(1);
        m_editor->captureInput(QStringLiteral("controller"), ControlId::Guide,
                               QStringLiteral("PS"));
        m_editor->setEditorGesture(QStringLiteral("tap"), 3, 0);
        m_editor->saveAssignment();
        QVERIFY(m_editor->compatibilityPending());

        m_editor->setPersistRow([this](const BindingOverrideRow& row) {
            if (row.actionId == QLatin1String("overlay.favorite"))
                return false;
            return m_database->upsertBindingOverride(row);
        });
        m_editor->confirmCompatibility();
        m_editor->setPersistRow(nullptr);

        QCOMPARE(m_editor->relationKind(), QStringLiteral("persistence_error"));
        QCOMPARE(bindingFor(*m_runtime, QStringLiteral("controller"), {},
                            QStringLiteral("global.toggle_overlay"), 1).gesture(),
                 GestureSpec::press());
        QVERIFY(!hasBinding(*m_runtime, QStringLiteral("controller"), {},
                            QStringLiteral("overlay.favorite"), 2, ControlId::Guide));
        // Rollback leaves exactly the press row this test seeded — the
        // conversion's tap(1) rewrite and the tap(3) row are both gone.
        const auto rows = m_database->listBindingOverrides();
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.first().actionId, QStringLiteral("global.toggle_overlay"));
        QCOMPARE(rows.first().activation, QStringLiteral("press"));
        m_editor->closeAssignmentEditor();
    }

    void savingOverAnExistingAssignmentStillRaisesTheConflictModal()
    {
        m_editor->noteObservedControl(ControlId::Guide);
        m_editor->openAssignmentEditor(QStringLiteral("global.screenshot"), 2);
        m_editor->beginTriggerCapture(1);
        m_editor->captureInput(QStringLiteral("controller"), ControlId::Guide,
                               QStringLiteral("PS"));
        m_editor->setEditorGesture(QStringLiteral("press"), 1, 0);
        QCOMPARE(m_editor->editorNoticeKind(), QStringLiteral("hard_conflict"));
        m_editor->saveAssignment();
        QVERIFY(m_editor->conflictPending());
        m_editor->confirmConflict();
        QVERIFY(hasBinding(*m_runtime, QStringLiteral("controller"), {},
                           QStringLiteral("global.screenshot"), 2, ControlId::Guide));
        m_editor->closeAssignmentEditor();
    }
};


QTEST_GUILESS_MAIN(BindingEditorTest)
#include "tst_bindingeditor.moc"
