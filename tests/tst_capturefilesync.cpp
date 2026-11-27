#include "storage/CaptureDatabase.h"
#include "storage/CaptureFolderWatcher.h"
#include "storage/CaptureScanner.h"
#include "config/CaptureLocations.h"
#include "ui/GalleryModel.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

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

    struct Passes
    {
        QList<QStringList> roots;
        QList<CaptureScanner::Delta> deltas;
    };

    static void record(CaptureScanner* scanner, Passes& passes)
    {
        QObject::connect(scanner, &CaptureScanner::foldersReconciled, scanner,
                         [&passes](const QStringList& roots, const CaptureScanner::Delta& d) {
                             passes.roots.append(roots);
                             passes.deltas.append(d);
                         });
    }

    // Library contents relative to the root: "game|type|file|favorite".
    static QStringList snapshot(Library& lib)
    {
        QStringList out;
        for (const CaptureRecord& r : lib.db->listCaptures(QStringLiteral("all")))
            out.append(r.gameName + '|' + r.type + '|'
                       + QDir(lib.root).relativeFilePath(r.filePath) + '|'
                       + (r.isFavorite ? "fav" : "-"));
        out.sort();
        return out;
    }

private slots:
    // aud-b3: the full scan used to run on the GUI thread, thumbnail decoding
    // included. The background scan returns at once, indexes the same files,
    // and folds a second request into one follow-up pass.
    void backgroundScanIndexesWithoutBlocking()
    {
        Library lib;
        QVERIFY(lib.open());
        QVERIFY(!lib.writeShot(QStringLiteral("a.png")).isEmpty());
        QVERIFY(!lib.writeShot(QStringLiteral("b.png")).isEmpty());
        QSignalSpy finished(lib.scanner.get(), &CaptureScanner::scanFinished);
        lib.scanner->scanAllAsync();
        QVERIFY(lib.scanner->scanInProgress());
        lib.scanner->scanAllAsync();   // folded into one follow-up
        QTRY_COMPARE(finished.count(), 2);
        QCOMPARE(finished.at(0).at(0).toInt(), 2);
        QCOMPARE(finished.at(1).at(0).toInt(), 0);   // nothing new the second time
        QVERIFY(!lib.scanner->scanInProgress());
        QCOMPARE(lib.db->listCaptures(QStringLiteral("all")).size(), 2);
        for (const CaptureRecord& r : lib.db->listCaptures(QStringLiteral("all")))
            QVERIFY2(!r.thumbnailPath.isEmpty(), qPrintable(r.filePath));
    }

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

    // ---- t35: folder reconciliation off the GUI thread --------------------
    // CaptureFolderWatcher used to call reconcile() synchronously: the walk,
    // thumbnail decoding and existence checks froze the window for as long as
    // the folder took. reconcileAsync() shares the full scan's single worker.

    void asyncReconcileHidesAndRestoresLikeTheSyncPass()
    {
        Library lib;
        QVERIFY(lib.open());
        lib.writeShot(QStringLiteral("kept.png"));
        const QString gone = lib.writeShot(QStringLiteral("gone.png"));
        QCOMPARE(lib.scanner->scanAll(), 2);
        for (const CaptureRecord& r : lib.db->listCaptures(QStringLiteral("all"))) {
            if (r.filePath == gone)
                QVERIFY(lib.db->setFavorite(r.id, true));
        }
        Passes passes;
        record(lib.scanner.get(), passes);

        QVERIFY(QFile::remove(gone));
        lib.writeShot(QStringLiteral("new.png"));
        lib.scanner->reconcileAsync({ lib.root }, 0);
        QVERIFY(lib.scanner->scanInProgress());   // returned before the walk
        QCOMPARE(passes.deltas.size(), 0);
        QTRY_COMPARE(passes.deltas.size(), 1);
        QCOMPARE(passes.roots.first(), QStringList{ lib.root });
        QCOMPARE(passes.deltas.first().hidden, 1);
        QCOMPARE(passes.deltas.first().added, 1);
        QVERIFY(lib.db->listCaptures(QStringLiteral("favorites")).isEmpty());
        QVERIFY(lib.db->hasCapture(gone));

        QVERIFY(!lib.writeShot(QStringLiteral("gone.png")).isEmpty());
        lib.scanner->reconcileAsync({ lib.root }, 0);
        QTRY_COMPARE(passes.deltas.size(), 2);
        QCOMPARE(passes.deltas.last().restored, 1);
        QCOMPARE(passes.deltas.last().added, 0);
        QCOMPARE(lib.db->listCaptures(QStringLiteral("favorites")).size(), 1);
        QVERIFY(!lib.scanner->scanInProgress());
    }

    void asyncReconcileStillHonoursTheSettleWindow()
    {
        Library lib;
        QVERIFY(lib.open());
        lib.writeShot(QStringLiteral("fresh.png"));
        Passes passes;
        record(lib.scanner.get(), passes);
        lib.scanner->reconcileAsync({ lib.root }, 60000);
        QTRY_COMPARE(passes.deltas.size(), 1);
        QVERIFY(passes.deltas.first().deferred);
        QCOMPARE(passes.deltas.first().added, 0);
        QVERIFY(lib.db->listCaptures(QStringLiteral("all")).isEmpty());
    }

    void aWatcherBurstCoalescesIntoOneFollowUp()
    {
        Library lib;
        QVERIFY(lib.open());
        for (int i = 0; i < 40; ++i)
            lib.writeShot(QStringLiteral("s%1.png").arg(i));
        const QString other = QDir::cleanPath(lib.dir.filePath(QStringLiteral("Other")));
        QDir().mkpath(other + QStringLiteral("/Game B"));
        QVERIFY(lib.db->addWatchedFolder(other, QStringLiteral("Imported")));
        Passes passes;
        record(lib.scanner.get(), passes);

        lib.scanner->reconcileAsync({ lib.root }, 0);
        for (int i = 0; i < 100; ++i)
            lib.scanner->reconcileAsync({ i % 2 ? other : lib.root }, 0);
        QVERIFY(lib.scanner->reconcileQueued());
        QTRY_VERIFY(!lib.scanner->scanInProgress() && !lib.scanner->reconcileQueued());
        QTest::qWait(50);
        QCOMPARE(passes.deltas.size(), 2);   // the first pass + one merged follow-up
        QCOMPARE(passes.deltas.first().added, 40);
        QStringList merged = passes.roots.last();
        merged.sort(Qt::CaseInsensitive);
        QStringList expected{ lib.root, other };
        expected.sort(Qt::CaseInsensitive);
        QCOMPARE(merged, expected);
        QCOMPARE(lib.db->listCaptures(QStringLiteral("all")).size(), 40);
    }

    void aQueuedFullScanAbsorbsQueuedFolderChanges()
    {
        Library lib;
        QVERIFY(lib.open());
        lib.writeShot(QStringLiteral("a.png"));
        Passes passes;
        record(lib.scanner.get(), passes);
        QSignalSpy full(lib.scanner.get(), &CaptureScanner::scanFinished);
        lib.scanner->reconcileAsync({ lib.root }, 0);
        lib.scanner->reconcileAsync({ lib.root }, 0);
        lib.scanner->scanAllAsync();
        QTRY_COMPARE(full.count(), 1);
        QTest::qWait(50);
        QCOMPARE(passes.deltas.size(), 1);
        QVERIFY(!lib.scanner->reconcileQueued());
        QCOMPARE(lib.db->listCaptures(QStringLiteral("all")).size(), 1);
    }

    void aResultWalkedUnderOldRootsIsDropped()
    {
        // The folder is removed from the library while its walk is in flight:
        // the stale result must not insert its files, and a full scan of the
        // new configuration follows.
        Library lib;
        QVERIFY(lib.open());
        lib.writeShot(QStringLiteral("a.png"));
        lib.writeShot(QStringLiteral("b.png"));
        Passes passes;
        record(lib.scanner.get(), passes);
        QSignalSpy full(lib.scanner.get(), &CaptureScanner::scanFinished);
        lib.scanner->reconcileAsync({ lib.root }, 0);
        // finishPass() is queued to this thread, so this lands first.
        QVERIFY(lib.db->removeWatchedFolder(lib.root));
        QTRY_COMPARE(full.count(), 1);
        QCOMPARE(passes.deltas.size(), 0);
        QVERIFY(lib.db->listCaptures(QStringLiteral("all")).isEmpty());
        // A reconcile for a folder that is no longer a root does nothing.
        lib.scanner->reconcileAsync({ lib.root }, 0);
        QCOMPARE(passes.deltas.size(), 1);
        QVERIFY(!passes.deltas.first().changed());
        QVERIFY(!lib.scanner->scanInProgress());
    }

    void shutdownCancelsALongWalk()
    {
        Library lib;
        QVERIFY(lib.open());
        for (int i = 0; i < 600; ++i)
            lib.writeShot(QStringLiteral("s%1.png").arg(i));
        lib.scanner->reconcileAsync({ lib.root }, 0);
        QTest::qWait(20);   // the worker is decoding thumbnails now
        QElapsedTimer timer;
        timer.start();
        lib.scanner.reset();
        const qint64 shutdownMs = timer.elapsed();
        qInfo() << "scanner shutdown during a 600-file walk took" << shutdownMs << "ms";
        QVERIFY2(shutdownMs < 1000, qPrintable(QString::number(shutdownMs)));
        QTest::qWait(50);   // a dropped result must not reach the dead scanner
        QVERIFY(lib.db->listCaptures(QStringLiteral("all")).isEmpty());
    }

    // Disposable synthetic libraries; GAMEHQ_SCAN_BENCH=1 adds 1,000 and 10,000
    // files (=1000 or =10000 runs just that size).
    // Compares the old synchronous reconcile (the GUI thread is blocked for
    // its whole duration) with the worst GUI-thread gap during reconcileAsync,
    // and checks both produce the same library.
    void reconcileBenchmark_data()
    {
        QTest::addColumn<int>("files");
        // Thumbnail decoding dominates and is slow on a scanned temp folder,
        // so the default row stays small. Set QTEST_FUNCTION_TIMEOUT for 10k.
        const int bench = qEnvironmentVariableIntValue("GAMEHQ_SCAN_BENCH");
        if (bench == 0 || bench == 1)
            QTest::newRow("100") << 100;
        if (bench == 1 || bench == 1000)
            QTest::newRow("1k") << 1000;
        if (bench == 1 || bench == 10000)
            QTest::newRow("10k") << 10000;
    }

    void reconcileBenchmark()
    {
        QFETCH(int, files);
        QStringList results[2];
        qint64 blockedMs[2] = { 0, 0 };
        for (int mode = 0; mode < 2; ++mode) {
            Library lib;
            QVERIFY(lib.open());
            for (int g = 0; g < 10; ++g)
                QDir().mkpath(lib.root + QStringLiteral("/Game %1").arg(g));
            QImage image(8, 8, QImage::Format_RGB32);
            image.fill(Qt::blue);
            for (int i = 0; i < files; ++i)
                QVERIFY(image.save(lib.root + QStringLiteral("/Game %1/s%2.png").arg(i % 10).arg(i)));
            QElapsedTimer timer;
            if (mode == 0) {
                timer.start();
                const CaptureScanner::Delta delta = lib.scanner->reconcile({ lib.root }, 0);
                blockedMs[0] = timer.elapsed();
                QCOMPARE(delta.added, files);
            } else {
                Passes passes;
                record(lib.scanner.get(), passes);
                QElapsedTimer beat;
                qint64 worst = 0;
                QTimer heartbeat;
                heartbeat.setInterval(5);
                QObject::connect(&heartbeat, &QTimer::timeout, &heartbeat, [&] {
                    worst = qMax(worst, beat.restart());
                });
                timer.start();
                beat.start();
                heartbeat.start();
                lib.scanner->reconcileAsync({ lib.root }, 0);
                worst = qMax(worst, beat.restart());
                QTRY_COMPARE_WITH_TIMEOUT(passes.deltas.size(), 1, 600000);
                heartbeat.stop();
                worst = qMax(worst, beat.elapsed());
                blockedMs[1] = worst;
                QCOMPARE(passes.deltas.first().added, files);
                qInfo() << "async pass" << timer.elapsed() << "ms total";
            }
            results[mode] = snapshot(lib);
        }
        qInfo() << files << "files: sync reconcile blocked the GUI thread" << blockedMs[0]
                << "ms; async worst GUI gap" << blockedMs[1] << "ms";
        QCOMPARE(results[1], results[0]);
        QCOMPARE(results[0].size(), files);
    }
};

QTEST_MAIN(TestCaptureFileSync)
#include "tst_capturefilesync.moc"
