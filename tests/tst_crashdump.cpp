#include "diagnostics/CrashDump.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

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

    // Runs the fixture until it dies; returns how long that took.
    static qint64 crashFixture(const QString& logsDir, const char* mode)
    {
        QProcess child;
        QElapsedTimer timer;
        timer.start();
        child.start(QStringLiteral(CRASHDUMP_FIXTURE), {logsDir, QString::fromLatin1(mode)});
        if (!child.waitForStarted(10000) || !child.waitForFinished(60000)) {
            child.kill();
            child.waitForFinished();
            return -1;
        }
        return timer.elapsed();
    }

    // The exception code recorded in a minidump's exception stream, or 0.
    // Layout: MINIDUMP_HEADER {'MDMP', version, streams, directory rva, ...};
    // directory entries {type, size, rva}; MINIDUMP_EXCEPTION_STREAM starts
    // {thread id, alignment, MINIDUMP_EXCEPTION {code, ...}}.
    static quint32 dumpExceptionCode(const QByteArray& dump)
    {
        auto u32 = [&dump](qsizetype at) -> quint32 {
            if (at < 0 || at + 4 > dump.size())
                return 0;
            return qFromLittleEndian<quint32>(dump.constData() + at);
        };
        if (!dump.startsWith("MDMP"))
            return 0;
        const quint32 streams = u32(8);
        const quint32 directory = u32(12);
        for (quint32 i = 0; i < streams; ++i) {
            const qsizetype entry = qsizetype(directory) + qsizetype(i) * 12;
            if (u32(entry) == 6 /* ExceptionStream */)
                return u32(qsizetype(u32(entry + 8)) + 8);
        }
        return 0;
    }

    static QByteArray onlyDump(const QString& logsDir)
    {
        QStringList dumps;
        for (const QString& name : QDir(logsDir).entryList(QDir::Files))
            if (CrashDump::isDumpFileName(name))
                dumps << name;
        if (dumps.size() != 1)
            return {};
        QFile f(QDir(logsDir).filePath(dumps.first()));
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
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
    // A real crash in a child process leaves exactly one readable dump whose
    // exception stream names the access violation.
    void accessViolationWritesAReadableDump()
    {
        QTemporaryDir logs;
        QVERIFY(logs.isValid());
        QVERIFY(crashFixture(logs.path(), "av") >= 0);
        const QByteArray dump = onlyDump(logs.path());
        QVERIFY2(!dump.isEmpty(), "no dump was written");
        QVERIFY2(dump.size() < 16 * 1024 * 1024, QByteArray::number(dump.size()).constData());
        QCOMPARE(dumpExceptionCode(dump), quint32(0xC0000005));
    }

    // An uncaught C++ exception never reaches the exception filter; the
    // terminate handler still writes a dump, tagged with its own code.
    void uncaughtExceptionWritesADump()
    {
        QTemporaryDir logs;
        QVERIFY(logs.isValid());
        QVERIFY(crashFixture(logs.path(), "terminate") >= 0);
        QCOMPARE(dumpExceptionCode(onlyDump(logs.path())), quint32(0xE0000001));
    }

    // A logs "folder" that cannot hold files must not turn a crash into a hang.
    // (Fail-fast terminations - RaiseFailFastException, __fastfail - bypass
    // every handler and are deliberately not covered; nothing is written.)
    void unwritableLogsFolderStillLetsTheProcessDie()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString notAFolder = root.filePath(QStringLiteral("logs"));
        QFile blocker(notAFolder);
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        const qint64 elapsed = crashFixture(notAFolder, "av");
        QVERIFY(elapsed >= 0);
        QVERIFY2(elapsed < 20000, QByteArray::number(elapsed).constData());
    }
};

QTEST_GUILESS_MAIN(TestCrashDump)
#include "tst_crashdump.moc"
