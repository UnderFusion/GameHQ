#include "capture/FramePumpService.h"
#include "capture/CapturePublisher.h"
#include "storage/ThumbnailService.h"
#include <QDir>
#include <QFile>
#include <QImage>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

class TestReplayThumbnail : public QObject
{
    Q_OBJECT
private slots:
    void exportHandsBackWorkerAndPublishesMatchingThumbnail()
    {
        QTemporaryDir dir(QCoreApplication::applicationDirPath() + "/thumbnail-XXXXXX");
        QVERIFY(dir.isValid());
        const QString segment = dir.path() + "/segment.mp4";
        QVERIFY(QFile::copy(QStringLiteral(REPLAY_THUMB_FIXTURE), segment));
        // An existing clip claims the unsuffixed identity; it must survive.
        const auto first = CapturePublisher::reserve(dir.path(), "'same-second'", ".mp4");
        QVERIFY(first.isValid());
        QFile sentinel(first.pendingPath);
        QVERIFY(sentinel.open(QIODevice::WriteOnly));
        QCOMPARE(sentinel.write("existing clip"), qint64(13));
        sentinel.close();
        QVERIFY(CapturePublisher::publish(first));
        const auto clip = CapturePublisher::reserve(dir.path(), "'same-second'", ".mp4");
        QVERIFY(clip.isValid());
        QVERIFY(clip.finalPath.endsWith("same-second_2.mp4"));
        const QString thumbs = dir.path() + "/thumbnails";
        const QString thumb = CapturePublisher::companionPath(clip.finalPath, thumbs, "_clip.png");
        QVERIFY(thumb.endsWith("same-second_2_clip.png"));

        QThread captureThread;
        auto* worker = new FramePumpWorker;
        worker->moveToThread(&captureThread);
        connect(&captureThread, &QThread::finished, worker, &QObject::deleteLater);
        QSignalSpy saved(worker, &FramePumpWorker::clipSaved);
        QSignalSpy failed(worker, &FramePumpWorker::clipFailed);
        QSemaphore started;
        connect(&captureThread, &QThread::started, worker, [&] { started.release(); }, Qt::DirectConnection);
        QSemaphore returned;
        qint64 elapsed = -1;
        QElapsedTimer timer;
        captureThread.start();
        // Join before destroying anything referenced by queued test callbacks,
        // including on an assertion or timeout exit.
        const auto cleanup = qScopeGuard([&] { captureThread.quit(); captureThread.wait(); });
        QVERIFY(started.tryAcquire(1, 2000)); // exclude creating the test thread
        timer.start();
        // Exercise the real production export handoff with real MP4 decode,
        // scaling, PNG encoding and publication, not an empty callback.
        QVERIFY(QMetaObject::invokeMethod(worker, [&, worker, clip, thumb, segment, timer] {
            worker->runExport(SegmentLease({segment}), clip, thumb,
                              "Fixture game", "fixture.exe", "thumbnail-test", 17);
            QMetaObject::invokeMethod(worker, [&, timer] {
                elapsed = timer.elapsed();
                returned.release();
            }, Qt::QueuedConnection);
        }, Qt::QueuedConnection));
        QVERIFY(returned.tryAcquire(1, 2000));
        qInfo() << "capture worker export-handoff return elapsedMs=" << elapsed;
        QVERIFY2(elapsed >= 0 && elapsed <= 20, "Thumbnail export blocked capture worker beyond 20 ms");
        QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 1, 10000);
        QCOMPARE(failed.count(), 0);
        const auto result = saved.takeFirst();
        QCOMPARE(result.at(0).toString(), clip.finalPath);
        QCOMPARE(result.at(2).toString(), thumb);
        QVERIFY(QFileInfo::exists(clip.finalPath));
        QVERIFY(!QFileInfo::exists(clip.pendingPath));
        const QImage image(thumb);
        QVERIFY(!image.isNull());
        QCOMPARE(image.size(), QSize(64, 64));
        QVERIFY(image.pixelColor(32, 32).blue() > 200);
        // This is the gallery/scanner reattachment path, using the published
        // clip name. It must select the exact completed companion image.
        QCOMPARE(ThumbnailService::ensureThumbnail(clip.finalPath, "video", thumbs), thumb);
        sentinel.setFileName(first.finalPath);
        QVERIFY(sentinel.open(QIODevice::ReadOnly));
        QCOMPARE(sentinel.readAll(), QByteArray("existing clip"));
    }
};
QTEST_GUILESS_MAIN(TestReplayThumbnail)
#include "tst_replaythumbnail.moc"
