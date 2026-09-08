#include "capture/CapturePublisher.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>

#include <atomic>

// A screenshot used to be named with an exists()-then-write test and encoded
// straight to that final name. Two encoder threads finishing inside the same
// second could therefore be handed the same path, and any interrupted encode
// left a truncated image sitting where the scanner indexes captures.
class TestCapturePublisher : public QObject
{
    Q_OBJECT

private slots:
    void parallelReservationsNeverShareAName()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        // More contenders than the two real encoder threads, all inside one
        // timestamp tick, which is exactly the case the old check lost. Keep
        // this bounded: Windows CI runners may refuse a 32-thread burst before
        // the test reaches the code it is meant to exercise.
        constexpr int kThreads = 8;
        QVector<CapturePublisher::Reservation> results(kThreads);
        std::atomic_int ready{0};
        QThreadPool pool;
        pool.setMaxThreadCount(kThreads);
        for (int i = 0; i < kThreads; ++i) {
            pool.start([&, i]() {
                // Line the workers up so they reserve at the same moment, but
                // never wait forever if the pool hands out fewer threads.
                ++ready;
                QElapsedTimer waited;
                waited.start();
                while (ready.load() < kThreads && waited.elapsed() < 2000) { }
                results[i] = CapturePublisher::reserve(dir.path(),
                                                       QStringLiteral("yyyy-MM-dd_HH-mm-ss"),
                                                       QStringLiteral(".png"));
            });
        }
        QVERIFY(pool.waitForDone(30000));

        QSet<QString> finals;
        QSet<QString> pendings;
        for (const CapturePublisher::Reservation& reservation : results) {
            QVERIFY(reservation.isValid());
            QVERIFY(QFileInfo::exists(reservation.pendingPath));
            finals.insert(reservation.finalPath);
            pendings.insert(reservation.pendingPath);
        }
        QCOMPARE(finals.size(), qsizetype(kThreads));
        QCOMPARE(pendings.size(), qsizetype(kThreads));
    }

    void anUnfinishedCaptureIsNeverVisibleUnderItsFinalName()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const CapturePublisher::Reservation reservation = CapturePublisher::reserve(
            dir.path(), QStringLiteral("yyyy-MM-dd_HH-mm-ss"), QStringLiteral(".png"));
        QVERIFY(reservation.isValid());
        // Half-written: bytes on disk, but nothing under the final name.
        {
            QFile pending(reservation.pendingPath);
            QVERIFY(pending.open(QIODevice::WriteOnly));
            QCOMPARE(pending.write("partial-image-bytes"), 19);
        }
        QVERIFY(!QFileInfo::exists(reservation.finalPath));
        // And the reserved name really is reserved.
        QVERIFY(reservation.pendingPath.endsWith(CapturePublisher::kPendingSuffix));

        QString error;
        QVERIFY2(CapturePublisher::publish(reservation, &error), qPrintable(error));
        QVERIFY(QFileInfo::exists(reservation.finalPath));
        QVERIFY(!QFileInfo::exists(reservation.pendingPath));
        QCOMPARE(QFileInfo(reservation.finalPath).size(), qint64(19));
    }

    void aFailedEncodeLeavesNoCaptureBehind()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const CapturePublisher::Reservation reservation = CapturePublisher::reserve(
            dir.path(), QStringLiteral("yyyy-MM-dd_HH-mm-ss"), QStringLiteral(".png"));
        QVERIFY(reservation.isValid());
        CapturePublisher::discard(reservation);
        QVERIFY(!QFileInfo::exists(reservation.pendingPath));
        QVERIFY(!QFileInfo::exists(reservation.finalPath));
        // The name is free again, so the next capture reuses it rather than
        // leaving a gap in the numbering.
        const CapturePublisher::Reservation again = CapturePublisher::reserve(
            dir.path(), QStringLiteral("yyyy-MM-dd_HH-mm-ss"), QStringLiteral(".png"));
        QCOMPARE(again.finalPath, reservation.finalPath);
    }

    void neverReservesANameACaptureAlreadyOccupies()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const CapturePublisher::Reservation first = CapturePublisher::reserve(
            dir.path(), QStringLiteral("yyyy-MM-dd_HH-mm-ss"), QStringLiteral(".png"));
        QVERIFY(first.isValid());
        QVERIFY(CapturePublisher::publish(first));

        const CapturePublisher::Reservation second = CapturePublisher::reserve(
            dir.path(), QStringLiteral("yyyy-MM-dd_HH-mm-ss"), QStringLiteral(".png"));
        QVERIFY(second.isValid());
        QVERIFY(second.finalPath != first.finalPath);
        QVERIFY(QFileInfo::exists(first.finalPath));
    }

    // The replay path used to name a clip with a bare timestamp, delete
    // whatever held that name and rename its .partial on top. Two saves inside
    // one second therefore left one clip where there should have been two.
    void twoClipSavesInsideOneSecondBothSurvive()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const auto exportClip = [&dir](const char* bytes) {
            const CapturePublisher::Reservation reservation = CapturePublisher::reserve(
                dir.path(), QStringLiteral("yyyy-MM-dd_HH-mm-ss"), QStringLiteral(".mp4"));
            {
                QFile pending(reservation.pendingPath);
                pending.open(QIODevice::WriteOnly);
                pending.write(bytes);
            }
            return reservation;
        };

        const CapturePublisher::Reservation first = exportClip("first-clip");
        const CapturePublisher::Reservation second = exportClip("second-clip");
        QVERIFY(first.isValid());
        QVERIFY(second.isValid());
        QVERIFY(first.finalPath != second.finalPath);
        QVERIFY(CapturePublisher::publish(first));
        QVERIFY(CapturePublisher::publish(second));

        QCOMPARE(QDir(dir.path()).entryList({ QStringLiteral("*.mp4") }, QDir::Files).size(),
                 qsizetype(2));
        QFile older(first.finalPath);
        QVERIFY(older.open(QIODevice::ReadOnly));
        QCOMPARE(older.readAll(), QByteArray("first-clip"));   // untouched by the later save
    }

    // Every failure path gives the reservation back and nothing else. The old
    // code removed the final path too, so a failed export could delete a clip
    // saved earlier that happened to carry the same timestamp.
    void aFailedExportNeverDeletesAnExistingClip()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const CapturePublisher::Reservation saved = CapturePublisher::reserve(
            dir.path(), QStringLiteral("yyyy-MM-dd_HH-mm-ss"), QStringLiteral(".mp4"));
        {
            QFile pending(saved.pendingPath);
            QVERIFY(pending.open(QIODevice::WriteOnly));
            pending.write("a clip the user already has");
        }
        QVERIFY(CapturePublisher::publish(saved));

        const CapturePublisher::Reservation failing = CapturePublisher::reserve(
            dir.path(), QStringLiteral("yyyy-MM-dd_HH-mm-ss"), QStringLiteral(".mp4"));
        QVERIFY(failing.isValid());
        CapturePublisher::discard(failing);

        QVERIFY(!QFileInfo::exists(failing.finalPath));
        QVERIFY(!QFileInfo::exists(failing.pendingPath));
        QFile survivor(saved.finalPath);
        QVERIFY(survivor.open(QIODevice::ReadOnly));
        QCOMPARE(survivor.readAll(), QByteArray("a clip the user already has"));
    }

    // A clip and its preview must stay a pair: the preview inherits the _2 the
    // reservation had to add, so the second save cannot overwrite the first
    // clip's thumbnail either.
    void aThumbnailFollowsTheNameItsClipWasGiven()
    {
        QTemporaryDir clips;
        QTemporaryDir thumbs;
        QVERIFY(clips.isValid() && thumbs.isValid());

        const CapturePublisher::Reservation first = CapturePublisher::reserve(
            clips.path(), QStringLiteral("yyyy-MM-dd_HH-mm-ss"), QStringLiteral(".mp4"));
        const CapturePublisher::Reservation second = CapturePublisher::reserve(
            clips.path(), QStringLiteral("yyyy-MM-dd_HH-mm-ss"), QStringLiteral(".mp4"));
        const QString firstThumb = CapturePublisher::companionPath(
            first.finalPath, thumbs.path(), QStringLiteral("_clip.png"));
        const QString secondThumb = CapturePublisher::companionPath(
            second.finalPath, thumbs.path(), QStringLiteral("_clip.png"));

        QVERIFY(firstThumb != secondThumb);
        // ThumbnailService reattaches a preview by "<clip base>_clip.png", so
        // the companion has to keep exactly that shape.
        QCOMPARE(QFileInfo(secondThumb).fileName(),
                 QFileInfo(second.finalPath).completeBaseName() + QStringLiteral("_clip.png"));
        QVERIFY(secondThumb.endsWith(QStringLiteral("_2_clip.png")));
    }

    void sweepRemovesOnlyAbandonedFiles()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("Some Game/Screenshots")));
        const QString nested = dir.path() + QStringLiteral("/Some Game/Screenshots");

        const CapturePublisher::Reservation running = CapturePublisher::reserve(
            nested, QStringLiteral("yyyy-MM-dd_HH-mm-ss"), QStringLiteral(".png"));
        QVERIFY(running.isValid());

        // A leftover from a crash: same shape, but old.
        const QString abandoned = nested + QStringLiteral("/2020-01-01_00-00-00.png")
            + CapturePublisher::kPendingSuffix;
        {
            QFile file(abandoned);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("stale");
            // setFileTime needs the handle, so age it before closing.
            QVERIFY(file.setFileTime(QDateTime::currentDateTime().addSecs(-3600),
                                     QFileDevice::FileModificationTime));
        }

        // A real capture next to them must not be touched.
        const QString keeper = nested + QStringLiteral("/keep-me.png");
        {
            QFile file(keeper);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("image");
        }

        QCOMPARE(CapturePublisher::sweepStale(dir.path(), 600), 1);
        QVERIFY(!QFileInfo::exists(abandoned));
        QVERIFY(QFileInfo::exists(running.pendingPath));   // too young to be abandoned
        QVERIFY(QFileInfo::exists(keeper));
    }
};

QTEST_GUILESS_MAIN(TestCapturePublisher)
#include "tst_capturepublisher.moc"
