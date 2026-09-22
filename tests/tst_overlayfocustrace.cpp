#include "overlay/OverlayFocusTrace.h"

#include <QtTest>

#include <functional>

// cpo-o06a: the overlay's focus/controller record, with every Win32 query
// already replaced by the fact it would resolve to (OverlayManager is the only
// place those facts come from real Windows).
//
// What matters here is that the record cannot overstate what it observed. The
// isolation question this whole item exists for — "did the game stop receiving
// the pad?" — is NOT answerable inside GameHQ's process, so the trace must say
// so in every line it emits, and a successful foreground grab must never be
// formatted as if it were isolation.
namespace
{
const void* hwnd(quintptr id) { return reinterpret_cast<const void*>(id); }

OverlayFocus::WindowFacts liveWindow(quintptr id, unsigned long pid)
{
    OverlayFocus::WindowFacts facts;
    facts.handle = hwnd(id);
    facts.exists = true;
    facts.visible = true;
    facts.iconic = false;
    facts.pid = pid;
    facts.left = 0;
    facts.top = 0;
    facts.right = 2560;
    facts.bottom = 1440;
    return facts;
}

// The state GameHQ ships today: overlay presented without activation, so the
// game window still owns the foreground.
OverlayFocus::ShowTrace nonActivatingShow()
{
    OverlayFocus::ShowTrace trace;
    trace.game = liveWindow(0x1111, 4242);
    trace.overlay = liveWindow(0x2222, 99);
    trace.foregroundBefore = hwnd(0x1111);
    trace.foregroundAfterPresent = hwnd(0x1111);
    trace.foregroundAfterActivation = hwnd(0x1111);
    trace.activationRequested = false;
    trace.overlayActiveQt = false;
    trace.overlayForegroundWin32 = false;
    trace.controllerProvider = QStringLiteral("Sony controller");
    trace.controllerProfile = QStringLiteral("p-1a2b3c");
    trace.gameInputFocusPolicy = QStringLiteral("background input + guide + share");
    return trace;
}
}  // namespace

class TestOverlayFocusTrace : public QObject
{
    Q_OBJECT
private slots:
    void handleFormattingIsStable();
    void destroyedWindowStatesNothingElse();
    void nonActivatingShowReportsGameForeground();
    void activatedOverlayReportsOverlayForeground();
    void activationFieldDecidesWhichForegroundCounts();
    void qtAndWin32DisagreementIsVisible();
    void showNeverClaimsIsolation();
    void interactiveForegroundNeedsTheGameStillOnScreen();
    void aMinimizedGameBreaksTheInteractiveVerdict();
    void aDeniedRequestIsReportedAsNotAcquired();
    void hideReportsRestoreOutcome();
    void hideWithoutRememberedGameIsNotRestored();
    // cpo-o06c
    void exclusivePolicyIsRequestedOnlyForVerifiedInteractiveForeground();
    void exclusivePolicyIsNeverRequestedWhenTheForegroundRequestDidNotSucceed();
    void theTwoDependentPredicatesCannotDisagree();
    void policyRefusalsNameTheClauseThatRefusedThem();
    void restoreReasonNamesTheActualExitPath();
    void policyFactsAndLinesStaySelfContained();
};

void TestOverlayFocusTrace::handleFormattingIsStable()
{
    QCOMPARE(OverlayFocus::formatHandle(nullptr), QStringLiteral("none"));
    QCOMPARE(OverlayFocus::formatHandle(hwnd(0x1a2b)), QStringLiteral("0x1a2b"));
}

void TestOverlayFocusTrace::destroyedWindowStatesNothingElse()
{
    // A handle Windows no longer knows must not be dressed up with a rect or a
    // pid left over from a previous read: the report says "destroyed", period.
    OverlayFocus::WindowFacts gone;
    gone.handle = hwnd(0x1111);
    gone.exists = false;
    QCOMPARE(gone.toLogString(), QStringLiteral("0x1111 destroyed"));

    OverlayFocus::WindowFacts absent;
    QCOMPARE(absent.toLogString(), QStringLiteral("none"));

    const QString live = liveWindow(0x1111, 4242).toLogString();
    QVERIFY(live.contains(QStringLiteral("visible=1")));
    QVERIFY(live.contains(QStringLiteral("iconic=0")));
    QVERIFY(live.contains(QStringLiteral("pid=4242")));
    QVERIFY(live.contains(QStringLiteral("rect=0,0-2560,1440")));
}

void TestOverlayFocusTrace::nonActivatingShowReportsGameForeground()
{
    const OverlayFocus::ShowTrace trace = nonActivatingShow();
    QVERIFY(trace.foregroundPreserved());
    QVERIFY(!trace.overlayOwnsForeground());
    QVERIFY(!trace.qtWin32Disagree());

    const QString line = trace.toLogString();
    QVERIFY(line.contains(QStringLiteral("game-kept-foreground=yes")));
    QVERIFY(line.contains(QStringLiteral("overlay-foreground=no")));
    QVERIFY(line.contains(QStringLiteral("provider=Sony controller")));
    QVERIFY(line.contains(QStringLiteral("profile=p-1a2b3c")));
}

void TestOverlayFocusTrace::activatedOverlayReportsOverlayForeground()
{
    // The cpo-o06b variant: activation was asked for and Windows granted it.
    OverlayFocus::ShowTrace trace = nonActivatingShow();
    trace.activationRequested = true;
    trace.foregroundAfterActivation = trace.overlay.handle;
    trace.overlayActiveQt = true;
    trace.overlayForegroundWin32 = true;

    QVERIFY(trace.overlayOwnsForeground());
    // The game still kept the foreground through PRESENTATION — that fact is
    // about the presenter and must not be rewritten by a later activation.
    QVERIFY(trace.foregroundPreserved());
    QVERIFY(trace.toLogString().contains(QStringLiteral("activation-requested=yes")));
}

void TestOverlayFocusTrace::activationFieldDecidesWhichForegroundCounts()
{
    // Without a request, a post-activation value that happens to name the
    // overlay must not be read as the overlay owning the foreground: nothing
    // asked for it, so the presentation result is the answer.
    OverlayFocus::ShowTrace trace = nonActivatingShow();
    trace.foregroundAfterActivation = trace.overlay.handle;
    QVERIFY(!trace.overlayOwnsForeground());
    QCOMPARE(trace.finalForeground(), trace.foregroundAfterPresent);

    trace.activationRequested = true;
    QVERIFY(trace.overlayOwnsForeground());
    QCOMPARE(trace.finalForeground(), trace.overlay.handle);
}

void TestOverlayFocusTrace::qtAndWin32DisagreementIsVisible()
{
    // The exact state that made earlier focus attempts look successful: Qt
    // believes it is active, Windows still points somewhere else.
    OverlayFocus::ShowTrace trace = nonActivatingShow();
    trace.overlayActiveQt = true;
    trace.overlayForegroundWin32 = false;
    QVERIFY(trace.qtWin32Disagree());
    QVERIFY(trace.toLogString().contains(QStringLiteral("qt/win32-disagree=yes")));
}

void TestOverlayFocusTrace::showNeverClaimsIsolation()
{
    // Every open record, including one where the overlay won the foreground,
    // has to state that isolation was not measured here.
    OverlayFocus::ShowTrace trace = nonActivatingShow();
    trace.activationRequested = true;
    trace.foregroundAfterActivation = trace.overlay.handle;
    const QString line = trace.toLogString();
    QVERIFY(trace.overlayOwnsForeground());
    QVERIFY(line.contains(QStringLiteral("isolation=not measured in-process")));
    QVERIFY(!line.contains(QStringLiteral("isolated")));
}

void TestOverlayFocusTrace::hideReportsRestoreOutcome()
{
    OverlayFocus::HideTrace trace;
    trace.foregroundBefore = hwnd(0x2222);
    trace.foregroundAfter = hwnd(0x1111);
    trace.restoreTarget = hwnd(0x1111);
    trace.restoreRequested = true;
    trace.restored = true;
    trace.providerBefore = QStringLiteral("Sony controller");
    trace.providerAfter = QStringLiteral("Sony controller");

    QVERIFY(!trace.providerChanged());
    const QString line = trace.toLogString();
    QVERIFY(line.contains(QStringLiteral("restored=yes")));
    // Until cpo-o06e implements it, the record must admit the handoff is
    // missing rather than leaving the field blank.
    QVERIFY(line.contains(QStringLiteral("neutral-handoff=not implemented")));
}

void TestOverlayFocusTrace::hideWithoutRememberedGameIsNotRestored()
{
    // No remembered game window means there is nothing to restore to, so the
    // record must not read as a successful restore just because the
    // foreground and the (absent) target are both null.
    OverlayFocus::HideTrace trace;
    trace.foregroundBefore = nullptr;
    trace.foregroundAfter = nullptr;
    trace.restoreTarget = nullptr;
    trace.restored = trace.restoreTarget != nullptr
        && trace.foregroundAfter == trace.restoreTarget;
    QVERIFY(!trace.restored);
    QVERIFY(trace.toLogString().contains(QStringLiteral("restore-target=none")));
    QVERIFY(trace.toLogString().contains(QStringLiteral("restored=no")));
}

QTEST_MAIN(TestOverlayFocusTrace)
// cpo-o06b: the acceptance verdict is a conjunction on purpose. A foreground
// grab that succeeded while the game minimized itself is a failure of this
// leaf, and the record must be able to say so.
void TestOverlayFocusTrace::interactiveForegroundNeedsTheGameStillOnScreen()
{
    OverlayFocus::ShowTrace trace = nonActivatingShow();
    trace.activationRequested = true;
    trace.foregroundAfterActivation = hwnd(0x2222);
    trace.acquisitionSucceeded = true;
    trace.acquisitionAttempts = 1;
    trace.gameAfterAcquisition = liveWindow(0x1111, 4242);
    trace.overlayAfterAcquisition = liveWindow(0x2222, 99);

    QVERIFY(trace.overlayOwnsForeground());
    QVERIFY(trace.interactiveForegroundTruth());
    const QString line = trace.toLogString();
    QVERIFY2(line.contains(QStringLiteral("interactive-foreground=yes")), qPrintable(line));
    QVERIFY2(line.contains(QStringLiteral("acquisition=acquired attempts=1")), qPrintable(line));
}

void TestOverlayFocusTrace::aMinimizedGameBreaksTheInteractiveVerdict()
{
    OverlayFocus::ShowTrace trace = nonActivatingShow();
    trace.activationRequested = true;
    trace.foregroundAfterActivation = hwnd(0x2222);
    trace.acquisitionSucceeded = true;
    trace.acquisitionAttempts = 1;
    trace.overlayAfterAcquisition = liveWindow(0x2222, 99);

    // The overlay really is the foreground window — and the leaf still failed.
    trace.gameAfterAcquisition = liveWindow(0x1111, 4242);
    trace.gameAfterAcquisition.iconic = true;
    QVERIFY(trace.overlayOwnsForeground());
    QVERIFY(!trace.interactiveForegroundTruth());

    // Same for a game that hid its window rather than minimizing it, and for
    // one that is gone entirely.
    trace.gameAfterAcquisition.iconic = false;
    trace.gameAfterAcquisition.visible = false;
    QVERIFY(!trace.interactiveForegroundTruth());

    trace.gameAfterAcquisition = OverlayFocus::WindowFacts{};
    QVERIFY(!trace.interactiveForegroundTruth());
    QVERIFY2(trace.toLogString().contains(QStringLiteral("interactive-foreground=no")),
             qPrintable(trace.toLogString()));
}

void TestOverlayFocusTrace::aDeniedRequestIsReportedAsNotAcquired()
{
    OverlayFocus::ShowTrace trace = nonActivatingShow();
    trace.activationRequested = true;
    trace.acquisitionAttempts = 3;
    trace.acquisitionSucceeded = false;
    trace.gameAfterAcquisition = liveWindow(0x1111, 4242);
    trace.overlayAfterAcquisition = liveWindow(0x2222, 99);

    // Asked and refused: the game kept the foreground, so the record must not
    // read like the overlay got it.
    QVERIFY(!trace.overlayOwnsForeground());
    QVERIFY(!trace.interactiveForegroundTruth());
    const QString line = trace.toLogString();
    QVERIFY2(line.contains(QStringLiteral("acquisition=not-acquired attempts=3")), qPrintable(line));
    QVERIFY2(line.contains(QStringLiteral("activation-requested=yes")), qPrintable(line));
    QVERIFY2(line.contains(OverlayFocus::ShowTrace::isolationEvidence()), qPrintable(line));
}

namespace
{
// The state cpo-o06b established and cpo-o06c is allowed to act on: the overlay
// owns the foreground while the game stays alive, visible and un-minimized.
OverlayFocus::ShowTrace verifiedInteractiveShow()
{
    OverlayFocus::ShowTrace trace = nonActivatingShow();
    trace.activationRequested = true;
    trace.acquisitionAttempts = 1;
    trace.acquisitionSucceeded = true;
    trace.foregroundAfterActivation = hwnd(0x2222);
    trace.overlayActiveQt = true;
    trace.overlayForegroundWin32 = true;
    trace.gameAfterAcquisition = liveWindow(0x1111, 4242);
    trace.overlayAfterAcquisition = liveWindow(0x2222, 99);
    return trace;
}
}  // namespace

void TestOverlayFocusTrace::exclusivePolicyIsRequestedOnlyForVerifiedInteractiveForeground()
{
    const OverlayFocus::ShowTrace verified = verifiedInteractiveShow();
    QVERIFY(verified.interactiveForegroundTruth());

    const OverlayFocus::GameInputPolicyDecision decision =
        OverlayFocus::decideGameInputPolicy(verified);
    QVERIFY(decision.request);
    QVERIFY2(decision.reason.contains(QStringLiteral("verified interactive foreground")),
             qPrintable(decision.reason));

    // Presented but never activated — the state that shipped before o06b — asks
    // for nothing, and says so.
    const OverlayFocus::GameInputPolicyDecision untouched =
        OverlayFocus::decideGameInputPolicy(nonActivatingShow());
    QVERIFY(!untouched.request);
    QCOMPARE(untouched.reason, QStringLiteral("no foreground request was made"));
}

void TestOverlayFocusTrace::exclusivePolicyIsNeverRequestedWhenTheForegroundRequestDidNotSucceed()
{
    // Denied, or cancelled because the overlay was closing while the request was
    // still retrying: even if the overlay happens to hold the foreground at that
    // instant, asking for a policy that is about to be released would only widen
    // the window in which another process can lose input.
    OverlayFocus::ShowTrace trace = verifiedInteractiveShow();
    trace.acquisitionSucceeded = false;
    QVERIFY(trace.interactiveForegroundTruth());   // the truth condition alone would allow it
    const OverlayFocus::GameInputPolicyDecision decision =
        OverlayFocus::decideGameInputPolicy(trace);
    QVERIFY(!decision.request);
    QCOMPARE(decision.reason, QStringLiteral("foreground was not acquired"));
}

void TestOverlayFocusTrace::theTwoDependentPredicatesCannotDisagree()
{
    // decideGameInputPolicy() mirrors interactiveForegroundTruth() clause by
    // clause, so a later edit to one of them must not silently leave the other
    // behind: walk a matrix of broken clauses and require the implication to hold
    // in both directions (plus the acquirer's verdict on top).
    for (int broken = 0; broken < 6; ++broken) {
        OverlayFocus::ShowTrace trace = verifiedInteractiveShow();
        switch (broken) {
        case 0: trace.activationRequested = false; break;
        case 1: trace.acquisitionSucceeded = false; break;
        case 2: trace.gameAfterAcquisition.exists = false; break;
        case 3: trace.gameAfterAcquisition.iconic = true; break;
        case 4: trace.gameAfterAcquisition.visible = false; break;
        case 5: trace.overlayAfterAcquisition.visible = false; break;
        default: break;
        }

        const bool truth = trace.interactiveForegroundTruth();
        const bool request = OverlayFocus::decideGameInputPolicy(trace).request;
        QVERIFY2(!request || truth, "a policy request without the verified interactive state");
        QVERIFY2(!(truth && trace.acquisitionSucceeded) || request,
                 "the verified interactive state must be what asks for the policy");
    }

    const OverlayFocus::ShowTrace intact = verifiedInteractiveShow();
    QVERIFY(intact.interactiveForegroundTruth());
    QVERIFY(OverlayFocus::decideGameInputPolicy(intact).request);
}

void TestOverlayFocusTrace::policyRefusalsNameTheClauseThatRefusedThem()
{
    struct Case
    {
        std::function<void(OverlayFocus::ShowTrace&)> breakIt;
        QString expected;
    };
    const Case cases[] = {
        { [](OverlayFocus::ShowTrace& t) { t.gameAfterAcquisition.exists = false; },
          QStringLiteral("the game window is gone") },
        { [](OverlayFocus::ShowTrace& t) { t.gameAfterAcquisition.iconic = true; },
          QStringLiteral("the game minimized itself") },
        { [](OverlayFocus::ShowTrace& t) { t.gameAfterAcquisition.visible = false; },
          QStringLiteral("the game window is no longer visible") },
        { [](OverlayFocus::ShowTrace& t) { t.overlayAfterAcquisition.exists = false; },
          QStringLiteral("the overlay window is not visible") },
        { [](OverlayFocus::ShowTrace& t) {
              t.foregroundAfterActivation = t.foregroundBefore;
              t.overlayForegroundWin32 = false;
          },
          QStringLiteral("the overlay does not own the foreground") },
    };
    for (const Case& item : cases) {
        OverlayFocus::ShowTrace trace = verifiedInteractiveShow();
        item.breakIt(trace);
        const OverlayFocus::GameInputPolicyDecision decision =
            OverlayFocus::decideGameInputPolicy(trace);
        QVERIFY2(!decision.request, qPrintable(item.expected));
        QCOMPARE(decision.reason, item.expected);
    }
}

void TestOverlayFocusTrace::restoreReasonNamesTheActualExitPath()
{
    // Most specific fact first, so the close record says what actually ended the
    // interactive state instead of always blaming the close.
    QCOMPARE(OverlayFocus::gameInputRestoreReason(false, false, true, false),
             QStringLiteral("the game window is gone"));
    QCOMPARE(OverlayFocus::gameInputRestoreReason(true, true, true, false),
             QStringLiteral("the game was minimized"));
    QCOMPARE(OverlayFocus::gameInputRestoreReason(true, false, false, false),
             QStringLiteral("the overlay lost the foreground"));
    QCOMPARE(OverlayFocus::gameInputRestoreReason(true, false, true, true),
             QStringLiteral("desktop handoff"));
    QCOMPARE(OverlayFocus::gameInputRestoreReason(true, false, true, false),
             QStringLiteral("the overlay closed"));
}

void TestOverlayFocusTrace::policyFactsAndLinesStaySelfContained()
{
    OverlayFocus::ShowTrace trace = verifiedInteractiveShow();
    trace.gameInputPolicy.mode = QStringLiteral("exclusive-foreground");
    trace.gameInputPolicy.request = QStringLiteral("requested");
    trace.gameInputPolicy.reason = QStringLiteral("overlay holds the verified interactive foreground");
    trace.gameInputPolicy.transitions = 1;

    QVERIFY(trace.gameInputPolicy.exclusiveApplied());
    QVERIFY(trace.gameInputPolicy.requested());
    const QString policy = trace.gameInputPolicy.toLogString();
    for (const QString& field : { QStringLiteral("mode=exclusive-foreground"),
                                  QStringLiteral("request=requested"),
                                  QStringLiteral("reason=\"overlay holds the verified interactive foreground\""),
                                  QStringLiteral("transitions=1") }) {
        QVERIFY2(policy.contains(field), qPrintable(policy));
    }

    const QString line = trace.toLogString();
    QVERIFY2(line.contains(QStringLiteral("gameinput-policy=[")), qPrintable(line));
    QVERIFY2(line.contains(QStringLiteral("mode=exclusive-foreground")), qPrintable(line));
    // The whole point of the record: a policy request is still not isolation.
    QVERIFY2(line.contains(OverlayFocus::ShowTrace::isolationEvidence()), qPrintable(line));

    OverlayFocus::HideTrace hide;
    hide.gameInputPolicy.mode = QStringLiteral("background");
    hide.gameInputPolicy.request = QStringLiteral("released");
    hide.gameInputPolicy.reason = QStringLiteral("the overlay closed");
    const QString hideLine = hide.toLogString();
    QVERIFY2(hideLine.contains(QStringLiteral("gameinput-policy=[")), qPrintable(hideLine));
    QVERIFY2(hideLine.contains(QStringLiteral("request=released")), qPrintable(hideLine));
    QVERIFY2(hideLine.contains(QStringLiteral("the overlay closed")), qPrintable(hideLine));
}

#include "tst_overlayfocustrace.moc"
