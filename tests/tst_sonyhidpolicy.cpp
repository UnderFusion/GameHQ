#include "input/SonyHidReaderPolicy.h"
#include <QtTest>
class TestSonyHidPolicy : public QObject {
    Q_OBJECT
private slots:
    void compiledDefault() {
        QCOMPARE(SonyHidReaderPolicy::resolve({}).enabled,
                 GAMEHQ_EXPECTED_HID_DEFAULT != 0);
    }
    void overrides() {
        QVERIFY(!SonyHidReaderPolicy::resolve("0").enabled);
        QVERIFY(SonyHidReaderPolicy::resolve("1").enabled);
        QCOMPARE(SonyHidReaderPolicy::resolve("invalid").enabled,
                 GAMEHQ_EXPECTED_HID_DEFAULT != 0);
    }
};
QTEST_GUILESS_MAIN(TestSonyHidPolicy)
#include "tst_sonyhidpolicy.moc"
