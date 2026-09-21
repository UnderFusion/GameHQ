#include "overlay/OverlayFocusTrace.h"

#include <QtTest>

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
    void hideReportsRestoreOutcome();
    void hideWithoutRememberedGameIsNotRestored();
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
#include "tst_overlayfocustrace.moc"
