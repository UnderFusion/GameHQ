#include "overlay/OverlayPresenter.h"

#include "overlay/ForegroundApi.h"

#include <QGuiApplication>
#include <QScreen>
#include <QWindow>
#include <QtTest>

#include <windows.h>

// cpo-o02: the overlay must never take the foreground away from the game.
// Two layers of proof:
//   * a recording fake that makes every native call inspectable — including
//     the handle swap Qt performs when it rebuilds a native window, which no
//     real machine reproduces on demand;
//   * one real-window case on the actual production adapter.
namespace
{
struct PosCall
{
    void* hwnd = nullptr;
    void* insertAfter = nullptr;
    unsigned flags = 0;
};

void* hwnd(quintptr id) { return reinterpret_cast<void*>(id); }

class FakeOverlayWindowApi final : public OverlayWindowApi
{
public:
    void* handle = hwnd(0x100);
    QMap<void*, unsigned long> styles;
    QList<PosCall> posCalls;
    QList<QRect> geometries;
    int shows = 0;
    void* foreground = hwnd(0x900);   // "the game"
    // Emulates Qt rebuilding the native window during the named step.
    QString recreateOn;
    void* recreateTo = hwnd(0x200);

    void* nativeHandle() override { return handle; }

    unsigned long extendedStyle(void* h) override { return styles.value(h, 0); }

    void setExtendedStyle(void* h, unsigned long style) override { styles[h] = style; }

    void setWindowPos(void* h, void* insertAfter, unsigned flags) override
    {
        posCalls.append(PosCall{h, insertAfter, flags});
    }

    void setGeometry(const QRect& rect) override
    {
        geometries.append(rect);
        maybeRecreate(QStringLiteral("geometry"));
    }

    void showWindow() override
    {
        ++shows;
        maybeRecreate(QStringLiteral("show"));
    }

    void* foregroundWindow() override { return foreground; }

    bool everyPosCallIsNonActivating() const
    {
        for (const PosCall& call : posCalls) {
            if (!(call.flags & OverlayWin32::kSwpNoActivate))
                return false;
        }
        return true;
    }

private:
    void maybeRecreate(const QString& step)
    {
        if (recreateOn == step)
            handle = recreateTo;
    }
};

std::unique_ptr<OverlayPresenter> presenterFor(FakeOverlayWindowApi* api)
{
    return std::make_unique<OverlayPresenter>(std::unique_ptr<OverlayWindowApi>(api));
}
}  // namespace

class OverlayPresenterTest : public QObject
{
    Q_OBJECT

private slots:
    void firstShowStylesTheWindowBeforeShowingIt()
    {
        auto* api = new FakeOverlayWindowApi;
        auto presenter = presenterFor(api);

        const OverlayPresentReport report = presenter->present(QRect(0, 0, 1920, 1080));

        // The ex-style must be on the window before it can become visible:
        // the first recorded native call happens while shows == 0.
        QVERIFY(report.styleApplied);
        QVERIFY(api->styles.value(hwnd(0x100)) & OverlayWin32::kExNoActivate);
        QCOMPARE(api->shows, 1);
        QCOMPARE(api->geometries, QList<QRect>{QRect(0, 0, 1920, 1080)});
        QVERIFY(api->everyPosCallIsNonActivating());
        QCOMPARE(report.handle, hwnd(0x100));
        QVERIFY(report.foregroundPreserved());
    }

    void showPinsTopmostWithoutActivating()
    {
        auto* api = new FakeOverlayWindowApi;
        auto presenter = presenterFor(api);

        presenter->present(QRect(0, 0, 800, 600));

        QVERIFY(!api->posCalls.isEmpty());
        const PosCall last = api->posCalls.last();
        QCOMPARE(last.insertAfter, OverlayWin32::topmost());
        QVERIFY(last.flags & OverlayWin32::kSwpNoActivate);
        QVERIFY(last.flags & OverlayWin32::kSwpNoMove);
        QVERIFY(last.flags & OverlayWin32::kSwpNoSize);
    }

    void repeatedShowKeepsTheGuaranteeWithoutRewritingTheStyle()
    {
        auto* api = new FakeOverlayWindowApi;
        auto presenter = presenterFor(api);

        presenter->present(QRect(0, 0, 800, 600));
        const int callsAfterFirst = api->posCalls.size();
        const OverlayPresentReport second = presenter->present(QRect(0, 0, 800, 600));

        QVERIFY(!second.styleApplied);          // already styled, nothing to redo
        QVERIFY(!second.recreated);
        QCOMPARE(api->shows, 2);
        QCOMPARE(api->posCalls.size(), callsAfterFirst + 1);   // topmost only
        QVERIFY(api->everyPosCallIsNonActivating());
        QVERIFY(second.foregroundPreserved());
    }

    void repositioningToAnotherScreenNeverActivates()
    {
        auto* api = new FakeOverlayWindowApi;
        auto presenter = presenterFor(api);

        presenter->present(QRect(0, 0, 1920, 1080));
        presenter->present(QRect(-2560, -200, 2560, 1440));   // monitor left of primary

        QCOMPARE(api->geometries.size(), 2);
        QCOMPARE(api->geometries.last(), QRect(-2560, -200, 2560, 1440));
        QVERIFY(api->everyPosCallIsNonActivating());
    }

    void aRebuiltNativeWindowIsStyledAgainBeforeItIsShown()
    {
        auto* api = new FakeOverlayWindowApi;
        api->recreateOn = QStringLiteral("geometry");   // Qt swaps the HWND on the move
        auto presenter = presenterFor(api);

        presenter->present(QRect(0, 0, 800, 600));      // adopts 0x200 mid-call
        const OverlayPresentReport report = presenter->present(QRect(0, 0, 800, 600));

        QVERIFY(api->styles.value(hwnd(0x200)) & OverlayWin32::kExNoActivate);
        QCOMPARE(report.handle, hwnd(0x200));
        QVERIFY(api->everyPosCallIsNonActivating());
        QVERIFY(report.foregroundPreserved());
    }

    void recreationDuringShowIsStillCaughtBeforeTheWindowIsRaised()
    {
        auto* api = new FakeOverlayWindowApi;
        api->recreateOn = QStringLiteral("show");
        auto presenter = presenterFor(api);

        const OverlayPresentReport report = presenter->present(QRect(0, 0, 800, 600));

        QVERIFY(api->styles.value(hwnd(0x200)) & OverlayWin32::kExNoActivate);
        QCOMPARE(api->posCalls.last().hwnd, hwnd(0x200));
        QCOMPARE(api->posCalls.last().insertAfter, OverlayWin32::topmost());
        QVERIFY(api->everyPosCallIsNonActivating());
    }

    void screenChangeReassertsTheStyleOnTheNewHandle()
    {
        auto* api = new FakeOverlayWindowApi;
        auto presenter = presenterFor(api);
        presenter->present(QRect(0, 0, 800, 600));

        api->handle = hwnd(0x300);                      // rebuilt by the screen change
        const OverlayPresentReport report = presenter->reassert();

        QVERIFY(report.recreated);
        QVERIFY(report.styleApplied);
        QVERIFY(api->styles.value(hwnd(0x300)) & OverlayWin32::kExNoActivate);
        QCOMPARE(api->shows, 1);                        // reassert never shows
        QVERIFY(api->geometries.size() == 1);           // and never moves
        QVERIFY(api->everyPosCallIsNonActivating());
    }

    void aStyleStrippedByWindowsIsRestored()
    {
        auto* api = new FakeOverlayWindowApi;
        auto presenter = presenterFor(api);
        presenter->present(QRect(0, 0, 800, 600));

        api->styles[hwnd(0x100)] = 0;                   // style lost under us
        const OverlayPresentReport report = presenter->reassert();

        QVERIFY(report.styleApplied);
        QVERIFY(api->styles.value(hwnd(0x100)) & OverlayWin32::kExNoActivate);
    }

    // The production adapter against real Windows: another top-level window
    // holds the foreground, the overlay is presented, and the foreground must
    // not move. Skipped only when this session cannot hold a foreground
    // window at all (headless/locked desktop) — never on a failure.
    // --- cpo-o06b: the activatable mode ------------------------------------

    void makeActivatableClearsTheStyleWithoutTouchingAnythingElse()
    {
        auto* api = new FakeOverlayWindowApi;
        auto presenter = presenterFor(api);
        presenter->present(QRect(0, 0, 1920, 1080));
        const int showsAfterPresent = api->shows;
        const int geometriesAfterPresent = api->geometries.size();

        const OverlayPresentReport report = presenter->makeActivatable();

        QVERIFY(report.activatable);
        QVERIFY(report.styleApplied);
        QVERIFY(!(api->styles.value(hwnd(0x100)) & OverlayWin32::kExNoActivate));
        // Presentation state is untouched: no show, no geometry, and the one
        // positioning call it makes still refuses to activate.
        QCOMPARE(api->shows, showsAfterPresent);
        QCOMPARE(api->geometries.size(), geometriesAfterPresent);
        QVERIFY(api->everyPosCallIsNonActivating());
        QVERIFY(presenter->isActivatable());
    }

    void presentationItselfNeverMakesTheWindowActivatable()
    {
        auto* api = new FakeOverlayWindowApi;
        auto presenter = presenterFor(api);

        const OverlayPresentReport report = presenter->present(QRect(0, 0, 1920, 1080));

        // Whatever cpo-o06b does afterwards, the window becomes visible
        // unactivatable first: a foreground request is a separate decision.
        QVERIFY(!report.activatable);
        QVERIFY(api->styles.value(hwnd(0x100)) & OverlayWin32::kExNoActivate);
    }

    void theActivatableModeSurvivesAReassert()
    {
        auto* api = new FakeOverlayWindowApi;
        auto presenter = presenterFor(api);
        presenter->present(QRect(0, 0, 1920, 1080));
        presenter->makeActivatable();

        // A screen change re-asserts on whatever handle exists now. An
        // interactive overlay must not silently fall back to unactivatable —
        // it would keep the foreground it has and lose it on the next
        // transition.
        const OverlayPresentReport report = presenter->reassert();

        QVERIFY(report.activatable);
        QVERIFY(!(api->styles.value(hwnd(0x100)) & OverlayWin32::kExNoActivate));
    }

    void aRebuiltHandleIsMadeActivatableAgain()
    {
        auto* api = new FakeOverlayWindowApi;
        auto presenter = presenterFor(api);
        presenter->present(QRect(0, 0, 1920, 1080));
        presenter->makeActivatable();

        // Qt rebuilds the native window: the new handle starts with whatever
        // Qt gave it, so the mode has to be written onto it, not assumed.
        api->styles[hwnd(0x200)] = OverlayWin32::kExNoActivate;
        api->handle = hwnd(0x200);
        const OverlayPresentReport report = presenter->reassert();

        QVERIFY(report.recreated);
        QVERIFY(report.styleApplied);
        QVERIFY(!(api->styles.value(hwnd(0x200)) & OverlayWin32::kExNoActivate));
    }

    void resettingTheActivationPolicyRestoresTheGuaranteeOnTheNextPresent()
    {
        auto* api = new FakeOverlayWindowApi;
        auto presenter = presenterFor(api);
        presenter->present(QRect(0, 0, 1920, 1080));
        presenter->makeActivatable();

        // Every open starts from the proven path: a closed overlay must not
        // stay activatable until someone asks for the foreground again.
        presenter->resetActivationPolicy();
        QVERIFY(!presenter->isActivatable());
        const OverlayPresentReport report = presenter->present(QRect(0, 0, 1920, 1080));

        QVERIFY(!report.activatable);
        QVERIFY(report.styleApplied);
        QVERIFY(api->styles.value(hwnd(0x100)) & OverlayWin32::kExNoActivate);
        QVERIFY(api->everyPosCallIsNonActivating());
    }

    void realWindowShowLeavesTheOtherWindowForeground()
    {
        QWindow other;
        other.setTitle(QStringLiteral("gamehq-test-foreground"));
        other.setGeometry(QRect(50, 50, 320, 240));
        other.show();
        QTest::qWait(200);

        const HWND otherHwnd = reinterpret_cast<HWND>(other.winId());
        // requestActivate() alone loses to the Win32 foreground lock when the
        // test was not started by a user click, so use the same AttachThreadInput
        // path the app itself uses for the desktop summon.
        std::unique_ptr<ForegroundApi> foreground(ForegroundApi::createSystem());
        foreground->forceForeground(otherHwnd);
        QTest::qWait(200);
        if (GetForegroundWindow() != otherHwnd)
            QSKIP("this session cannot put a test window in the foreground");

        QWindow overlay;
        overlay.setFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                         | Qt::Tool | Qt::WindowDoesNotAcceptFocus);
        OverlayPresenter presenter(makeQWindowOverlayApi(&overlay));

        const QRect screen = QGuiApplication::primaryScreen()->geometry();
        const OverlayPresentReport shown = presenter.present(screen);
        QTest::qWait(200);

        QVERIFY(GetWindowLongPtrW(reinterpret_cast<HWND>(overlay.winId()), GWL_EXSTYLE)
                & WS_EX_NOACTIVATE);
        QCOMPARE(GetForegroundWindow(), otherHwnd);
        QVERIFY2(shown.foregroundPreserved(), qPrintable(shown.toLogString()));

        // Repeated show and a reposition must not change that either.
        presenter.present(screen.adjusted(0, 0, -100, -100));
        presenter.reassert();
        QTest::qWait(200);
        QCOMPARE(GetForegroundWindow(), otherHwnd);
    }
};

QTEST_MAIN(OverlayPresenterTest)
#include "tst_overlaypresenter.moc"
