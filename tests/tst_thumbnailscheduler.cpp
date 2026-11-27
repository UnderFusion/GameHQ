#include "storage/CaptureDatabase.h"
#include "storage/ThumbnailScheduler.h"
#include "storage/ThumbnailService.h"
#include "ui/CaptureLibraryService.h"
#include "ui/GalleryModel.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QRegularExpression>
#include <QTest>
#include <QThread>

#include <atomic>
#include <memory>

// t38/A08: thumbnails are scheduled, not produced on the caller's thread.
// commitCapture used to decode the screenshot and write its preview on the
// GUI thread before the gallery even refreshed.
namespace
{
QString writeImage(const QString& path, int w, int h, QColor color)
{
    QImage image(w, h, QImage::Format_RGB32);
    image.fill(color);
    return image.save(path) ? path : QString();
}

// Counts decodes and can hold every one of them until released.
struct ControlledDecoder
{
    std::shared_ptr<std::atomic_int> calls = std::make_shared<std::atomic_int>(0);
    std::shared_ptr<std::atomic_int> running = std::make_shared<std::atomic_int>(0);
    std::shared_ptr<std::atomic_int> peak = std::make_shared<std::atomic_int>(0);
    std::shared_ptr<std::atomic_bool> hold = std::make_shared<std::atomic_bool>(false);
    std::shared_ptr<std::atomic_int> sleepMs = std::make_shared<std::atomic_int>(0);
    std::shared_ptr<std::function<void()>> during = std::make_shared<std::function<void()>>();

    ThumbnailScheduler::Decoder decoder() const
    {
        auto c = calls, r = running, p = peak, s = sleepMs;
        auto h = hold;
        auto d = during;
        return [c, r, p, h, s, d](const QString& file, const QString& type, const QString& dir) {
            c->fetch_add(1);
            const int now = r->fetch_add(1) + 1;
            int old = p->load();
            while (now > old && !p->compare_exchange_weak(old, now)) {}
            if (*d)
                (*d)();
            while (h->load())
                QThread::msleep(2);
            if (s->load() > 0)
                QThread::msleep(static_cast<unsigned long>(s->load()));
            const QString thumb = ThumbnailService::ensureThumbnail(file, type, dir);
            r->fetch_sub(1);
            return thumb;
        };
    }
};
} // namespace

class TestThumbnailScheduler : public QObject
{
    Q_OBJECT

    std::unique_ptr<QTemporaryDir> m_dir;
    QString thumbs() const { return m_dir->filePath(QStringLiteral("thumbs")); }
    QString file(const QString& name) const { return m_dir->filePath(name); }

private slots:
    void init()
    {
        m_dir = std::make_unique<QTemporaryDir>();
        QVERIFY(m_dir->isValid());
    }

    void duplicateRequestsShareOneDecode()
    {
        ControlledDecoder dec;
        dec.hold->store(true);
        ThumbnailScheduler s(thumbs(), dec.decoder());
        QSignalSpy ready(&s, &ThumbnailScheduler::thumbnailReady);
        const QString shot = writeImage(file("a.png"), 640, 360, Qt::red);
        for (int id = 1; id <= 5; ++id)
            QVERIFY(s.request(id, shot, QStringLiteral("screenshot")));
        QVERIFY(s.request(3, shot, QStringLiteral("screenshot")));   // same capture again
        dec.hold->store(false);
        QTRY_COMPARE(ready.size(), 5);
        QCOMPARE(dec.calls->load(), 1);
        QSet<int> ids;
        for (const auto& args : ready)
            ids.insert(args.at(0).toInt());
        QCOMPARE(ids, (QSet<int>{1, 2, 3, 4, 5}));
        QVERIFY(ThumbnailService::isUsableThumbnail(ready.first().at(2).toString()));
    }

    void anUnchangedSourceReusesItsCachedThumbnail()
    {
        ThumbnailScheduler s(thumbs());
        QSignalSpy ready(&s, &ThumbnailScheduler::thumbnailReady);
        const QString shot = writeImage(file("cached.png"), 1920, 1080, Qt::blue);
        QVERIFY(s.request(1, shot, QStringLiteral("screenshot")));
        QTRY_COMPARE(ready.size(), 1);
        const QString thumb = ready.first().at(2).toString();
        const QDateTime written = QFileInfo(thumb).lastModified();
        QTest::qWait(20);
        QVERIFY(s.request(2, shot, QStringLiteral("screenshot")));
        QTRY_COMPARE(ready.size(), 2);
        QCOMPARE(ready.last().at(2).toString(), thumb);
        QCOMPARE(QFileInfo(thumb).lastModified(), written);   // not re-encoded
        // Quality unchanged: a 1920-wide source still gets a 512-wide preview.
        QCOMPARE(QImage(thumb).width(), 512);
    }

    void aSourceReplacedDuringDecodeIsDecodedAgain()
    {
        ControlledDecoder dec;
        ThumbnailScheduler s(thumbs(), dec.decoder());
        QSignalSpy ready(&s, &ThumbnailScheduler::thumbnailReady);
        const QString shot = writeImage(file("swap.png"), 320, 180, Qt::red);
        std::atomic_bool swapped{false};
        *dec.during = [&] {
            if (!swapped.exchange(true)) {
                // Replaced at the same path (different size and time) while the
                // first decode runs, as an editor saving over it would.
                QThread::msleep(20);
                writeImage(shot, 640, 360, Qt::green);
            }
        };
        QVERIFY(s.request(7, shot, QStringLiteral("screenshot")));
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 5000);
        QCOMPARE(dec.calls->load(), 2);
        const QImage preview(ready.first().at(2).toString());
        QVERIFY(!preview.isNull());
        // The published preview shows the replacement, not the original.
        QCOMPARE(QColor(preview.pixel(preview.width() / 2, preview.height() / 2)).green() > 200,
                 true);
    }

    void malformedAndMissingSourcesFailOnceAndAreNotRetried()
    {
        ControlledDecoder dec;
        ThumbnailScheduler s(thumbs(), dec.decoder());
        QSignalSpy failed(&s, &ThumbnailScheduler::thumbnailFailed);
        QSignalSpy ready(&s, &ThumbnailScheduler::thumbnailReady);
        const QString junk = file("junk.png");
        {
            QFile f(junk);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("this is not a png");
        }
        QVERIFY(s.request(1, junk, QStringLiteral("screenshot")));
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(dec.calls->load(), 1);
        // Unchanged: refused at once, no second decode.
        QVERIFY(!s.request(2, junk, QStringLiteral("screenshot")));
        QCOMPARE(dec.calls->load(), 1);
        // Missing: refused without a decode.
        QVERIFY(!s.request(3, file("never.png"), QStringLiteral("screenshot")));
        QCOMPARE(dec.calls->load(), 1);
        // Repaired: a changed file is a new source and is tried again.
        QTest::qWait(20);
        QVERIFY(!writeImage(junk, 64, 64, Qt::white).isEmpty());
        QVERIFY(s.request(4, junk, QStringLiteral("screenshot")));
        QTRY_COMPARE(ready.size(), 1);
        QCOMPARE(dec.calls->load(), 2);
        QVERIFY(QFile::exists(junk));   // the original is never touched
    }

    void workersAndQueueStayBounded()
    {
        ControlledDecoder dec;
        dec.hold->store(true);
        ThumbnailScheduler s(thumbs(), dec.decoder());
        QSignalSpy done(&s, &ThumbnailScheduler::thumbnailReady);
        const QString shot = writeImage(file("base.png"), 64, 64, Qt::red);
        int accepted = 0;
        for (int i = 0; i < 400; ++i) {
            const QString copy = file(QStringLiteral("q%1.png").arg(i));
            QVERIFY(QFile::copy(shot, copy));
            accepted += s.request(i, copy, QStringLiteral("screenshot")) ? 1 : 0;
        }
        QCOMPARE(accepted, ThumbnailScheduler::kMaxJobs);
        QTRY_COMPARE(dec.running->load(), ThumbnailScheduler::kMaxWorkers);
        QCOMPARE(s.activeJobs(), ThumbnailScheduler::kMaxWorkers);
        QCOMPARE(s.queuedJobs(), ThumbnailScheduler::kMaxJobs - ThumbnailScheduler::kMaxWorkers);
        dec.hold->store(false);
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), ThumbnailScheduler::kMaxJobs, 30000);
        QCOMPARE(dec.peak->load(), ThumbnailScheduler::kMaxWorkers);
        QCOMPARE(s.peakActiveJobs(), ThumbnailScheduler::kMaxWorkers);
        QCOMPARE(s.queuedJobs(), 0);
        QCOMPARE(s.activeJobs(), 0);
    }

    void aSlowDecodeDoesNotHoldTheOthers()
    {
        ThumbnailScheduler s(thumbs(), [](const QString& f, const QString& t, const QString& d) {
            if (f.endsWith(QLatin1String("slow.png")))
                QThread::msleep(1500);   // a stalled decoder
            return ThumbnailService::ensureThumbnail(f, t, d);
        });
        QSignalSpy ready(&s, &ThumbnailScheduler::thumbnailReady);
        QVERIFY(s.request(1, writeImage(file("slow.png"), 64, 64, Qt::red), QStringLiteral("screenshot")));
        QElapsedTimer t;
        t.start();
        for (int i = 0; i < 5; ++i)
            QVERIFY(s.request(10 + i, writeImage(file(QStringLiteral("f%1.png").arg(i)), 64, 64, Qt::blue),
                              QStringLiteral("screenshot")));
        QTRY_COMPARE(ready.size(), 5);
        QVERIFY2(t.elapsed() < 1200, qPrintable(QString::number(t.elapsed())));
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 6, 5000);
    }

    void shutdownDropsWaitingJobsAndWaitsForRunningOnes()
    {
        ControlledDecoder dec;
        dec.sleepMs->store(300);
        auto s = std::make_unique<ThumbnailScheduler>(thumbs(), dec.decoder());
        int delivered = 0;
        connect(s.get(), &ThumbnailScheduler::thumbnailReady, this, [&] { ++delivered; });
        const QString shot = writeImage(file("base.png"), 64, 64, Qt::red);
        for (int i = 0; i < 20; ++i) {
            const QString copy = file(QStringLiteral("s%1.png").arg(i));
            QVERIFY(QFile::copy(shot, copy));
            QVERIFY(s->request(i, copy, QStringLiteral("screenshot")));
        }
        QTRY_COMPARE(dec.running->load(), ThumbnailScheduler::kMaxWorkers);
        QElapsedTimer t;
        t.start();
        s.reset();
        const qint64 ms = t.elapsed();
        QCOMPARE(dec.running->load(), 0);           // nothing left running
        QCOMPARE(dec.calls->load(), ThumbnailScheduler::kMaxWorkers);   // waiting ones dropped
        QVERIFY2(ms < 1000, qPrintable(QString::number(ms)));
        QTest::qWait(50);
        QCOMPARE(delivered, 0);                     // no callback into the dead scheduler
    }

    void aDecodeOutlastingTheGraceIsFinishedNotAbandoned()
    {
        // t41: destruction waits for a running decode however long it takes
        // and logs the dependency holding the exit; its file is complete.
        ControlledDecoder dec;
        dec.sleepMs->store(2400);   // longer than the 2 s shutdown grace
        auto s = std::make_unique<ThumbnailScheduler>(thumbs(), dec.decoder());
        const QString shot = writeImage(file("slow-exit.png"), 640, 360, Qt::red);
        QVERIFY(s->request(1, shot, QStringLiteral("screenshot")));
        QTRY_COMPARE(dec.running->load(), 1);
        QTest::ignoreMessage(QtWarningMsg,
                             QRegularExpression("Shutdown: thumbnail decodes still running after"));
        QTest::ignoreMessage(QtWarningMsg,
                             QRegularExpression("Shutdown: thumbnail decodes finished after"));
        QElapsedTimer t;
        t.start();
        s.reset();
        QVERIFY(t.elapsed() >= 2000);
        QCOMPARE(dec.running->load(), 0);
        QVERIFY(ThumbnailService::isUsableThumbnail(
            ThumbnailService::ensureThumbnail(shot, QStringLiteral("screenshot"), thumbs())));
        QCOMPARE(QDir(thumbs()).entryList(QDir::Files).size(), 1);   // written once, by the decode
    }

    // ---- CaptureLibraryService: commit first, preview later ----------------
    void aScreenshotIsIndexedBeforeItsThumbnailExists()
    {
        {
            CaptureDatabase db(file("gamehq.db"));
            QVERIFY(db.open());
            GalleryModel gallery(&db);
            ControlledDecoder dec;
            dec.hold->store(true);
            CaptureLibraryService service(&db, &gallery, nullptr, CaptureFileOps::systemOps(),
                                          std::make_unique<ThumbnailScheduler>(thumbs(), dec.decoder()));
            const QString shot = writeImage(file("4k.png"), 3840, 2160, Qt::darkCyan);
            QElapsedTimer t;
            t.start();
            QCOMPARE(service.commitCapture(shot, QStringLiteral("screenshot"), QStringLiteral("Game")),
                     CaptureCommitOutcome::Indexed);
            qInfo() << "4K commitCapture returned in" << t.elapsed() << "ms with the thumbnail pending";
            // What the old commit path did on this thread before returning.
            // Noisy pixels, so the PNG decodes like a real frame, not a flat fill.
            QImage noisy(3840, 2160, QImage::Format_RGB32);
            quint32 seed = 12345;
            for (int y = 0; y < noisy.height(); ++y) {
                auto* line = reinterpret_cast<quint32*>(noisy.scanLine(y));
                for (int x = 0; x < noisy.width(); ++x)
                    line[x] = 0xff000000u | ((seed = seed * 1664525u + 1013904223u) >> 8);
            }
            const QString other = file("4k-sync.png");
            QVERIFY(noisy.save(other));
            t.restart();
            QVERIFY(!ThumbnailService::ensureThumbnail(other, QStringLiteral("screenshot"), thumbs()).isEmpty());
            qInfo() << "the synchronous 4K thumbnail it no longer waits for took" << t.elapsed() << "ms";
            QCOMPARE(gallery.rowCount(), 1);
            const QModelIndex row = gallery.index(0);
            QVERIFY(gallery.data(row, GalleryModel::ThumbnailRole).toString().isEmpty());   // placeholder
            QSignalSpy changed(&gallery, &QAbstractItemModel::dataChanged);
            QSignalSpy reset(&gallery, &QAbstractItemModel::modelReset);
            dec.hold->store(false);
            QTRY_VERIFY_WITH_TIMEOUT(
                !gallery.data(row, GalleryModel::ThumbnailRole).toString().isEmpty(), 10000);
            QCOMPARE(reset.size(), 0);   // only the affected row changed
            QCOMPARE(changed.size(), 1);
            QVERIFY(!db.thumbnailForCapture(shot).isEmpty());
        }
        QSqlDatabase::removeDatabase(QStringLiteral("gamehq"));
    }

    void aFailedThumbnailLeavesAValidCapture()
    {
        {
            CaptureDatabase db(file("gamehq.db"));
            QVERIFY(db.open());
            GalleryModel gallery(&db);
            CaptureLibraryService service(&db, &gallery, nullptr, CaptureFileOps::systemOps(),
                                          std::make_unique<ThumbnailScheduler>(thumbs()));
            const QString junk = file("broken.png");
            QFile f(junk);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("not an image");
            f.close();
            QSignalSpy failed(service.thumbnails(), &ThumbnailScheduler::thumbnailFailed);
            QCOMPARE(service.commitCapture(junk, QStringLiteral("screenshot"), QStringLiteral("Game")),
                     CaptureCommitOutcome::Indexed);
            QTRY_COMPARE(failed.size(), 1);
            QVERIFY(db.hasCapture(junk));
            QCOMPARE(gallery.rowCount(), 1);
            QVERIFY(db.thumbnailForCapture(junk).isEmpty());
        }
        QSqlDatabase::removeDatabase(QStringLiteral("gamehq"));
    }

    void aCaptureDeletedBeforeItsThumbnailGetsNone()
    {
        {
            CaptureDatabase db(file("gamehq.db"));
            QVERIFY(db.open());
            GalleryModel gallery(&db);
            ControlledDecoder dec;
            dec.hold->store(true);
            // Deleted in the library while the preview is being made. The
            // trash step is a stand-in that keeps the file, so the decode
            // still succeeds and only the library can refuse its result.
            CaptureFileOps ops = CaptureFileOps::systemOps();
            ops.trash = [](const QString&) { return true; };
            ops.remove = [](const QString&) { return true; };
            CaptureLibraryService service(&db, &gallery, nullptr, ops,
                                          std::make_unique<ThumbnailScheduler>(thumbs(), dec.decoder()));
            const QString shot = writeImage(file("gone.png"), 64, 64, Qt::red);
            QSignalSpy ready(service.thumbnails(), &ThumbnailScheduler::thumbnailReady);
            QCOMPARE(service.commitCapture(shot, QStringLiteral("screenshot"), QStringLiteral("Game")),
                     CaptureCommitOutcome::Indexed);
            QCOMPARE(service.deleteCapture(&gallery, 0).removed, 1);
            dec.hold->store(false);
            QTRY_COMPARE(ready.size(), 1);
            QTest::qWait(20);
            QCOMPARE(gallery.rowCount(), 0);
            // The tombstoned row itself (the library's queries hide it).
            QSqlQuery q(QSqlDatabase::database(QStringLiteral("gamehq")));
            QVERIFY(q.exec(QStringLiteral("SELECT thumbnail_path, deleted_at FROM captures")));
            QVERIFY(q.next());
            QVERIFY(!q.value(1).isNull());
            QVERIFY2(q.value(0).toString().isEmpty(), qPrintable(q.value(0).toString()));
        }
        QSqlDatabase::removeDatabase(QStringLiteral("gamehq"));
    }
};

QTEST_MAIN(TestThumbnailScheduler)
#include "tst_thumbnailscheduler.moc"
