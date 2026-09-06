#include "storage/CaptureDatabase.h"
#include "ui/CaptureLibraryService.h"
#include "ui/GalleryModel.h"

#include <QFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <QVariant>

#include <memory>

// Deleting a capture writes to two stores that can fail independently: the
// media file on disk and the library row. The old code removed the row even
// when the file could not be deleted, so a locked file made the capture vanish
// from the library while still filling the drive, and bulk deletion always
// reported success. These tests pin the recoverable behaviour instead.
//
// Filesystem failures are injected through CaptureFileOps rather than by
// locking real files, so the outcome does not depend on Windows lock timing.
// The database failure is a real one: a SQLite trigger that aborts the
// tombstone UPDATE.
class TestCaptureLibraryDeletion : public QObject
{
    Q_OBJECT

private:
    struct RawConnection
    {
        QString name;
        QSqlDatabase db;

        explicit RawConnection(const QString& path)
            : name(QUuid::createUuid().toString())
        {
            db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
            db.setDatabaseName(path);
            db.open();
        }
        ~RawConnection()
        {
            db = QSqlDatabase();
            QSqlDatabase::removeDatabase(name);
        }
    };

    // One library with real files on disk, its database, and the model the
    // service deletes through.
    struct Library
    {
        QTemporaryDir dir;
        std::unique_ptr<CaptureDatabase> db;
        std::unique_ptr<GalleryModel> model;

        QString dbPath() const { return dir.filePath(QStringLiteral("gamehq.db")); }

        bool open()
        {
            db = std::make_unique<CaptureDatabase>(dbPath());
            if (!db->open())
                return false;
            model = std::make_unique<GalleryModel>(db.get());
            return true;
        }

        QString writeFile(const QString& name) const
        {
            const QString path = dir.filePath(name);
            QFile f(path);
            if (!f.open(QIODevice::WriteOnly))
                return {};
            f.write("x");
            return path;
        }

        // Returns the capture id, or -1.
        int addCapture(const QString& mediaName, const QString& thumbName, QString* mediaPath,
                       QString* thumbPath)
        {
            const QString media = writeFile(mediaName);
            if (media.isEmpty())
                return -1;
            const int id = db->insertCapture(media, QStringLiteral("screenshot"),
                                             QStringLiteral("Test Game"),
                                             QStringLiteral("2026-09-06T10:00:00"),
                                             QStringLiteral("manual"));
            if (id <= 0)
                return -1;
            QString thumb;
            if (!thumbName.isEmpty()) {
                thumb = writeFile(thumbName);
                db->setThumbnail(id, thumb);
            }
            model->refresh();
            if (mediaPath)
                *mediaPath = media;
            if (thumbPath)
                *thumbPath = thumb;
            return id;
        }

        int rowOf(int captureId) const
        {
            for (int row = 0; row < model->rowCount(); ++row) {
                const CaptureRecord* r = model->record(row);
                if (r && r->id == captureId)
                    return row;
            }
            return -1;
        }
    };

    // deleted_at of one row: null while the capture is still in the library,
    // set once it has been tombstoned.
    static bool stillInLibrary(QSqlDatabase& db, int captureId)
    {
        QSqlQuery q(db);
        q.prepare(QStringLiteral("SELECT deleted_at FROM captures WHERE id = :id"));
        q.bindValue(QStringLiteral(":id"), captureId);
        if (!q.exec() || !q.next())
            return false;
        return q.value(0).isNull();
    }

    // File ops that behave like the real filesystem except for the paths whose
    // removal is told to fail.
    static CaptureFileOps failingRemovals(const QStringList& failing)
    {
        return CaptureFileOps{
            [](const QString& path) { return QFile::exists(path); },
            [failing](const QString& path) {
                if (failing.contains(path))
                    return false;
                return QFile::remove(path);
            },
        };
    }

private slots:
    // Nothing exotic: file and row both go away, and the caller learns the
    // library changed.
    void removesFileAndRow()
    {
        Library lib;
        QVERIFY(lib.open());
        QString media;
        QString thumb;
        const int id = lib.addCapture(QStringLiteral("shot.png"), QStringLiteral("shot.thumb.jpg"),
                                      &media, &thumb);
        QVERIFY(id > 0);

        CaptureLibraryService service(lib.db.get(), lib.model.get(), nullptr,
                                      CaptureFileOps::systemOps());
        const CaptureDeletionResult r = service.deleteCapture(lib.model.get(), lib.rowOf(id));

        QCOMPARE(r.requested, 1);
        QCOMPARE(r.removed, 1);
        QCOMPARE(r.failed, 0);
        QCOMPARE(r.inconsistent, 0);
        QVERIFY(r.libraryChanged());
        QVERIFY(!QFile::exists(media));
        QVERIFY(!QFile::exists(thumb));
        RawConnection raw(lib.dbPath());
        QVERIFY(!stillInLibrary(raw.db, id));
    }

    // A capture whose file someone already deleted by hand must still be
    // removable from the library.
    void absentFileIsIdempotentSuccess()
    {
        Library lib;
        QVERIFY(lib.open());
        QString media;
        QString unused;
        const int id = lib.addCapture(QStringLiteral("gone.png"), QString(), &media, &unused);
        QVERIFY(id > 0);
        QVERIFY(QFile::remove(media));

        // Any remove() call here would be for a file that is not there any more.
        CaptureFileOps ops{
            [](const QString& path) { return QFile::exists(path); },
            [](const QString&) { return false; },
        };
        CaptureLibraryService service(lib.db.get(), lib.model.get(), nullptr, ops);
        const CaptureDeletionResult r = service.deleteCapture(lib.model.get(), lib.rowOf(id));

        QCOMPARE(r.removed, 1);
        QCOMPARE(r.failed, 0);
        QVERIFY(r.libraryChanged());
        RawConnection raw(lib.dbPath());
        QVERIFY(!stillInLibrary(raw.db, id));
    }

    // The regression this item is about: a file that cannot be deleted keeps
    // its library entry and its thumbnail, and does not claim success.
    void fileFailureKeepsRowAndThumbnail()
    {
        Library lib;
        QVERIFY(lib.open());
        QString media;
        QString thumb;
        const int id = lib.addCapture(QStringLiteral("locked.png"),
                                      QStringLiteral("locked.thumb.jpg"), &media, &thumb);
        QVERIFY(id > 0);

        CaptureLibraryService service(lib.db.get(), lib.model.get(), nullptr,
                                      failingRemovals({ media }));
        const CaptureDeletionResult r = service.deleteCapture(lib.model.get(), lib.rowOf(id));

        QCOMPARE(r.requested, 1);
        QCOMPARE(r.removed, 0);
        QCOMPARE(r.failed, 1);
        QCOMPARE(r.inconsistent, 0);
        QVERIFY(!r.libraryChanged());
        QVERIFY(QFile::exists(media));
        QVERIFY(QFile::exists(thumb));
        RawConnection raw(lib.dbPath());
        QVERIFY(stillInLibrary(raw.db, id));
    }

    // File gone, row still there: the truth is "inconsistent", not "deleted",
    // and the row plus thumbnail survive so a rescan can reconcile it.
    void dbFailureAfterFileRemovalIsInconsistent()
    {
        Library lib;
        QVERIFY(lib.open());
        QString media;
        QString thumb;
        const int id = lib.addCapture(QStringLiteral("orphan.png"),
                                      QStringLiteral("orphan.thumb.jpg"), &media, &thumb);
        QVERIFY(id > 0);

        RawConnection raw(lib.dbPath());
        QSqlQuery trigger(raw.db);
        QVERIFY2(trigger.exec(QStringLiteral(
                     "CREATE TRIGGER block_delete BEFORE UPDATE OF deleted_at ON captures "
                     "BEGIN SELECT RAISE(ABORT, 'blocked'); END")),
                 qPrintable(trigger.lastError().text()));

        CaptureLibraryService service(lib.db.get(), lib.model.get(), nullptr,
                                      CaptureFileOps::systemOps());
        const CaptureDeletionResult r = service.deleteCapture(lib.model.get(), lib.rowOf(id));

        QCOMPARE(r.removed, 0);
        QCOMPARE(r.failed, 0);
        QCOMPARE(r.inconsistent, 1);
        QVERIFY(!r.libraryChanged());
        QVERIFY(!QFile::exists(media));
        QVERIFY2(QFile::exists(thumb), "the thumbnail is the only preview left for reconciliation");
        QVERIFY(stillInLibrary(raw.db, id));
    }

    // A thumbnail is regenerated on demand, so failing to delete it must not
    // fail the deletion.
    void thumbnailFailureStillSucceeds()
    {
        Library lib;
        QVERIFY(lib.open());
        QString media;
        QString thumb;
        const int id = lib.addCapture(QStringLiteral("keepthumb.png"),
                                      QStringLiteral("keepthumb.thumb.jpg"), &media, &thumb);
        QVERIFY(id > 0);

        CaptureLibraryService service(lib.db.get(), lib.model.get(), nullptr,
                                      failingRemovals({ thumb }));
        const CaptureDeletionResult r = service.deleteCapture(lib.model.get(), lib.rowOf(id));

        QCOMPARE(r.removed, 1);
        QCOMPARE(r.failed, 0);
        QCOMPARE(r.inconsistent, 0);
        QVERIFY(r.libraryChanged());
        QVERIFY(!QFile::exists(media));
        QVERIFY(QFile::exists(thumb));
        RawConnection raw(lib.dbPath());
        QVERIFY(!stillInLibrary(raw.db, id));
    }

    // Bulk deletion reports what happened per item instead of a blanket true.
    void bulkBatchReportsExactCounts()
    {
        Library lib;
        QVERIFY(lib.open());
        QString okMedia;
        QString okThumb;
        QString absentMedia;
        QString lockedMedia;
        QString lockedThumb;
        QString unused;
        const int okId = lib.addCapture(QStringLiteral("bulk-ok.png"),
                                        QStringLiteral("bulk-ok.thumb.jpg"), &okMedia, &okThumb);
        const int absentId = lib.addCapture(QStringLiteral("bulk-absent.png"), QString(),
                                            &absentMedia, &unused);
        const int lockedId = lib.addCapture(QStringLiteral("bulk-locked.png"),
                                            QStringLiteral("bulk-locked.thumb.jpg"), &lockedMedia,
                                            &lockedThumb);
        QVERIFY(okId > 0 && absentId > 0 && lockedId > 0);
        QVERIFY(QFile::remove(absentMedia));

        CaptureLibraryService service(lib.db.get(), lib.model.get(), nullptr,
                                      failingRemovals({ lockedMedia }));
        const QVariantList rows{ lib.rowOf(okId), lib.rowOf(absentId), lib.rowOf(lockedId) };
        const CaptureDeletionResult r = service.deleteCaptures(lib.model.get(), rows);

        QCOMPARE(r.requested, 3);
        QCOMPARE(r.removed, 2);
        QCOMPARE(r.failed, 1);
        QCOMPARE(r.inconsistent, 0);
        QVERIFY(r.libraryChanged());
        QVERIFY(!r.allRemoved());

        RawConnection raw(lib.dbPath());
        QVERIFY(!stillInLibrary(raw.db, okId));
        QVERIFY(!stillInLibrary(raw.db, absentId));
        QVERIFY2(stillInLibrary(raw.db, lockedId), "a failed file deletion keeps its library entry");
        QVERIFY(QFile::exists(lockedMedia));
        QVERIFY(QFile::exists(lockedThumb));
    }

    // A batch in which nothing could be deleted must not look like a change.
    void bulkAllFailuresChangeNothing()
    {
        Library lib;
        QVERIFY(lib.open());
        QString firstMedia;
        QString secondMedia;
        QString unused;
        const int firstId = lib.addCapture(QStringLiteral("all-fail-1.png"), QString(),
                                           &firstMedia, &unused);
        const int secondId = lib.addCapture(QStringLiteral("all-fail-2.png"), QString(),
                                            &secondMedia, &unused);
        QVERIFY(firstId > 0 && secondId > 0);

        CaptureLibraryService service(lib.db.get(), lib.model.get(), nullptr,
                                      failingRemovals({ firstMedia, secondMedia }));
        const QVariantList rows{ lib.rowOf(firstId), lib.rowOf(secondId) };
        const CaptureDeletionResult r = service.deleteCaptures(lib.model.get(), rows);

        QCOMPARE(r.requested, 2);
        QCOMPARE(r.removed, 0);
        QCOMPARE(r.failed, 2);
        QVERIFY(!r.libraryChanged());
        RawConnection raw(lib.dbPath());
        QVERIFY(stillInLibrary(raw.db, firstId));
        QVERIFY(stillInLibrary(raw.db, secondId));
    }
};

QTEST_MAIN(TestCaptureLibraryDeletion)
#include "tst_capturelibrarydeletion.moc"
