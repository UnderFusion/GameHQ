#include "capture/GdiReadback.h"

#include <QElapsedTimer>
#include <QTest>

#include <windows.h>

// t37/A07: the SDR screenshot readback. The source is an off-screen 32-bit DIB
// section with a known pattern, never the desktop, so the test reads exactly
// the pixels it wrote and opens no window.
namespace
{
QRgb expected(int x, int y)
{
    return qRgb((x * 7) & 0xff, (y * 13) & 0xff, (x + 3 * y) & 0xff);
}

int gdiObjects()
{
    return static_cast<int>(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS));
}

// A memory DC holding a w×h pattern; logical origin optionally moved so the
// top-left pixel sits at (originX, originY) like a monitor left of primary.
struct PatternSource
{
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;

    PatternSource(int w, int h, int originX = 0, int originY = 0)
    {
        BITMAPINFO info = {};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = w;
        info.bmiHeader.biHeight = -h;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        dc = CreateCompatibleDC(nullptr);
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dc || !bitmap)
            return;
        auto* px = static_cast<quint32*>(bits);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                px[y * w + x] = expected(x, y) & 0x00ffffff;   // BGRX, X = 0 like GDI
        previous = SelectObject(dc, bitmap);
        if (originX || originY)
            SetWindowOrgEx(dc, originX, originY, nullptr);
    }
    ~PatternSource()
    {
        if (dc && previous)
            SelectObject(dc, previous);
        if (bitmap)
            DeleteObject(bitmap);
        if (dc)
            DeleteDC(dc);
    }
    bool valid() const { return dc && bitmap && previous; }
};

bool matches(const QImage& img, int srcX, int srcY, QString* why)
{
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            const QRgb got = img.pixel(x, y);
            const QRgb want = expected(srcX + x, srcY + y);
            if (got != want) {
                *why = QStringLiteral("(%1,%2): got %3 want %4")
                           .arg(x).arg(y).arg(got, 8, 16, QLatin1Char('0'))
                           .arg(want, 8, 16, QLatin1Char('0'));
                return false;
            }
        }
    }
    return true;
}

// Failure injection: each stub fails one GDI call.
HDC WINAPI failCreateDC(HDC) { return nullptr; }
HBITMAP WINAPI failCreateBitmap(HDC, int, int) { return nullptr; }
HGDIOBJ WINAPI failSelect(HDC, HGDIOBJ) { return nullptr; }
BOOL WINAPI failBitBlt(HDC, int, int, int, int, HDC, int, int, DWORD) { return FALSE; }
int WINAPI failGetDIBits(HDC, HBITMAP, UINT, UINT, LPVOID, LPBITMAPINFO, UINT) { return 0; }
int WINAPI partialGetDIBits(HDC dc, HBITMAP bmp, UINT start, UINT lines, LPVOID bits,
                            LPBITMAPINFO info, UINT usage)
{
    return GetDIBits(dc, bmp, start, lines, bits, info, usage) - 1;
}

// Records whether the bitmap was still selected when GetDIBits ran.
int g_selectedDuringRead = -1;
int WINAPI checkingGetDIBits(HDC dc, HBITMAP bmp, UINT start, UINT lines, LPVOID bits,
                             LPBITMAPINFO info, UINT usage)
{
    g_selectedDuringRead = GetCurrentObject(dc, OBJ_BITMAP) == bmp ? 1 : 0;
    return GetDIBits(dc, bmp, start, lines, bits, info, usage);
}
} // namespace

class TestGdiReadback : public QObject
{
    Q_OBJECT

private slots:
    void aKnownPatternRoundTrips_data()
    {
        QTest::addColumn<int>("x");
        QTest::addColumn<int>("y");
        QTest::addColumn<int>("w");
        QTest::addColumn<int>("h");
        QTest::newRow("whole") << 0 << 0 << 64 << 48;
        QTest::newRow("odd sub-rect") << 5 << 7 << 37 << 23;
        QTest::newRow("one pixel") << 63 << 47 << 1 << 1;
        QTest::newRow("one row") << 0 << 20 << 64 << 1;
    }

    void aKnownPatternRoundTrips()
    {
        QFETCH(int, x);
        QFETCH(int, y);
        QFETCH(int, w);
        QFETCH(int, h);
        PatternSource source(64, 48);
        QVERIFY(source.valid());
        const QImage img = GdiReadback::readRect(source.dc, x, y, w, h);
        QVERIFY(!img.isNull());
        QCOMPARE(img.size(), QSize(w, h));
        QCOMPARE(img.format(), QImage::Format_RGB32);
        QString why;
        QVERIFY2(matches(img, x, y, &why), qPrintable(why));
        QCOMPARE(qAlpha(img.pixel(0, 0)), 255);
        // Channel order: red, green and blue land in their own channels.
        QCOMPARE(qRed(img.pixel(0, 0)), (x * 7) & 0xff);
        QCOMPARE(qGreen(img.pixel(0, 0)), (y * 13) & 0xff);
        QCOMPARE(qBlue(img.pixel(0, 0)), (x + 3 * y) & 0xff);
    }

    void negativeMonitorCoordinatesPassThrough()
    {
        // A monitor left of and above the primary one: virtual-screen origin
        // (-1920, -300). The same pixels must come back at those coordinates.
        PatternSource source(64, 48, -1920, -300);
        QVERIFY(source.valid());
        const QImage img = GdiReadback::readRect(source.dc, -1920 + 5, -300 + 7, 37, 23);
        QVERIFY(!img.isNull());
        QCOMPARE(img.size(), QSize(37, 23));
        QString why;
        QVERIFY2(matches(img, 5, 7, &why), qPrintable(why));
    }

    void theBitmapIsDeselectedBeforeItIsRead()
    {
        PatternSource source(16, 16);
        QVERIFY(source.valid());
        GdiReadback::Api api;
        api.getDIBits = &checkingGetDIBits;
        g_selectedDuringRead = -1;
        QVERIFY(!GdiReadback::readRect(source.dc, 0, 0, 16, 16, api).isNull());
        QCOMPARE(g_selectedDuringRead, 0);
    }

    void theImageOwnsItsPixelsAfterGdiIsGone()
    {
        QImage img;
        {
            PatternSource source(32, 32);
            QVERIFY(source.valid());
            img = GdiReadback::readRect(source.dc, 0, 0, 32, 32);
        }
        QVERIFY(!img.isNull());
        QVERIFY(img.isDetached());   // sole owner, no shared or foreign buffer
        QString why;
        QVERIFY2(matches(img, 0, 0, &why), qPrintable(why));
        img.setPixel(0, 0, qRgb(1, 2, 3));   // writable without a copy-on-write
        QCOMPARE(img.pixel(0, 0), qRgb(1, 2, 3));
    }

    void everyFailureReturnsNullAndReleasesHandles_data()
    {
        QTest::addColumn<int>("which");
        QTest::newRow("CreateCompatibleDC") << 0;
        QTest::newRow("CreateCompatibleBitmap") << 1;
        QTest::newRow("SelectObject") << 2;
        QTest::newRow("BitBlt") << 3;
        QTest::newRow("GetDIBits") << 4;
        QTest::newRow("GetDIBits partial") << 5;
    }

    void everyFailureReturnsNullAndReleasesHandles()
    {
        QFETCH(int, which);
        PatternSource source(16, 16);
        QVERIFY(source.valid());
        GdiReadback::Api api;
        switch (which) {
        case 0: api.createCompatibleDC = &failCreateDC; break;
        case 1: api.createCompatibleBitmap = &failCreateBitmap; break;
        case 2: api.selectObject = &failSelect; break;
        case 3: api.bitBlt = &failBitBlt; break;
        case 4: api.getDIBits = &failGetDIBits; break;
        case 5: api.getDIBits = &partialGetDIBits; break;
        }
        const int before = gdiObjects();
        for (int i = 0; i < 50; ++i)
            QVERIFY(GdiReadback::readRect(source.dc, 0, 0, 16, 16, api).isNull());
        QCOMPARE(gdiObjects(), before);
    }

    void invalidRequestsAreRefused()
    {
        PatternSource source(16, 16);
        QVERIFY(source.valid());
        const int before = gdiObjects();
        QVERIFY(GdiReadback::readRect(nullptr, 0, 0, 16, 16).isNull());
        QVERIFY(GdiReadback::readRect(source.dc, 0, 0, 0, 16).isNull());
        QVERIFY(GdiReadback::readRect(source.dc, 0, 0, 16, -1).isNull());
        // Too large to allocate: refused before any GDI object exists.
        QVERIFY(GdiReadback::readRect(source.dc, 0, 0, 200000, 200000).isNull());
        QCOMPARE(gdiObjects(), before);
    }

    void repeatedCapturesDoNotGrowGdiHandles()
    {
        PatternSource source(320, 200);
        QVERIFY(source.valid());
        const int before = gdiObjects();
        for (int i = 0; i < 1000; ++i)
            QVERIFY(!GdiReadback::readRect(source.dc, i % 50, i % 30, 200, 150).isNull());
        const int after = gdiObjects();
        qInfo() << "GDI objects before/after 1000 readbacks:" << before << after;
        QCOMPARE(after, before);
    }

    // The removed QImage::copy() held a second full frame while copying it.
    void fourKReadbackCost()
    {
        PatternSource source(3840, 2160);
        QVERIFY(source.valid());
        constexpr int kRuns = 10;
        QElapsedTimer t;
        t.start();
        QImage img;
        for (int i = 0; i < kRuns; ++i)
            img = GdiReadback::readRect(source.dc, 0, 0, 3840, 2160);
        const double readMs = double(t.nsecsElapsed()) / 1e6 / kRuns;
        QVERIFY(!img.isNull());
        t.restart();
        qint64 sink = 0;
        for (int i = 0; i < kRuns; ++i)
            sink += img.copy().constBits()[i];
        const double copyMs = double(t.nsecsElapsed()) / 1e6 / kRuns;
        Q_UNUSED(sink);
        qInfo().nospace() << "4K readback " << readMs << " ms; the removed copy cost "
                          << copyMs << " ms and " << img.sizeInBytes() / (1024 * 1024)
                          << " MiB more peak memory per screenshot";
        QString why;
        QVERIFY2(matches(img.copy(0, 0, 64, 64), 0, 0, &why), qPrintable(why));
    }
};

QTEST_GUILESS_MAIN(TestGdiReadback)
#include "tst_gdireadback.moc"
