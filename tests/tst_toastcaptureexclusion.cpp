#include "notify/NotificationCenter.h"

#include <QOperatingSystemVersion>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QtTest>

#include <windows.h>

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

// Toasts must stay on screen for the player but out of GameHQ screenshots.
// This judges real captured pixels with the same screen-DC BitBlt
// (SRCCOPY | CAPTUREBLT) that ScreenshotService::grabRect uses for SDR games.
// The toast window never takes focus, so the test leaves the foreground alone.
class TestToastCaptureExclusion : public QObject
{
    Q_OBJECT

    // Share of pixels in the toast's screen rectangle that are the stub colour.
    static double toastShare(HWND hwnd)
    {
        RECT r{};
        if (!GetWindowRect(hwnd, &r)) return -1;
        const int w = r.right - r.left, h = r.bottom - r.top;
        HDC screen = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(screen);
        HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
        HGDIOBJ old = SelectObject(mem, bmp);
        QImage img(w, h, QImage::Format_RGB32);
        BITMAPINFO bi{};
        bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB };
        const bool ok = BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY | CAPTUREBLT)
            && GetDIBits(mem, bmp, 0, h, img.bits(), &bi, DIB_RGB_COLORS);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        if (!ok) return -1;
        qint64 hits = 0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const QRgb p = img.pixel(x, y);
                hits += qRed(p) > 235 && qGreen(p) < 20 && qBlue(p) > 235;
            }
        return double(hits) / (qint64(w) * h);
    }
    static double settledShare(HWND hwnd)
    {
        QTest::qWait(250);   // let DWM compose the latest change first
        return toastShare(hwnd);
    }
    static HWND hwndOf(QQuickWindow* w) { return reinterpret_cast<HWND>(w->winId()); }
    static DWORD affinityOf(HWND hwnd)
    {
        DWORD a = WDA_NONE;
        GetWindowDisplayAffinity(hwnd, &a);
        return a;
    }

private slots:
    void toastIsVisibleButNeverCaptured()
    {
        QQmlApplicationEngine engine;
        engine.addImportPath(QStringLiteral(":/qt/qml"));
        NotificationCenter notifications(&engine);
        notifications.setVisibleLimit(4);   // the real ToastWindow binds Theme's limit
        const HWND foreground = GetForegroundWindow();

        notifications.post(1, QStringLiteral("Capture request received"));
        QQuickWindow* toast = notifications.toastWindow();
        QVERIFY(toast);
        QVERIFY(QTest::qWaitForWindowExposed(toast));
        HWND hwnd = hwndOf(toast);
        if (QOperatingSystemVersion::current() < QOperatingSystemVersion::Windows10_2004)
            QSKIP("WDA_EXCLUDEFROMCAPTURE needs Windows 10 2004+");
        QCOMPARE(affinityOf(hwnd), DWORD(WDA_EXCLUDEFROMCAPTURE));

        // Control first: with the exclusion lifted the stub must be in the
        // grab, otherwise this desktop cannot be judged (locked, exclusive
        // fullscreen covering the corner) and a "clean" capture proves nothing.
        QVERIFY(SetWindowDisplayAffinity(hwnd, WDA_NONE));
        const double visible = settledShare(hwnd);
        if (visible < 0.9)
            QSKIP(qPrintable(QStringLiteral("toast area not capturable here (share %1)").arg(visible)));
        QVERIFY(SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE));

        // Normal and consecutive screenshots, the toast still on screen.
        for (int shot = 0; shot < 3; ++shot) {
            QVERIFY(toast->isVisible());
            QCOMPARE(settledShare(hwnd), 0.0);
        }

        // Pointer over the stack drops click-through and back again; the
        // exclusion has to survive both flag changes, whether Qt restyles the
        // HWND in place or replaces it (then the SurfaceCreated hook reapplies).
        for (const bool inside : { true, false }) {
            toast->setFlag(Qt::WindowTransparentForInput, !inside);
            hwnd = hwndOf(toast);
            QCOMPARE(affinityOf(hwnd), DWORD(WDA_EXCLUDEFROMCAPTURE));
            QCOMPARE(settledShare(hwnd), 0.0);
        }

        // A destroyed and recreated native window is excluded before it shows.
        notifications.hideWindow();
        toast->destroy();
        notifications.post(2, QStringLiteral("Clip saved"));
        QVERIFY(QTest::qWaitForWindowExposed(toast));
        hwnd = hwndOf(toast);
        QCOMPARE(affinityOf(hwnd), DWORD(WDA_EXCLUDEFROMCAPTURE));
        QCOMPARE(settledShare(hwnd), 0.0);

        // Still on screen for the player: lifting the exclusion shows it again.
        QVERIFY(SetWindowDisplayAffinity(hwnd, WDA_NONE));
        QVERIFY(settledShare(hwnd) >= 0.9);

        notifications.hideWindow();
        QCOMPARE(GetForegroundWindow(), foreground);   // never took focus
    }
};

QTEST_MAIN(TestToastCaptureExclusion)
#include "tst_toastcaptureexclusion.moc"
