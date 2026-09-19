// cpo-c01 / cpo-c02: one physical controller, three APIs, one canonical
// control per button.
//
// The reported defect: a wired DualSense stays detected, but pressing *some*
// buttons made the configured action stop matching. The cause was that each
// legacy backend normalized the same physical button to a different canonical
// control -- the Sony HID and XInput views published gamepad.trigger_left for
// L2 while the WinMM view published the unnamed gamepad.button.0, and
// gamepad.button.0 simultaneously meant "left stick click" over XInput. A
// binding therefore resolved or failed depending on which API happened to
// deliver that particular edge.
//
// This fixture drives the three pure normalizers directly, so it fails on a
// mapping-table regression without needing a physical pad.

#include "input/DualSenseDevice.h"
#include "input/Gamepad.h"
#include "input/WinMMDevice.h"
#include "input/XInputDevice.h"

#include <windows.h>
#include <xinput.h>

#include <QSet>
#include <QtTest>

namespace {

// Canonical controls a raw backend button mask resolves to.
QSet<QString> canonicalControls(quint32 mask)
{
    QSet<QString> controls;
    for (int b = 0; b < Gamepad::MaxButtons; ++b) {
        if (!(mask & (1u << b)))
            continue;
        const QString id = Gamepad::controlIdFor(b);
        if (!id.isEmpty())
            controls.insert(id);
    }
    return controls;
}

QSet<QString> fromWinMMSony(quint32 rawButtons)
{
    return canonicalControls(WinMMDevice::mapDigitalButtons(rawButtons, true));
}

QSet<QString> fromWinMMXbox(quint32 rawButtons)
{
    return canonicalControls(WinMMDevice::mapDigitalButtons(rawButtons, false));
}

QSet<QString> fromXInput(quint16 buttons, quint8 lt = 0, quint8 rt = 0)
{
    return canonicalControls(XInputDevice::mapDigitalButtons(buttons, lt, rt));
}

// Minimal USB DualSense input report: report id 0x01, button block at base 8.
QSet<QString> fromSonyHid(quint8 b0, quint8 b1, quint8 b2)
{
    unsigned char report[16] = {};
    report[0] = 0x01;
    report[8] = static_cast<unsigned char>(b0 | 0x08);  // hat 8 == neutral
    report[9] = b1;
    report[10] = b2;
    return canonicalControls(DualSenseDevice::decodeButtons(report, 8));
}

} // namespace

class TestProviderControlMap : public QObject
{
    Q_OBJECT

private slots:
    // The regression the reporter hit: L2/R2 over WinMM used to be
    // gamepad.button.0/1 while every other backend called them
    // gamepad.trigger_left/right.
    void triggersAgreeAcrossProviders()
    {
        const QSet<QString> expected{QStringLiteral("gamepad.trigger_left")};

        QCOMPARE(fromWinMMSony(0x0040), expected);                     // DI button 6
        QCOMPARE(fromXInput(0, XINPUT_GAMEPAD_TRIGGER_THRESHOLD + 1), expected);
        QCOMPARE(fromSonyHid(0x00, 0x04, 0x00), expected);             // b1 bit 2

        const QSet<QString> right{QStringLiteral("gamepad.trigger_right")};
        QCOMPARE(fromWinMMSony(0x0080), right);                        // DI button 7
        QCOMPARE(fromXInput(0, 0, XINPUT_GAMEPAD_TRIGGER_THRESHOLD + 1), right);
        QCOMPARE(fromSonyHid(0x00, 0x08, 0x00), right);                // b1 bit 3
    }

    // The collision that made gamepad.button.0 ambiguous: it meant L2 over
    // WinMM and left stick click over XInput, for the very same pad.
    void thumbClicksAgreeAndAreNamed()
    {
        const QSet<QString> left{QStringLiteral("gamepad.thumb_left")};
        QCOMPARE(fromWinMMSony(0x0400), left);                         // DI button 10
        QCOMPARE(fromWinMMXbox(0x0100), left);                         // DI button 8
        QCOMPARE(fromXInput(XINPUT_GAMEPAD_LEFT_THUMB), left);

        const QSet<QString> right{QStringLiteral("gamepad.thumb_right")};
        QCOMPARE(fromWinMMSony(0x0800), right);                        // DI button 11
        QCOMPARE(fromWinMMXbox(0x0200), right);                        // DI button 9
        QCOMPARE(fromXInput(XINPUT_GAMEPAD_RIGHT_THUMB), right);
    }

    // Face buttons and shoulders always agreed; assert it so a future layout
    // edit cannot silently break the part that already worked.
    void faceAndShoulderButtonsAgreeAcrossProviders()
    {
        const QSet<QString> south{QStringLiteral("gamepad.face_south")};
        QCOMPARE(fromWinMMSony(0x0002), south);                        // Cross
        QCOMPARE(fromWinMMXbox(0x0001), south);                        // A
        QCOMPARE(fromXInput(XINPUT_GAMEPAD_A), south);
        QCOMPARE(fromSonyHid(0x20, 0x00, 0x00), south);

        const QSet<QString> west{QStringLiteral("gamepad.face_west")};
        QCOMPARE(fromWinMMSony(0x0001), west);                         // Square
        QCOMPARE(fromWinMMXbox(0x0004), west);                         // X
        QCOMPARE(fromXInput(XINPUT_GAMEPAD_X), west);
        QCOMPARE(fromSonyHid(0x10, 0x00, 0x00), west);

        const QSet<QString> shoulder{QStringLiteral("gamepad.shoulder_left")};
        QCOMPARE(fromWinMMSony(0x0010), shoulder);
        QCOMPARE(fromWinMMXbox(0x0010), shoulder);
        QCOMPARE(fromXInput(XINPUT_GAMEPAD_LEFT_SHOULDER), shoulder);
        QCOMPARE(fromSonyHid(0x00, 0x01, 0x00), shoulder);
    }

    // Share/Create and Guide are capability-routed, but their canonical
    // identity must still be provider-independent on a Sony pad.
    void systemButtonsKeepOneCanonicalIdentity()
    {
        const QSet<QString> capture{QStringLiteral("gamepad.capture")};
        QCOMPARE(fromWinMMSony(0x0100), capture);                      // DI button 8
        QCOMPARE(fromSonyHid(0x00, 0x10, 0x00), capture);

        const QSet<QString> guide{QStringLiteral("gamepad.guide")};
        QCOMPARE(fromWinMMSony(0x1000), guide);                        // DI button 12
        QCOMPARE(fromSonyHid(0x00, 0x00, 0x01), guide);
    }

    // No canonical control may be reachable through a generic index any more:
    // a generic code is index-scoped, so sharing one across providers is what
    // made a single binding mean two different physical buttons.
    void noNamedPositionIsPublishedAsAGenericButton()
    {
        const quint32 masks[] = {
            WinMMDevice::mapDigitalButtons(0x3FFF, true),
            WinMMDevice::mapDigitalButtons(0x03FF, false),
            XInputDevice::mapDigitalButtons(0xFFFF, 255, 255),
        };
        for (quint32 mask : masks) {
            for (int b = Gamepad::GenericButtonBase; b < Gamepad::MaxButtons; ++b) {
                if (!(mask & (1u << b)))
                    continue;
                // Only the DualSense touchpad click survives as generic: no
                // other backend exposes it, so it cannot collide.
                QCOMPARE(b, Gamepad::GenericButtonBase + 4);
            }
        }
    }

    // The touchpad click keeps its historic generic index so an existing
    // binding to it is not silently orphaned by this correction.
    void touchpadClickKeepsItsHistoricGenericIndex()
    {
        const QSet<QString> touchpad = fromWinMMSony(0x2000);
        QCOMPARE(touchpad.size(), 1);
        QVERIFY(touchpad.contains(QStringLiteral("gamepad.button.4")));
    }
};

QTEST_APPLESS_MAIN(TestProviderControlMap)
#include "tst_providercontrolmap.moc"
