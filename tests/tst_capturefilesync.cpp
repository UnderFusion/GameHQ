#include "storage/CaptureDatabase.h"
#include "storage/CaptureFolderWatcher.h"
#include "storage/CaptureScanner.h"
#include "config/CaptureLocations.h"
#include "ui/GalleryModel.h"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

// Only watched folders are scanned here, so the managed-root lookup the
// scanner makes is stubbed instead of linking config/ConfigManager.
QStringList CaptureLocations::managedRoots() const { return {}; }

// A capture deleted or restored outside GameHQ (Explorer, Recycle Bin) must
// leave or rejoin the gallery on its own. Vanished media is hidden through a
// session-only list and never deleted from the library, so favorites and
// metadata survive a round trip through the Recycle Bin.
class TestCaptureFileSync : public QObject
{
    Q_OBJECT

private:
    struct Library
    {
        QTemporaryDir dir;
        QString root;
        std::unique_ptr<CaptureDatabase> db;
        std::unique_ptr<CaptureScanner> scanner;

        ~Library()
        {
            scanner.reset();
            db.reset();
            QSqlDatabase::removeDatabase(QStringLiteral("gamehq"));
        }

        bool open()
        {
            root = QDir::cleanPath(dir.filePath(QStringLiteral("Imported")));
            QDir().mkpath(root + QStringLiteral("/Game A"));
            db = std::make_unique<CaptureDatabase>(dir.filePath(QStringLiteral("gamehq.db")));
            if (!db->open())
                return false;
            db->addWatchedFolder(root, QStringLiteral("Imported"));
            scanner = std::make_unique<CaptureScanner>(db.get(), nullptr,
                                                       dir.filePath(QStringLiteral("thumbs")));
            return true;
        }

        QString writeShot(const QString& name)
        {
            const QString path = root + QStringLiteral("/Game A/") + name;
            QImage image(8, 8, QImage::Format_RGB32);
            image.fill(Qt::red);
            return image.save(path) ? path : QString();
        }
    };

private slots:
    void vanishedFileIsHiddenAndComesBackWithItsFavorite()
    {
        Library lib;
        QVERIFY(lib.open());
        const QString kept = lib.writeShot(QStringLiteral("kept.png"));
        const QString gone = lib.writeShot(QStringLiteral("gone.png"));
        QCOMPARE(lib.scanner->scanAll(), 2);

        QVector<CaptureRecord> rows = lib.db->listCaptures(QStringLiteral("all"));
        QCOMPARE(rows.size(), 2);
        for (const CaptureRecord& r : rows) {
            if (r.filePath == gone)
                QVERIFY(lib.db->setFavorite(r.id, true));
        }

        // Deleted in Explorer: hidden from captures and favorites, row kept.
        QVERIFY(QFile::remove(gone));
        CaptureScanner::Delta delta = lib.scanner->reconcile({ lib.root }, 0);
        QCOMPARE(delta.hidden, 1);
        rows = lib.db->listCaptures(QStringLiteral("all"));
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.first().filePath, kept);
        QVERIFY(lib.db->listCaptures(QStringLiteral("favorites")).isEmpty());
        QVERIFY(lib.db->hasCapture(gone));

        // Restored from the Recycle Bin: back, still a favorite.
        QVERIFY(!lib.writeShot(QStringLiteral("gone.png")).isEmpty());
        delta = lib.scanner->reconcile({ lib.root }, 0);
        QCOMPARE(delta.restored, 1);
        QCOMPARE(delta.added, 0);
        const QVector<CaptureRecord> favorites = lib.db->listCaptures(QStringLiteral("favorites"));
        QCOMPARE(favorites.size(), 1);
        QCOMPARE(favorites.first().filePath, gone);
    }

    void wholeFolderGoneHidesItsGameAndRestoresIt()
    {
        Library lib;
        QVERIFY(lib.open());
        QVERIFY(!lib.writeShot(QStringLiteral("a.png")).isEmpty());
        QCOMPARE(lib.scanner->scanAll(), 1);
        QCOMPARE(lib.db->listGames().size(), 1);

        const QString gameDir = lib.root + QStringLiteral("/Game A");
        const QString parked = lib.dir.filePath(QStringLiteral("parked"));
        QVERIFY(QDir().rename(gameDir, parked));
        QCOMPARE(lib.scanner->reconcile({ lib.root }, 0).hidden, 1);
        QVERIFY(lib.db->listGames().isEmpty());

        QVERIFY(QDir().rename(parked, gameDir));
        QCOMPARE(lib.scanner->reconcile({ lib.root }, 0).restored, 1);
        QCOMPARE(lib.db->listGames().size(), 1);
    }

    void freshFileWaitsForTheSettleWindow()
    {
        Library lib;
        QVERIFY(lib.open());
        QVERIFY(!lib.writeShot(QStringLiteral("new.png")).isEmpty());
        CaptureScanner::Delta delta = lib.scanner->reconcile({ lib.root }, 60000);
        QCOMPARE(delta.added, 0);
        QVERIFY(delta.deferred);
        delta = lib.scanner->reconcile({ lib.root }, 0);
        QCOMPARE(delta.added, 1);
        QVERIFY(!delta.deferred);
    }

    void gallerySyncChangesRowsWithoutReset()
    {
        Library lib;
        QVERIFY(lib.open());
        const QString first = lib.writeShot(QStringLiteral("1.png"));
        const QString second = lib.writeShot(QStringLiteral("2.png"));
        QCOMPARE(lib.scanner->scanAll(), 2);
        GalleryModel model(lib.db.get());
        QCOMPARE(model.rowCount(), 2);

        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        QSignalSpy removed(&model, &QAbstractItemModel::rowsRemoved);
        QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
        QVERIFY(QFile::remove(second));
        lib.scanner->reconcile({ lib.root }, 0);
        model.sync();
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.get(0).value(QStringLiteral("filePath")).toString(), first);
        QCOMPARE(removed.size(), 1);

        QVERIFY(!lib.writeShot(QStringLiteral("2.png")).isEmpty());
        lib.scanner->reconcile({ lib.root }, 0);
        model.sync();
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(inserted.size(), 1);
        QCOMPARE(resets.size(), 0);
    }

    // The real Windows notification, not a simulated signal: a delete deep in
    // the tree reports its root once, after the debounce.
    void watcherReportsADeleteUnderTheRoot()
    {
        Library lib;
        QVERIFY(lib.open());
        const QString shot = lib.writeShot(QStringLiteral("w.png"));
        CaptureFolderWatcher watcher;
        watcher.setRoots({ lib.root });
        QCOMPARE(watcher.roots().size(), 1);

        QSignalSpy changed(&watcher, &CaptureFolderWatcher::rootsChanged);
        QVERIFY(QFile::remove(shot));
        QVERIFY(changed.wait(5000));
        const QStringList roots = changed.first().first().toStringList();
        QCOMPARE(roots, QStringList{ lib.root });
    }
};

QTEST_MAIN(TestCaptureFileSync)
#include "tst_capturefilesync.moc"
