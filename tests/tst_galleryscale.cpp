#include "config/Paths.h"
#include "storage/CaptureDatabase.h"
#include "storage/ThumbnailScheduler.h"
#include "ui/CaptureLibraryService.h"
#include "ui/GalleryModel.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QPersistentModelIndex>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>

#include <windows.h>
#include <psapi.h>

#include <memory>

// t39/A09: the library at 1k/10k/50k rows. Measures what a capture commit
// costs the GUI thread with a desktop ("all") and an overlay ("recent")
// gallery open, and pins ordering, selection and stored-state behaviour.
namespace
{
qint64 privateBytes()
{
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                         sizeof(pmc));
    return qint64(pmc.PrivateUsage);
}

QSqlDatabase conn() { return QSqlDatabase::database(QStringLiteral("gamehq")); }

// rows synthetic captures for one game, `dupEvery` of them sharing a timestamp.
bool populate(const QString& root, int rows, int dupEvery = 5)
{
    QSqlDatabase db = conn();
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("INSERT INTO games (display_name, created_at) "
                               "VALUES ('Bench', '2026-01-01T00:00:00Z')"))) {
        qWarning() << q.lastError().text();
        return false;
    }
    const QVariant gameId = q.lastInsertId();
    if (!db.transaction())
        return false;
    q.prepare(QStringLiteral("INSERT INTO captures (file_path, type, game_id, created_at, source, "
                             "is_favorite) VALUES (:p, :t, :g, :c, 'GameHQ', :f)"));
    const QDateTime base = QDateTime(QDate(2026, 1, 1), QTime(0, 0), QTimeZone::UTC);
    // GAMEHQ_GALLERY_FILES=1: every row's file exists, as in a healthy
    // library; otherwise none does, the worst case for path repair.
    const bool files = qEnvironmentVariableIntValue("GAMEHQ_GALLERY_FILES") == 1;
    if (files)
        QDir().mkpath(root + QStringLiteral("/Bench/Screenshots"));
    for (int i = 0; i < rows; ++i) {
        const QString path = QStringLiteral("%1/Bench/Screenshots/s%2.png").arg(root).arg(i);
        if (files) {
            QFile f(path);
            if (!f.open(QIODevice::WriteOnly))
                return false;
        }
        q.bindValue(QStringLiteral(":p"), path);
        q.bindValue(QStringLiteral(":t"), i % 4 ? QStringLiteral("screenshot") : QStringLiteral("video"));
        q.bindValue(QStringLiteral(":g"), gameId);
        q.bindValue(QStringLiteral(":c"), base.addSecs(i / dupEvery).toString(Qt::ISODate));
        q.bindValue(QStringLiteral(":f"), i % 10 == 0 ? 1 : 0);
        if (!q.exec()) {
            qWarning() << q.lastError().text();
            db.rollback();
            return false;
        }
    }
    return db.commit();
}

struct Library
{
    QTemporaryDir dir;
    std::unique_ptr<CaptureDatabase> db;
    std::unique_ptr<GalleryModel> desktop;
    std::unique_ptr<GalleryModel> overlay;
    std::unique_ptr<CaptureLibraryService> service;

    bool open(int rows)
    {
        db = std::make_unique<CaptureDatabase>(dir.filePath(QStringLiteral("gamehq.db")));
        if (!db->open() || (rows > 0 && !populate(QDir::cleanPath(dir.path()), rows)))
            return false;
        desktop = std::make_unique<GalleryModel>(db.get());
        overlay = std::make_unique<GalleryModel>(db.get());
        overlay->setFilter(QStringLiteral("recent"));
        // A decoder that never runs: these measurements exclude thumbnails.
        service = std::make_unique<CaptureLibraryService>(
            db.get(), desktop.get(), overlay.get(), CaptureFileOps::systemOps(),
            std::make_unique<ThumbnailScheduler>(dir.filePath(QStringLiteral("thumbs")),
                                                 [](const QString&, const QString&, const QString&) {
                                                     return QString();
                                                 }));
        return true;
    }
    ~Library()
    {
        service.reset();
        overlay.reset();
        desktop.reset();
        db.reset();
        QSqlDatabase::removeDatabase(QStringLiteral("gamehq"));
    }
    QString writeShot(const QString& name)
    {
        const QString path = dir.filePath(name);
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return {};
        f.write("png");
        return path;
    }
};

QList<int> ids(const GalleryModel& m)
{
    QList<int> out;
    for (int row = 0; row < m.rowCount(); ++row)
        out.append(m.record(row)->id);
    return out;
}
} // namespace

class TestGalleryScale : public QObject
{
    Q_OBJECT

private slots:
    void commitCost_data()
    {
        QTest::addColumn<int>("rows");
        QTest::newRow("1k") << 1000;
        QTest::newRow("10k") << 10000;
        QTest::newRow("50k") << 50000;
    }

    void commitCost()
    {
        QFETCH(int, rows);
        Library lib;
        QVERIFY(lib.open(rows));
        QCOMPARE(lib.desktop->rowCount(), rows);

        QElapsedTimer t;
        t.start();
        const QVector<CaptureRecord> all = lib.db->listCaptures(QStringLiteral("all"));
        const qint64 listMs = t.elapsed();
        QCOMPARE(all.size(), rows);

        const qint64 before = privateBytes();
        t.restart();
        lib.desktop->refresh();
        const qint64 refreshMs = t.elapsed();
        const qint64 refreshBytes = privateBytes() - before;

        t.restart();
        lib.desktop->sync();
        const qint64 syncMs = t.elapsed();

        QSignalSpy resetDesk(lib.desktop.get(), &QAbstractItemModel::modelReset);
        QSignalSpy resetOver(lib.overlay.get(), &QAbstractItemModel::modelReset);
        QSignalSpy insDesk(lib.desktop.get(), &QAbstractItemModel::rowsInserted);
        QSignalSpy insOver(lib.overlay.get(), &QAbstractItemModel::rowsInserted);
        const QString shot = lib.writeShot(QStringLiteral("new.png"));
        constexpr int kCommits = 5;
        t.restart();
        for (int i = 0; i < kCommits; ++i) {
            const QString path = lib.writeShot(QStringLiteral("new%1.png").arg(i));
            QCOMPARE(lib.service->commitCapture(path, QStringLiteral("screenshot"),
                                                QStringLiteral("Bench")),
                     CaptureCommitOutcome::Indexed);
        }
        const double commitMs = double(t.elapsed()) / kCommits;
        Q_UNUSED(shot);
        QCOMPARE(lib.desktop->rowCount(), rows + kCommits);
        QCOMPARE(lib.overlay->rowCount(), 50);

        qInfo().noquote() << QStringLiteral(
            "%1 rows: listCaptures %2 ms, refresh %3 ms (+%4 KiB private), sync %5 ms, "
            "commitCapture %6 ms; per commit resets desktop/overlay %7/%8, inserts %9/%10")
            .arg(rows).arg(listMs).arg(refreshMs).arg(refreshBytes / 1024).arg(syncMs)
            .arg(commitMs, 0, 'f', 1)
            .arg(double(resetDesk.size()) / kCommits).arg(double(resetOver.size()) / kCommits)
            .arg(double(insDesk.size()) / kCommits).arg(double(insOver.size()) / kCommits);
    }

    void sameTimestampRowsHaveOneStableOrder()
    {
        Library lib;
        QVERIFY(lib.open(200));   // five rows share each timestamp
        const QList<int> first = ids(*lib.desktop);
        lib.desktop->refresh();
        QCOMPARE(ids(*lib.desktop), first);
        // Within a timestamp, the newer id comes first.
        for (int row = 1; row < lib.desktop->rowCount(); ++row) {
            const CaptureRecord* a = lib.desktop->record(row - 1);
            const CaptureRecord* b = lib.desktop->record(row);
            QVERIFY(a->createdAt > b->createdAt || (a->createdAt == b->createdAt && a->id > b->id));
        }
    }

    void incrementalInsertsMatchAFullQueryInEveryView()
    {
        Library lib;
        QVERIFY(lib.open(300));
        const int benchGame = lib.desktop->record(0)->gameId;
        GalleryModel favorites(lib.db.get());
        favorites.setFilter(QStringLiteral("favorites"));
        GalleryModel clipsOnly(lib.db.get());
        clipsOnly.setFilter(QStringLiteral("clips"));
        GalleryModel game(lib.db.get());
        game.setFilter(QStringLiteral("all"), benchGame);
        QSignalSpy reset(lib.desktop.get(), &QAbstractItemModel::modelReset);
        for (int i = 0; i < 12; ++i) {
            const QString path = lib.writeShot(QStringLiteral("inc%1.%2").arg(i).arg(i % 3 ? "png" : "mp4"));
            if (i % 3)
                QCOMPARE(lib.service->commitCapture(path, QStringLiteral("screenshot"),
                                                    i % 2 ? QStringLiteral("Bench") : QStringLiteral("Other")),
                         CaptureCommitOutcome::Indexed);
            else
                QCOMPARE(lib.service->commitClip(path, QStringLiteral("Bench"), QString()),
                         CaptureCommitOutcome::Indexed);
            for (GalleryModel* m : {&favorites, &clipsOnly, &game})
                m->addCapture(lib.desktop->record(0)->id);
        }
        QCOMPARE(reset.size(), 0);
        const auto same = [&](GalleryModel& m) {
            QList<int> expected;
            for (const CaptureRecord& r : lib.db->listCaptures(m.category(), m.gameId()))
                expected.append(r.id);
            return ids(m) == expected;
        };
        QVERIFY(same(*lib.desktop));
        QVERIFY(same(*lib.overlay));
        QCOMPARE(lib.overlay->rowCount(), 50);
        QVERIFY(same(favorites));
        QVERIFY(same(clipsOnly));
        QVERIFY(same(game));
    }

    void selectionSurvivesCommitsAndDeletions()
    {
        Library lib;
        QVERIFY(lib.open(100));
        // The capture the user is looking at, on both views.
        const QPersistentModelIndex deskSel(lib.desktop->index(10));
        const int deskId = lib.desktop->record(10)->id;
        const QPersistentModelIndex overSel(lib.overlay->index(3));
        const int overId = lib.overlay->record(3)->id;
        QSignalSpy resetDesk(lib.desktop.get(), &QAbstractItemModel::modelReset);
        QSignalSpy resetOver(lib.overlay.get(), &QAbstractItemModel::modelReset);
        for (int i = 0; i < 3; ++i)
            QCOMPARE(lib.service->commitCapture(lib.writeShot(QStringLiteral("sel%1.png").arg(i)),
                                                QStringLiteral("screenshot"), QStringLiteral("Bench")),
                     CaptureCommitOutcome::Indexed);
        // A just-committed capture is deleted again (a GameHQ capture, so it
        // goes to the Recycle Bin path; the file ops keep the disk untouched).
        CaptureFileOps keep = CaptureFileOps::systemOps();
        keep.trash = [](const QString&) { return true; };
        keep.remove = [](const QString&) { return true; };
        CaptureLibraryService deleter(lib.db.get(), lib.desktop.get(), lib.overlay.get(), keep,
                                      std::make_unique<ThumbnailScheduler>(lib.dir.filePath(QStringLiteral("t2")),
                                          [](const QString&, const QString&, const QString&) { return QString(); }));
        QCOMPARE(deleter.deleteCapture(lib.desktop.get(), 0).removed, 1);
        QCOMPARE(resetDesk.size(), 0);
        QCOMPARE(resetOver.size(), 0);
        QVERIFY(deskSel.isValid() && overSel.isValid());
        QCOMPARE(lib.desktop->record(deskSel.row())->id, deskId);
        QCOMPARE(lib.overlay->record(overSel.row())->id, overId);
        QCOMPARE(deskSel.row(), 12);   // pushed down by the two remaining new rows
    }

    void batchPathRepairMatchesTheSingleCall()
    {
        QTemporaryDir dir;
        const QString existing = dir.filePath(QStringLiteral("there.png"));
        QFile f(existing);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();
        const Paths::MovedPathRepairer batch;
        const QStringList cases = {
            QString(), QStringLiteral("  "), existing, dir.filePath(QStringLiteral("gone.png")),
            QStringLiteral("portable:/Captures/Game/a.png"),
            QStringLiteral("D:/Old/GameHQ/Captures/Game/Screenshots/a.png"),
            QStringLiteral("D:/Old/gamehq-data/thumbnails/abc.jpg"),
            Paths::thumbnailsDir() + QStringLiteral("/never-written-t39.jpg"),
            Paths::capturesRoot() + QStringLiteral("/Never Game t39/x.png"),
            QDir::toNativeSeparators(existing),
        };
        for (const QString& p : cases)
            QCOMPARE(batch.repair(p), Paths::repairMovedPath(p));
    }

    void profileRepair()
    {
        QTemporaryDir dir;
        const QString existing = dir.filePath(QStringLiteral("e.png"));
        QFile f(existing);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();
        const QString missing = dir.filePath(QStringLiteral("missing.png"));
        QElapsedTimer t;
        constexpr int n = 2000;
        t.start();
        for (int i = 0; i < n; ++i) Paths::repairMovedPath(existing);
        const double e = double(t.nsecsElapsed()) / n / 1000;
        t.restart();
        for (int i = 0; i < n; ++i) Paths::repairMovedPath(missing);
        const double m = double(t.nsecsElapsed()) / n / 1000;
        t.restart();
        for (int i = 0; i < n; ++i) Paths::capturesRoot();
        const double r = double(t.nsecsElapsed()) / n / 1000;
        t.restart();
        for (int i = 0; i < n; ++i) QFileInfo::exists(missing);
        const double s = double(t.nsecsElapsed()) / n / 1000;
        qInfo() << "us per call: repair(existing)" << e << "repair(missing)" << m
                << "capturesRoot()" << r << "exists(missing)" << s;
    }

    void queryPlan()
    {
        Library lib;
        QVERIFY(lib.open(1000));
        QSqlQuery q(conn());
        QVERIFY(q.exec(QStringLiteral(
            "EXPLAIN QUERY PLAN SELECT c.id FROM captures c LEFT JOIN games g ON g.id = c.game_id "
            "WHERE c.deleted_at IS NULL AND c.file_path NOT IN (SELECT file_path FROM "
            "temp.missing_captures) ORDER BY c.created_at DESC, c.id DESC")));
        QStringList plan;
        while (q.next())
            plan.append(q.value(3).toString());
        qInfo().noquote() << "listCaptures plan:" << plan.join(QStringLiteral(" | "));
    }
};

QTEST_MAIN(TestGalleryScale)
#include "tst_galleryscale.moc"
