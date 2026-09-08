#include "core/WindowPlacement.h"

#include <QTest>

// M11 — window restore on any monitor. WindowPlacement is plain rectangle
// logic, so the whole rule set (negative-coordinate monitors, partly visible
// windows, a display that was unplugged since the last run) is checked here
// without a screen, a window or a QML engine.
class TestWindowPlacement : public QObject
{
    Q_OBJECT

private:
    // A three-monitor desktop: 1920x1080 primary at the origin, a second
    // monitor to its left (negative X) and a third above it (negative Y). The
    // work areas are one taskbar height shorter than the physical screens,
    // which is what QScreen::availableGeometry() reports.
    static QRect primary() { return QRect(0, 0, 1920, 1040); }
    static QRect leftScreen() { return QRect(-1920, 0, 1920, 1040); }
    static QRect topScreen() { return QRect(0, -1080, 1920, 1040); }
    static QList<QRect> desktop() { return { primary(), leftScreen(), topScreen() }; }

    static WindowPlacement::Saved saved(int x, int y, int w = 1280, int h = 760)
    {
        WindowPlacement::Saved s;
        s.x = x;
        s.y = y;
        s.width = w;
        s.height = h;
        return s;
    }

private slots:
    // The whole point of the item: a monitor left of the primary has negative
    // X and must be restored exactly, not centred.
    void restoresMonitorLeftOfPrimary()
    {
        const QRect rect =
            WindowPlacement::restore(saved(-1700, 120), desktop(), primary());
        QCOMPARE(rect, QRect(-1700, 120, 1280, 760));
    }

    // Same for a monitor mounted above the primary.
    void restoresMonitorAbovePrimary()
    {
        const QRect rect =
            WindowPlacement::restore(saved(300, -900), desktop(), primary());
        QCOMPARE(rect, QRect(300, -900, 1280, 760));
    }

    // The ordinary case still has to survive the new logic.
    void restoresPositiveMonitorUnchanged()
    {
        const QRect rect =
            WindowPlacement::restore(saved(320, 140), desktop(), primary());
        QCOMPARE(rect, QRect(320, 140, 1280, 760));
    }

    // Hanging off the right edge is a placement the user chose; as long as the
    // window is easy to grab it is left exactly where it was.
    void keepsReachablePartiallyVisibleWindow()
    {
        const QRect rect =
            WindowPlacement::restore(saved(1500, 900), desktop(), primary());
        QCOMPARE(rect, QRect(1500, 900, 1280, 760));
        QVERIFY(WindowPlacement::isReachable(rect, desktop()));
    }

    // A sliver is not a window the user can get back: 40 px of width is under
    // the reachable minimum even though the rectangle does intersect a screen.
    void recentresSliverThatCannotBeGrabbed()
    {
        const QRect rect =
            WindowPlacement::restore(saved(1880, 400), desktop(), primary());
        QVERIFY(!WindowPlacement::isReachable(QRect(1880, 400, 1280, 760), desktop()));
        QCOMPARE(rect, QRect(320, 140, 1280, 760));
    }

    // Nor is a window whose visible strip is tall but only two pixels wide —
    // the check is on the intersection's dimensions, not its area, so a
    // 2 x 760 strip (1520 px of window on screen) still fails.
    void rejectsTallTwoPixelStrip()
    {
        const QRect strip(1918, 100, 1280, 760);
        QCOMPARE(strip.intersected(primary()).size(), QSize(2, 760));
        QVERIFY(!WindowPlacement::isReachable(strip, desktop()));
    }

    // The monitor the window was on is gone: centre it on what is left, keeping
    // the size the user had.
    void recentresRectangleFromDisconnectedMonitor()
    {
        const QList<QRect> single = { primary() };
        const QRect rect =
            WindowPlacement::restore(saved(-1700, 120, 1400, 900), single, primary());
        QCOMPARE(rect, QRect(260, 70, 1400, 900));
    }

    // First run: nothing has ever been saved, so the default window opens in
    // the middle of the primary screen.
    void centresWhenPositionWasNeverSaved()
    {
        WindowPlacement::Saved never;
        const QRect rect = WindowPlacement::restore(never, desktop(), primary());
        QCOMPARE(rect, QRect(320, 140, 1280, 760));
    }

    // A missing or hand-edited size falls back to the default one, never below
    // the window's minimum.
    void repairsInvalidSize()
    {
        const QRect rect = WindowPlacement::restore(saved(200, 100, 0, -5), desktop(), primary());
        QCOMPARE(rect.size(), QSize(1280, 760));
        const QRect tiny = WindowPlacement::restore(saved(200, 100, 300, 200), desktop(), primary());
        QCOMPARE(tiny.size(), QSize(1024, 640));
    }

    // Recovery onto a screen smaller than the saved size: filling that screen
    // beats opening with edges nobody can reach.
    void clampsOversizedWindowOnRecovery()
    {
        const QRect small(0, 0, 1280, 720);
        const QRect rect =
            WindowPlacement::restore(saved(-9000, -9000, 2560, 1400), { small }, small);
        QCOMPARE(rect, QRect(0, 0, 1280, 720));
    }

    // A reachable window is never resized, even when it is bigger than the
    // screen it sits on.
    void keepsOversizedWindowThatIsStillReachable()
    {
        const QRect small(0, 0, 1280, 720);
        const QRect rect = WindowPlacement::restore(saved(0, 0, 2560, 1400), { small }, small);
        QCOMPARE(rect, QRect(0, 0, 2560, 1400));
    }

    // No screens at all (a headless session): produce a sane rectangle instead
    // of an invalid one.
    void survivesWithoutScreens()
    {
        const QRect rect = WindowPlacement::restore(saved(200, 100), {}, QRect());
        QCOMPARE(rect, QRect(0, 0, 1280, 760));
    }
};

QTEST_MAIN(TestWindowPlacement)
#include "tst_windowplacement.moc"
