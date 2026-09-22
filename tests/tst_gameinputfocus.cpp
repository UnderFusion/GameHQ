// cpo-o06c: the GameInput focus policy has exactly one owner.
//
// This file pins the transitions that owner may make, through the real
// controller against the fake runtime, and asserts the SEQUENCE of policies the
// runtime was asked for — not just the final state. The three properties the
// leaf hangs on:
//
//   * the exclusive policy is only ever applied in response to a request (the
//     overlay's gate decides whether to request; see tst_overlayfocustrace);
//   * every exit path returns the runtime to the background policy, including
//     the one where the runtime itself goes away;
//   * repeated requests and repeated open/close cycles neither accumulate nor
//     leak policy applications.
//
// What it does NOT claim: nothing here says another process stopped receiving
// controller input. The policy is GameInput-scoped and best-effort, and that
// question belongs to cpo-o06d's external receiver.
#include "gameinput/FakeGameInputApi.h"
#include "gameinput/GameInputFocusController.h"

#include <QSignalSpy>
#include <QtTest>

#include <memory>

using namespace ModernInput;

namespace
{
// The policy applications the runtime actually saw, in order.
QStringList policyEntries(const FakeGameInputApi& api)
{
    QStringList entries;
    for (const QString& entry : api.callLog()) {
        if (entry.startsWith(QLatin1String("focus-policy:")))
            entries.append(entry);
    }
    return entries;
}
}  // namespace

class TestGameInputFocus : public QObject
{
    Q_OBJECT

private slots:
    void attachStartsTheSessionInTheBackgroundPolicy();
    void requestAppliesExclusiveExactlyOnce();
    void restoreIsIdempotentAndReturnsToBackground();
    void repeatedCyclesDoNotAccumulatePolicyApplications();
    void detachWhileExclusiveRestoresBeforeTheRuntimeGoesAway();
    void aRequestWithoutARuntimeIsRefusedAndRecorded();
    void aNewSessionNeverInheritsTheExclusiveState();
    void theSignalledSequenceMatchesWhatTheRuntimeWasAskedFor();
    void theCounterCountsPolicyApplications();
    void theMasksAreTheDocumentedOnes();
    void theExportPhrasingCannotBeMistakenForIsolation();
};

void TestGameInputFocus::attachStartsTheSessionInTheBackgroundPolicy()
{
    auto fake = std::make_unique<FakeGameInputApi>();
    FakeGameInputApi* raw = fake.get();
    QString error;
    QVERIFY2(raw->initialize(error), qPrintable(error));
    GameInputFocusController controller;
    controller.attach(raw);

    QVERIFY(controller.policyAttached());
    QVERIFY(controller.focusMode() == GameInputFocusMode::Background);
    QCOMPARE(policyEntries(*raw), QStringList{ QStringLiteral("focus-policy:background") });
    QCOMPARE(controller.focusTransitionCount(), 1);

    // Idempotent on the session, not on the policy: attaching the same runtime
    // again changes nothing at all.
    controller.attach(raw);
    QCOMPARE(policyEntries(*raw).size(), 1);
    QCOMPARE(controller.focusTransitionCount(), 1);
}

void TestGameInputFocus::requestAppliesExclusiveExactlyOnce()
{
    auto fake = std::make_unique<FakeGameInputApi>();
    FakeGameInputApi* raw = fake.get();
    GameInputFocusController controller;
    controller.attach(raw);

    QVERIFY(controller.requestExclusiveForeground(QStringLiteral("overlay-interactive")));
    QVERIFY(controller.focusMode() == GameInputFocusMode::ExclusiveForeground);
    QVERIFY(controller.exclusiveForegroundActive());
    QCOMPARE(controller.focusModeName(), QStringLiteral("exclusive-foreground"));
    QCOMPARE(policyEntries(*raw),
             QStringList({ QStringLiteral("focus-policy:background"),
                           QStringLiteral("focus-policy:exclusive-foreground") }));
    QCOMPARE(controller.focusTransitionCount(), 2);

    // A second request while it is already in force re-issues nothing.
    QVERIFY(controller.requestExclusiveForeground(QStringLiteral("overlay-interactive")));
    QCOMPARE(policyEntries(*raw).size(), 2);
    QCOMPARE(controller.focusTransitionCount(), 2);
    QCOMPARE(controller.duplicateRequestCount(), 1);
    QCOMPARE(controller.lastTransitionReason(), QStringLiteral("overlay-interactive"));
}

void TestGameInputFocus::restoreIsIdempotentAndReturnsToBackground()
{
    auto fake = std::make_unique<FakeGameInputApi>();
    FakeGameInputApi* raw = fake.get();
    GameInputFocusController controller;
    controller.attach(raw);
    QVERIFY(controller.requestExclusiveForeground(QStringLiteral("overlay-interactive")));

    controller.restoreBackground(QStringLiteral("overlay-closed"));
    QVERIFY(controller.focusMode() == GameInputFocusMode::Background);
    QVERIFY(!controller.exclusiveForegroundActive());
    QCOMPARE(policyEntries(*raw).last(), QStringLiteral("focus-policy:background"));
    QCOMPARE(controller.focusTransitionCount(), 3);
    QCOMPARE(controller.lastTransitionReason(), QStringLiteral("overlay-closed"));

    // Every exit path may call the release unconditionally; this is what makes
    // that safe.
    controller.restoreBackground(QStringLiteral("overlay-closed"));
    QCOMPARE(controller.focusTransitionCount(), 3);
    QCOMPARE(policyEntries(*raw).size(), 3);
}

void TestGameInputFocus::repeatedCyclesDoNotAccumulatePolicyApplications()
{
    auto fake = std::make_unique<FakeGameInputApi>();
    FakeGameInputApi* raw = fake.get();
    GameInputFocusController controller;
    controller.attach(raw);
    const int afterAttach = controller.focusTransitionCount();

    for (int cycle = 0; cycle < 5; ++cycle) {
        QVERIFY(controller.requestExclusiveForeground(QStringLiteral("overlay-interactive")));
        controller.restoreBackground(QStringLiteral("overlay-closed"));
    }

    // Two applications per cycle, exactly: no drift, no leak, and the session
    // ends where it started.
    QCOMPARE(controller.focusTransitionCount() - afterAttach, 10);
    QCOMPARE(policyEntries(*raw).size(), 11);   // 1 background + 5 x (exclusive + background)
    QVERIFY(controller.focusMode() == GameInputFocusMode::Background);
    QCOMPARE(controller.duplicateRequestCount(), 0);
}

void TestGameInputFocus::detachWhileExclusiveRestoresBeforeTheRuntimeGoesAway()
{
    auto fake = std::make_unique<FakeGameInputApi>();
    FakeGameInputApi* raw = fake.get();
    GameInputFocusController controller;
    controller.attach(raw);
    QVERIFY(controller.requestExclusiveForeground(QStringLiteral("overlay-interactive")));

    controller.detach();

    // The restore reached the runtime while it was still alive, and it was the
    // last thing the controller asked for.
    QCOMPARE(policyEntries(*raw),
             QStringList({ QStringLiteral("focus-policy:background"),
                           QStringLiteral("focus-policy:exclusive-foreground"),
                           QStringLiteral("focus-policy:background") }));
    QVERIFY(controller.focusMode() == GameInputFocusMode::Background);
    QVERIFY(!controller.policyAttached());

    // A request after the runtime is gone is refused, not queued and not
    // remembered as if it had been applied.
    QVERIFY(!controller.requestExclusiveForeground(QStringLiteral("overlay-interactive")));
    QCOMPARE(controller.refusedRequestCount(), 1);
    QCOMPARE(policyEntries(*raw).size(), 3);
    QVERIFY(!controller.exclusiveForegroundActive());
}

void TestGameInputFocus::aRequestWithoutARuntimeIsRefusedAndRecorded()
{
    GameInputFocusController controller;
    QVERIFY(!controller.policyAttached());
    QVERIFY(!controller.requestExclusiveForeground(QStringLiteral("overlay-interactive")));
    QCOMPARE(controller.refusedRequestCount(), 1);
    QCOMPARE(controller.focusTransitionCount(), 0);
    QVERIFY(controller.focusMode() == GameInputFocusMode::Background);

    // Same for a release: nothing to release, nothing done.
    controller.restoreBackground(QStringLiteral("overlay-closed"));
    QCOMPARE(controller.focusTransitionCount(), 0);
}

void TestGameInputFocus::aNewSessionNeverInheritsTheExclusiveState()
{
    auto first = std::make_unique<FakeGameInputApi>();
    FakeGameInputApi* firstRaw = first.get();
    GameInputFocusController controller;
    controller.attach(firstRaw);
    QVERIFY(controller.requestExclusiveForeground(QStringLiteral("overlay-interactive")));
    controller.detach();

    // A different runtime object: the new session must start from the background
    // policy even though the controller's previous state was exclusive.
    auto second = std::make_unique<FakeGameInputApi>();
    FakeGameInputApi* secondRaw = second.get();
    controller.attach(secondRaw);

    QCOMPARE(policyEntries(*secondRaw), QStringList{ QStringLiteral("focus-policy:background") });
    QVERIFY(controller.focusMode() == GameInputFocusMode::Background);
    QVERIFY(!controller.exclusiveForegroundActive());
}

void TestGameInputFocus::theSignalledSequenceMatchesWhatTheRuntimeWasAskedFor()
{
    auto fake = std::make_unique<FakeGameInputApi>();
    FakeGameInputApi* raw = fake.get();
    GameInputFocusController controller;
    QSignalSpy transitions(&controller, &GameInputFocusController::focusPolicyTransitioned);

    controller.attach(raw);
    QVERIFY(controller.requestExclusiveForeground(QStringLiteral("overlay-interactive")));
    QVERIFY(controller.requestExclusiveForeground(QStringLiteral("overlay-interactive")));  // duplicate
    controller.restoreBackground(QStringLiteral("the overlay closed"));
    controller.restoreBackground(QStringLiteral("the overlay closed"));                     // duplicate
    controller.detach();

    QCOMPARE(transitions.size(), 4);   // attach, request, release, detach — duplicates silent
    const QList<QVariant> expectedModes = { QStringLiteral("background"),
                                            QStringLiteral("exclusive-foreground"),
                                            QStringLiteral("background"),
                                            QStringLiteral("background") };
    const QList<QVariant> expectedReasons = { QStringLiteral("runtime-attached"),
                                              QStringLiteral("overlay-interactive"),
                                              QStringLiteral("the overlay closed"),
                                              QStringLiteral("runtime-stopped") };
    for (int i = 0; i < transitions.size(); ++i) {
        QCOMPARE(transitions.at(i).at(0), expectedModes.at(i));
        QCOMPARE(transitions.at(i).at(1), expectedReasons.at(i));
    }

    // The announcement cannot drift from the runtime call log.
    QCOMPARE(policyEntries(*raw).size(), transitions.size());
    QCOMPARE(policyEntries(*raw).last(), QStringLiteral("focus-policy:background"));
}

void TestGameInputFocus::theCounterCountsPolicyApplications()
{
    auto fake = std::make_unique<FakeGameInputApi>();
    FakeGameInputApi* raw = fake.get();
    GameInputFocusController controller;
    controller.attach(raw);
    QVERIFY(controller.requestExclusiveForeground(QStringLiteral("overlay-interactive")));
    controller.restoreBackground(QStringLiteral("the overlay closed"));

    // The counter exists so a caller can prove its own actions did not multiply
    // policy applications; it must therefore equal the runtime's own log.
    QCOMPARE(controller.focusTransitionCount(), policyEntries(*raw).size());
}

void TestGameInputFocus::theMasksAreTheDocumentedOnes()
{
    // The vendor constants, as the vendored GameInput.h defines them. Passing
    // them in keeps the mapping testable without the SDK and pins the three rules
    // the leaf promised the reviewer: background system buttons in BOTH modes,
    // exclusive input replacing background input, and guide/share never made
    // exclusive.
    ModernInput::GameInputFocusPolicyFlags flags;
    flags.backgroundInput = 0x40;
    flags.exclusiveForegroundInput = 0x02;
    flags.backgroundGuideButton = 0x80;
    flags.backgroundShareButton = 0x100;
    const int exclusiveGuideButton = 0x08;
    const int exclusiveShareButton = 0x20;

    const int background = ModernInput::gameInputFocusMask(GameInputFocusMode::Background, flags);
    QCOMPARE(background, 0x40 | 0x80 | 0x100);
    QVERIFY((background & flags.exclusiveForegroundInput) == 0);
    QVERIFY((background & exclusiveGuideButton) == 0);
    QVERIFY((background & exclusiveShareButton) == 0);

    const int exclusive =
        ModernInput::gameInputFocusMask(GameInputFocusMode::ExclusiveForeground, flags);
    QCOMPARE(exclusive, 0x02 | 0x80 | 0x100);
    QVERIFY(ModernInput::gameInputFocusModeName(GameInputFocusMode::ExclusiveForeground)
                == QStringLiteral("exclusive-foreground"));

    // Exactly one bit differs between the two states, and it is the standard
    // input axis: nothing about the system buttons changes.
    QCOMPARE(background ^ exclusive, 0x40 | 0x02);
    QVERIFY((exclusive & flags.backgroundGuideButton) == flags.backgroundGuideButton);
    QVERIFY((exclusive & flags.backgroundShareButton) == flags.backgroundShareButton);
}

void TestGameInputFocus::theExportPhrasingCannotBeMistakenForIsolation()
{
    const QString exclusive =
        ModernInput::gameInputFocusPolicyDescription(GameInputFocusMode::ExclusiveForeground);
    // The record has to read as policy control, not as a measurement.
    QVERIFY2(exclusive.contains(QStringLiteral("exclusive-foreground input")), qPrintable(exclusive));
    QVERIFY2(exclusive.contains(QStringLiteral("not verified in-process")), qPrintable(exclusive));
    QVERIFY2(exclusive.contains(QStringLiteral("background guide")), qPrintable(exclusive));
    QVERIFY2(!exclusive.contains(QStringLiteral("isolated")), qPrintable(exclusive));

    const QString background =
        ModernInput::gameInputFocusPolicyDescription(GameInputFocusMode::Background);
    QVERIFY2(background.contains(QStringLiteral("no exclusive-foreground flags")),
             qPrintable(background));
}

QTEST_GUILESS_MAIN(TestGameInputFocus)

#include "tst_gameinputfocus.moc"
