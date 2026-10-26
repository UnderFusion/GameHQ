#include "diagnostics/Logger.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

// gamehq.log was appended to forever: no size cap, no rotation, no retention.
// A long-lived install would hand the user a multi-gigabyte file, and a log
// directory that could not be opened silenced the whole session.
class TestLogRotation : public QObject
{
    Q_OBJECT

private:
    static bool write(const QString& path, qint64 bytes)
    {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            return false;
        return file.write(QByteArray(int(bytes), 'x')) == bytes;
    }

    static QString generation(const QString& dir, int index)
    {
        return dir + QStringLiteral("/gamehq.%1.log").arg(index);
    }

private slots:
    // Runs first: buffering only applies before any install().
    void messagesBeforeInstallReachTheLogFile()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Logger::beginBuffering();
        qWarning() << "logged before the log directory was known";

        const QString logs = dir.filePath(QStringLiteral("logs"));
        Logger::install(logs);
        QVERIFY(Logger::writingToFile());
        qInfo() << "logged after install";

        QFile file(Logger::filePath(logs));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray contents = file.readAll();
        file.close();
        const qsizetype early = contents.indexOf("logged before the log directory was known");
        QVERIFY(early >= 0);
        QVERIFY(contents.indexOf("logged after install") > early);

        // Release the file so the temporary directory can be removed.
        const QString blocked = dir.filePath(QStringLiteral("not-a-directory"));
        QVERIFY(write(blocked, 4));
        Logger::install(blocked);
    }

    void aLogUnderTheLimitIsLeftAlone()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString current = dir.filePath(QStringLiteral("gamehq.log"));
        QVERIFY(write(current, 512));

        QVERIFY(!Logger::rotateIfNeeded(dir.path(), QStringLiteral("gamehq.log"), 1024, 3));
        QCOMPARE(QFileInfo(current).size(), qint64(512));
        QVERIFY(!QFileInfo::exists(generation(dir.path(), 1)));
    }

    void aMissingLogIsNotAnError()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(!Logger::rotateIfNeeded(dir.path(), QStringLiteral("gamehq.log"), 1024, 3));
    }

    void reachingTheLimitShiftsEveryGenerationAndDropsTheOldest()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString current = dir.filePath(QStringLiteral("gamehq.log"));
        QVERIFY(write(current, 1024));
        QVERIFY(write(generation(dir.path(), 1), 11));
        QVERIFY(write(generation(dir.path(), 2), 22));
        QVERIFY(write(generation(dir.path(), 3), 33));

        QVERIFY(Logger::rotateIfNeeded(dir.path(), QStringLiteral("gamehq.log"), 1024, 3));

        // The current log became generation 1, the rest moved up one, and the
        // fourth-oldest is gone rather than accumulating forever.
        QVERIFY(!QFileInfo::exists(current));
        QCOMPARE(QFileInfo(generation(dir.path(), 1)).size(), qint64(1024));
        QCOMPARE(QFileInfo(generation(dir.path(), 2)).size(), qint64(11));
        QCOMPARE(QFileInfo(generation(dir.path(), 3)).size(), qint64(22));
        QVERIFY(!QFileInfo::exists(generation(dir.path(), 4)));
    }

    void rotationRepeatsWithoutGrowingTheNumberOfFiles()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        for (int round = 0; round < 6; ++round) {
            QVERIFY(write(dir.filePath(QStringLiteral("gamehq.log")), 1024));
            QVERIFY(Logger::rotateIfNeeded(dir.path(), QStringLiteral("gamehq.log"), 1024, 3));
        }
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files).size(), 3);
    }

    void anUnmovableGenerationLeavesTheLogIntact()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString current = dir.filePath(QStringLiteral("gamehq.log"));
        QVERIFY(write(current, 1024));
        QVERIFY(write(generation(dir.path(), 1), 11));

        // Something still holds the oldest generation open, so it can be
        // neither removed nor overwritten.
        QFile locked(generation(dir.path(), 2));
        QVERIFY(locked.open(QIODevice::WriteOnly));
        QVERIFY(locked.write("locked") > 0);
        // Keep the handle open across the call, with no sharing.
        const bool rotated = Logger::rotateIfNeeded(dir.path(), QStringLiteral("gamehq.log"),
                                                    1024, 2);
        locked.close();

        if (rotated)
            QSKIP("This filesystem allows renaming over an open file, so the failure path is unreachable.");
        // Nothing was lost: the live log and both generations are still there.
        QCOMPARE(QFileInfo(current).size(), qint64(1024));
        QCOMPARE(QFileInfo(generation(dir.path(), 1)).size(), qint64(11));
        QVERIFY(QFileInfo::exists(generation(dir.path(), 2)));
    }

    void aLogWithoutASuffixStillRotates()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(write(dir.filePath(QStringLiteral("gamehq")), 64));
        QVERIFY(Logger::rotateIfNeeded(dir.path(), QStringLiteral("gamehq"), 64, 2));
        QVERIFY(QFileInfo::exists(dir.filePath(QStringLiteral("gamehq.1"))));
    }

    void installSurvivesALogDirectoryItCannotUse()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        // A plain file where the log directory should be: mkpath and open both
        // fail, and the app must still run.
        const QString blocked = dir.filePath(QStringLiteral("not-a-directory"));
        QVERIFY(write(blocked, 4));

        Logger::install(blocked);
        QVERIFY(!Logger::writingToFile());
        qInfo() << "this message has nowhere to go but stderr";

        // And a usable directory afterwards is picked up normally.
        const QString good = dir.filePath(QStringLiteral("logs"));
        Logger::install(good);
        QVERIFY(Logger::writingToFile());
        qInfo() << "recorded";
        QVERIFY(QFileInfo(good + QStringLiteral("/gamehq.log")).size() > 0);

        // Re-pointing the logger releases the file it held, which is also what
        // lets this temporary directory be removed.
        Logger::install(blocked);
        QVERIFY(!Logger::writingToFile());
    }
};

QTEST_GUILESS_MAIN(TestLogRotation)
#include "tst_logrotation.moc"
