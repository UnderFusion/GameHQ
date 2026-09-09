#include "storage/CaptureDatabase.h"
#include "ui/CaptureLibraryService.h"
#include "ui/GalleryModel.h"

#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

#include <memory>

// Saving a capture writes to two stores that fail independently: the media file
// the encoder produced, and the library row that makes it visible. The commit
// used to swallow a rejected insert, so the toast said "Screenshot saved" for a
// file no gallery would ever show. These tests pin the three answers the commit
// can honestly give, because App.cpp turns them straight into what the user
// reads.
//
// The insert failure is a real one — a SQLite trigger that aborts INSERT — not a
// mocked return value, so the outcome survives changes inside insertCapture.
class TestCaptureCommitOutcome : public QObject
{
    Q_OBJECT

private:
    struct Library
    {
        QTemporaryDir dir;
        std::unique_ptr<CaptureDatabase> db;
        std::unique_ptr<GalleryModel> model;
        QString rawName;

        ~Library()
        {
            if (!rawName.isEmpty())
                QSqlDatabase::removeDatabase(rawName);
        }

        bool open()
        {
            db = std::make_unique<CaptureDatabase>(dir.filePath(QStringLiteral("gamehq.db")));
            if (!db->open())
                return false;
            model = std::make_unique<GalleryModel>(db.get());
            return true;
        }

        QString writeFile(const QString& name)
        {
            const QString path = dir.filePath(name);
            QFile f(path);
            if (!f.open(QIODevice::WriteOnly))
                return {};
            f.write("x");
            return path;
        }

        // Every later insert into captures is refused, exactly as a corrupt or
        // read-only library would refuse it.
        bool refuseInserts()
        {
            rawName = QUuid::createUuid().toString();
            QSqlDatabase raw = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), rawName);
            raw.setDatabaseName(dir.filePath(QStringLiteral("gamehq.db")));
            if (!raw.open())
                return false;
            QSqlQuery q(raw);
            const bool ok = q.exec(QStringLiteral(
                "CREATE TRIGGER refuse_inserts BEFORE INSERT ON captures "
                "BEGIN SELECT RAISE(ABORT, 'library is read-only'); END"));
            raw.close();
            return ok;
        }
    };

private slots:
    // The ordinary path: file on disk, row in the library, nothing to explain.
    void clipCommitReportsIndexed()
    {
        Library lib;
        QVERIFY(lib.open());
        const QString clip = lib.writeFile(QStringLiteral("clip.mp4"));
        QVERIFY(!clip.isEmpty());

        CaptureLibraryService service(lib.db.get(), lib.model.get(), nullptr,
                                      CaptureFileOps::systemOps());
        QCOMPARE(service.commitClip(clip, QStringLiteral("Test Game"), QString()),
                 CaptureCommitOutcome::Indexed);
    }

    // The case the user reported as "the clip is gone": the export succeeded and
    // the library refused it. The file is real, so the answer must not be a
    // plain failure.
    void rejectedRowWithFileOnDiskIsMediaOnly()
    {
        Library lib;
        QVERIFY(lib.open());
        const QString clip = lib.writeFile(QStringLiteral("clip.mp4"));
        QVERIFY(!clip.isEmpty());
        QVERIFY(lib.refuseInserts());

        CaptureLibraryService service(lib.db.get(), lib.model.get(), nullptr,
                                      CaptureFileOps::systemOps());
        QCOMPARE(service.commitClip(clip, QStringLiteral("Test Game"), QString()),
                 CaptureCommitOutcome::MediaOnly);
        QVERIFY(QFile::exists(clip));
    }

    // Screenshots take the same route, so they get the same answer.
    void rejectedScreenshotRowIsMediaOnly()
    {
        Library lib;
        QVERIFY(lib.open());
        const QString shot = lib.writeFile(QStringLiteral("shot.png"));
        QVERIFY(!shot.isEmpty());
        QVERIFY(lib.refuseInserts());

        CaptureLibraryService service(lib.db.get(), lib.model.get(), nullptr,
                                      CaptureFileOps::systemOps());
        QCOMPARE(service.commitCapture(shot, QStringLiteral("screenshot"),
                                       QStringLiteral("Test Game")),
                 CaptureCommitOutcome::MediaOnly);
    }

    // Neither store has it: promising the user a file in their captures folder
    // would send them looking for nothing.
    void rejectedRowWithNoFileIsFailed()
    {
        Library lib;
        QVERIFY(lib.open());
        const QString missing = lib.dir.filePath(QStringLiteral("never-written.mp4"));
        QVERIFY(!QFile::exists(missing));
        QVERIFY(lib.refuseInserts());

        CaptureLibraryService service(lib.db.get(), lib.model.get(), nullptr,
                                      CaptureFileOps::systemOps());
        QCOMPARE(service.commitClip(missing, QStringLiteral("Test Game"), QString()),
                 CaptureCommitOutcome::Failed);
    }

    // insertCapture refuses duplicates, so a file the scanner indexed first
    // comes back as a rejected insert. It is in the library all the same, and
    // saying otherwise would be a false alarm.
    void alreadyIndexedFileIsNotReportedAsMissing()
    {
        Library lib;
        QVERIFY(lib.open());
        const QString clip = lib.writeFile(QStringLiteral("clip.mp4"));
        QVERIFY(!clip.isEmpty());

        CaptureLibraryService service(lib.db.get(), lib.model.get(), nullptr,
                                      CaptureFileOps::systemOps());
        QCOMPARE(service.commitClip(clip, QStringLiteral("Test Game"), QString()),
                 CaptureCommitOutcome::Indexed);
        QCOMPARE(service.commitClip(clip, QStringLiteral("Test Game"), QString()),
                 CaptureCommitOutcome::Indexed);
    }
};

QTEST_MAIN(TestCaptureCommitOutcome)
#include "tst_capturecommitoutcome.moc"
