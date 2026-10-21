#include "diagnostics/CrashDump.h"

#include <QTest>

// Release crashes left nothing to debug. The crash path names its dump without
// allocating, and retention must only ever delete those exact names - the logs
// folder also holds gamehq.log and its rotated generations.
class TestCrashDump : public QObject
{
    Q_OBJECT

private:
    static SYSTEMTIME time(WORD year, WORD month, WORD day, WORD hour, WORD minute, WORD second)
    {
        SYSTEMTIME t = {};
        t.wYear = year;
        t.wMonth = month;
        t.wDay = day;
        t.wHour = hour;
        t.wMinute = minute;
        t.wSecond = second;
        return t;
    }

private slots:
    void fileNameIsZeroPaddedAndRecognized()
    {
        wchar_t buffer[64];
        QVERIFY(CrashDump::formatFileName(buffer, 64, time(2026, 3, 7, 4, 5, 9), 42));
        const QString name = QString::fromWCharArray(buffer);
        QCOMPARE(name, QStringLiteral("gamehq-crash-20260307-040509-42.dmp"));
        QVERIFY(CrashDump::isDumpFileName(name));

        QVERIFY(CrashDump::formatFileName(buffer, 64, time(2026, 12, 31, 23, 59, 59),
                                          4294967295u));
        QVERIFY(CrashDump::isDumpFileName(QString::fromWCharArray(buffer)));
    }

    void fileNameRefusesASmallBuffer()
    {
        wchar_t buffer[20];
        QVERIFY(!CrashDump::formatFileName(buffer, 20, time(2026, 1, 1, 0, 0, 0), 1));
        QVERIFY(!CrashDump::formatFileName(nullptr, 0, time(2026, 1, 1, 0, 0, 0), 1));
    }

    void onlyExactDumpNamesMatch()
    {
        for (const char* other : { "gamehq.log", "gamehq.1.log", "gamehq-crash-.dmp",
                                   "gamehq-crash-2026030-040509-42.dmp",
                                   "gamehq-crash-20260307-040509-42.dmp.txt",
                                   "GAMEHQ-CRASH-20260307-040509-42.DMP",
                                   "my-gamehq-crash-20260307-040509-42.dmp",
                                   "gamehq-crash-20260307-040509-.dmp" }) {
            QVERIFY2(!CrashDump::isDumpFileName(QString::fromLatin1(other)), other);
        }
    }

    void retentionKeepsTheNewestAndIgnoresOtherFiles()
    {
        const QStringList names = {
            QStringLiteral("gamehq.log"),
            QStringLiteral("gamehq-crash-20260101-100000-7.dmp"),
            QStringLiteral("gamehq-crash-20260301-090000-5.dmp"),
            QStringLiteral("notes.txt"),
            QStringLiteral("gamehq-crash-20260301-090000-12.dmp"),
            QStringLiteral("gamehq-crash-20251231-235959-99999.dmp"),
        };
        // Same second: the numerically larger pid counts as newer.
        QCOMPARE(CrashDump::namesToPrune(names, 2),
                 QStringList({ QStringLiteral("gamehq-crash-20260101-100000-7.dmp"),
                               QStringLiteral("gamehq-crash-20251231-235959-99999.dmp") }));
        QCOMPARE(CrashDump::namesToPrune(names, 0).first(),
                 QStringLiteral("gamehq-crash-20260301-090000-12.dmp"));
        QVERIFY(CrashDump::namesToPrune(names, 4).isEmpty());
        QVERIFY(CrashDump::namesToPrune(names, 10).isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestCrashDump)
#include "tst_crashdump.moc"
