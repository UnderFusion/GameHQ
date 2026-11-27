#include "capture/FramePumpService.h"
#include "capture/CapturePublisher.h"
#include "capture/ReplayExporter.h"
#include "storage/ThumbnailService.h"
#include <QDir>
#include <QFile>
#include <QImage>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <objbase.h>

class TestReplayThumbnail : public QObject
{
    Q_OBJECT
private slots:
    void unalignedClipDecodesWholeClient()
    {
        QVERIFY(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)));
        const auto apartment = qScopeGuard([] { CoUninitialize(); });
        QImage frame;
        QVERIFY(ReplayExporter::grabThumbnail(QStringLiteral(REPLAY_CLIENT_FIXTURE), frame));
        QCOMPARE(frame.size(), QSize(356, 196));
        const QColor blue = frame.pixelColor(170, 100);
        const QColor orange = frame.pixelColor(270, 20);
        QVERIFY(blue.blue() > 170 && blue.red() < 60);
        QVERIFY(orange.red() > 170 && orange.blue() < 80);
    }

    void shortExportAcceptsCompleteMediaAndRejectsInvalidInputs()
    {
        QTemporaryDir dir(QCoreApplication::applicationDirPath() + "/short-export-XXXXXX");
        QVERIFY(dir.isValid());
        QVERIFY(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)));
        const auto apartment = qScopeGuard([] { CoUninitialize(); });
        const QString segment = QStringLiteral(REPLAY_CLIENT_FIXTURE);
        const QString clip = dir.filePath(QStringLiteral("complete.mp4"));
        QVERIFY(ReplayExporter::concat({segment}, clip, QStringLiteral("short-complete")));
        QImage frame;
        QVERIFY(ReplayExporter::grabThumbnail(clip, frame));
        QCOMPARE(frame.size(), QSize(356, 196));
        QVERIFY(frame.pixelColor(270, 20).red() > 170);
        QVERIFY(frame.pixelColor(170, 100).blue() > 170);

        const QString broken = dir.filePath(QStringLiteral("corrupt.mp4"));
        QFile corrupt(broken);
        QVERIFY(corrupt.open(QIODevice::WriteOnly));
        QCOMPARE(corrupt.write("bad media"), 9);
        corrupt.close();
        QVERIFY(!ReplayExporter::concat({broken}, dir.filePath(QStringLiteral("corrupt-out.mp4")),
                                        QStringLiteral("short-corrupt")));
        // A readable first segment must not make an export with a missing
        // second segment or missing required audio succeed.
        QVERIFY(!ReplayExporter::concat({segment, dir.filePath(QStringLiteral("missing.mp4"))},
                                        dir.filePath(QStringLiteral("incomplete.mp4")),
                                        QStringLiteral("short-incomplete")));
        QVERIFY(!ReplayExporter::concat({segment}, dir.filePath(QStringLiteral("no-audio.mp4")),
                                        QStringLiteral("short-missing-audio"), ReplayExporter::Audio::Required));
    }

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
        const QString thumb = ThumbnailService::cachePath(clip.finalPath, thumbs);

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
                              "Fixture game", "fixture.exe", "thumbnail-test", 17, false);
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
        QCOMPARE(result.at(1).toString(), QStringLiteral("Fixture game"));
        QCOMPARE(result.at(2).toString(), thumb);
        QCOMPARE(result.at(3).toString(), QStringLiteral("fixture.exe"));
        QVERIFY(QFileInfo::exists(clip.finalPath));
        QVERIFY(!QFileInfo::exists(clip.pendingPath));
        const QImage image(thumb);
        QVERIFY(!image.isNull());
        QCOMPARE(image.size(), QSize(64, 64));
        QVERIFY(image.pixelColor(32, 32).blue() > 200);
        // This is the gallery/scanner reattachment path, using the published
        // clip name. It must select the exact completed companion image.
        QCOMPARE(ThumbnailService::ensureThumbnail(clip.finalPath, "video", thumbs), thumb);
        // A clip from another game can have the same basename and timestamp.
        // The shared cache and scanner must keep the two previews distinct.
        QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("Other game"))));
        const QString other = dir.filePath(QStringLiteral("Other game/same-second_2.mp4"));
        QVERIFY(QFile::copy(QStringLiteral(REPLAY_CLIENT_FIXTURE), other));
        const QString otherThumb = ThumbnailService::ensureThumbnail(other, "video", thumbs);
        QCOMPARE(otherThumb, ThumbnailService::cachePath(other, thumbs));
        QVERIFY(otherThumb != thumb);
        const QImage otherImage(otherThumb);
        QCOMPARE(otherImage.size(), QSize(356, 196));
        QVERIFY(otherImage.pixelColor(270, 20).red() > 170);
        QVERIFY(QImage(thumb).pixelColor(32, 32).blue() > 200);
        sentinel.setFileName(first.finalPath);
        QVERIFY(sentinel.open(QIODevice::ReadOnly));
        QCOMPARE(sentinel.readAll(), QByteArray("existing clip"));

        const auto rejected = CapturePublisher::reserve(dir.path(), "'changed-after-finalize'", ".mp4");
        QVERIFY(rejected.isValid());
        const qint64 finalizedSize = QFileInfo(segment).size();
        QFile shortened(segment);
        QVERIFY(shortened.open(QIODevice::ReadWrite));
        QVERIFY(shortened.resize(shortened.size() - 1));
        shortened.close();
        SegmentLease changed({segment}, {{segment, finalizedSize}});
        QVERIFY(QMetaObject::invokeMethod(worker, [worker, rejected, lease = std::move(changed)]() mutable {
            worker->runExport(std::move(lease), rejected, {}, "Fixture game", "fixture.exe",
                              "changed-segment", 18, false);
        }, Qt::QueuedConnection));
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 5000);
        QVERIFY(saved.isEmpty());
        QVERIFY(!QFileInfo::exists(rejected.finalPath));
        QVERIFY(!QFileInfo::exists(rejected.pendingPath));
    }

    void replacedScreenshotRegeneratesItsThumbnail()
    {
        QTemporaryDir dir(QCoreApplication::applicationDirPath() + "/thumbnail-XXXXXX");
        QVERIFY(dir.isValid());
        const QString shot = dir.path() + "/shot.png";
        const QString thumbs = dir.path() + "/thumbs";
        QImage red(64, 64, QImage::Format_RGB32);
        red.fill(Qt::red);
        QVERIFY(red.save(shot));
        const QString thumb = ThumbnailService::ensureThumbnail(shot, "screenshot", thumbs);
        QVERIFY(!thumb.isEmpty());
        QVERIFY(QImage(thumb).pixelColor(32, 32).red() > 200);
        QVERIFY(ThumbnailService::isCurrentThumbnail(thumb, shot, "screenshot"));

        // Same path, new picture, newer than the cached preview.
        QImage blue(64, 64, QImage::Format_RGB32);
        blue.fill(Qt::blue);
        QVERIFY(blue.save(shot));
        QFile thumbFile(thumb);
        QVERIFY(thumbFile.open(QIODevice::ReadWrite));
        QVERIFY(thumbFile.setFileTime(QDateTime::currentDateTime().addSecs(-60),
                                      QFileDevice::FileModificationTime));
        thumbFile.close();
        QVERIFY(!ThumbnailService::isCurrentThumbnail(thumb, shot, "screenshot"));

        QCOMPARE(ThumbnailService::ensureThumbnail(shot, "screenshot", thumbs), thumb);
        QVERIFY(QImage(thumb).pixelColor(32, 32).blue() > 200);
        QVERIFY(ThumbnailService::isCurrentThumbnail(thumb, shot, "screenshot"));
    }
};
QTEST_GUILESS_MAIN(TestReplayThumbnail)
#include "tst_replaythumbnail.moc"
