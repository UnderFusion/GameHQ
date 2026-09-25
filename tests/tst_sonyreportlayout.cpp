#include <QtTest>

#include "input/Gamepad.h"
#include "input/SonyReportLayout.h"
#include "input/StickNav.h"

namespace {
using Family = SonyReportLayout::Family;
constexpr StickNav::AxisConfig kNav{128, 60, 30, false};
constexpr quint32 bit(int b) { return quint32(1) << b; }
}

class SonyReportLayoutTest : public QObject
{
    Q_OBJECT

private slots:
    void resolvesEveryTransportLayout()
    {
        auto check = [](unsigned char id, Family f, int len, int axes, int buttons) {
            const auto o = SonyReportLayout::locate(id, f, len);
            QCOMPARE(o.axes, axes);
            QCOMPARE(o.buttons, buttons);
        };
        check(0x01, Family::Ds4, 64, 1, 5);
        check(0x11, Family::Ds4, 78, 3, 7);
        check(0x01, Family::DualSense, 64, 1, 8);   // USB
        check(0x01, Family::DualSense, 10, 1, 5);   // BT simple
        check(0x01, Family::DualSense, 78, 1, 5);   // BT simple, padded by Raw Input
        check(0x31, Family::DualSense, 78, 2, 9);   // BT full
        QVERIFY(!SonyReportLayout::locate(0x31, Family::Ds4, 78).valid());
        QVERIFY(!SonyReportLayout::locate(0x11, Family::DualSense, 78).valid());
    }

    void bluetoothDualSenseStickUpIsUpNotLeft()
    {
        // Report 0x31: [0]=id [1]=seq tag [2]=LX [3]=LY [4]=RX [5]=RY ...
        // Reading LX at byte 3 turned stick up into Left (issue from a
        // Bluetooth DualSense reporter).
        unsigned char r[78] = {};
        r[0] = 0x31; r[1] = 0x10;
        r[2] = 0x80; r[3] = 0x00; r[4] = 0x80; r[5] = 0x80;
        r[9] = 0x08; // hat neutral, no face buttons
        const auto o = SonyReportLayout::locate(r[0], Family::DualSense, int(sizeof r));
        const quint32 nav = StickNav::bits(kNav, r[o.axes], r[o.axes + 1]);
        QVERIFY(nav & bit(Gamepad::DpadUp));
        QVERIFY(!(nav & bit(Gamepad::DpadLeft)));
        QCOMPARE(r[o.buttons] & 0x0F, 0x08);
    }

    void bluetoothDualSenseSimpleReportReadsButtonsAtByteFive()
    {
        // Simple report padded to 78 bytes: buttons at 5, L2/R2 analog at 8/9.
        unsigned char r[78] = {};
        r[0] = 0x01;
        r[1] = 0x80; r[2] = 0x80; r[3] = 0x80; r[4] = 0x80;
        r[5] = 0x28; // hat neutral + Cross
        r[8] = 0x00; // L2 analog released (would read as hat 0 = Up at byte 8)
        const auto o = SonyReportLayout::locate(r[0], Family::DualSense, int(sizeof r));
        QCOMPARE(o.buttons, 5);
        QCOMPARE(r[o.buttons] & 0x0F, 0x08);
        QVERIFY(r[o.buttons] & 0x20);
        QCOMPARE(StickNav::bits(kNav, r[o.axes], r[o.axes + 1]), quint32(0));
    }

    void bluetoothDs4NeutralStickDoesNotBecomeLeft()
    {
        // Report 0x11: bytes 3/4 are LX/LY. Byte 1 is transport metadata and
        // may be zero; treating it as LX was the source of permanent Left.
        const unsigned char report[] = {0x11, 0x00, 0x00, 0x80, 0x80, 0x00, 0x00, 0x08};
        const int axisBase = SonyReportLayout::locate(0x11, Family::Ds4, 78).axes;
        QCOMPARE(StickNav::bits(kNav, report[axisBase], report[axisBase + 1]), quint32(0));
        QVERIFY(StickNav::bits(kNav, report[1], report[2]) & bit(Gamepad::DpadLeft));
    }
};

QTEST_APPLESS_MAIN(SonyReportLayoutTest)
#include "tst_sonyreportlayout.moc"
