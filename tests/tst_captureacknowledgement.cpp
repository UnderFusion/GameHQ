#include "capture/FramePumpService.h"
#include "capture/ScreenshotService.h"
#include <QtTest>
#include "config/ConfigManager.h"
#include "config/CaptureLocations.h"
#include "config/ConfigKeys.h"
#include <QImage>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <algorithm>

class CaptureAcknowledgementTest : public QObject
{
    Q_OBJECT
private slots:
    void concurrentScreenshotEncodesRetainTheirOwnIds()
    {
        QTemporaryDir dir(QCoreApplication::applicationDirPath() + "/ids-XXXXXX");
        QVERIFY(dir.isValid());
        ConfigManager config(dir.path() + "/config.json");
        config.setValue(ConfigKeys::StorageScreenshotsRoot, dir.path());
        CaptureLocations locations(&config);
        ScreenshotService shots(&config, &locations);
        QSignalSpy saved(&shots, &ScreenshotService::captured);
        QSignalSpy failed(&shots, &ScreenshotService::failed);
        QImage red(32, 32, QImage::Format_RGB32); red.fill(Qt::red);
        QImage blue(64, 64, QImage::Format_RGB32); blue.fill(Qt::blue);
        shots.saveImage(red, "Fixture", {}, 101);
        shots.saveImage(blue, "Fixture", {}, 202);
        QTRY_COMPARE_WITH_TIMEOUT(saved.count(), 2, 5000);
        QCOMPARE(failed.count(), 0);
        QSet<quint64> ids;
        for (const auto& result : saved) {
            const quint64 id = result.at(3).toULongLong();
            ids.insert(id);
            const QImage image(result.at(0).toString());
            QVERIFY(!image.isNull());
            QCOMPARE(image.size(), id == 101 ? QSize(32, 32) : QSize(64, 64));
        }
        QCOMPARE(ids, QSet<quint64>({101, 202}));
    }

    void receiptPrecedesRejection_data()
    {
        QTest::addColumn<bool>("replay");
        QTest::addColumn<CaptureRequest::Source>("source");
        for (bool replay : {false, true}) {
            for (auto source : {CaptureRequest::Source::Controller, CaptureRequest::Source::Keyboard}) {
                const auto name = (replay ? QByteArray("replay-") : QByteArray("screenshot-"))
                    + CaptureRequest::label(source).toUtf8();
                QTest::newRow(name.constData()) << replay << source;
            }
        }
    }

    void receiptPrecedesRejection()
    {
        QFETCH(bool, replay);
        QFETCH(CaptureRequest::Source, source);
        FramePumpService clips(nullptr, nullptr);
        ScreenshotService screenshots(nullptr, nullptr);
        // Deterministic gate: no foreground game, recording, or real input needed.
        clips.prepareForUpdate();
        screenshots.prepareForUpdate();
        QStringList outcomes;
        QList<qint64> latency;
        quint64 expectedId = 0;
        const auto accepted = [&](const CaptureRequest& request, CaptureRequest::Kind kind) {
            QCOMPARE(request.id, expectedId);
            QCOMPARE(request.source, source);
            QCOMPARE(kind, replay ? CaptureRequest::Kind::Replay : CaptureRequest::Kind::Screenshot);
            QCOMPARE(QThread::currentThread(), clips.thread());
            outcomes.append("accepted");
            latency.append(request.ageMs());
        };
        connect(&clips, &FramePumpService::requestAccepted, this, accepted);
        connect(&screenshots, &ScreenshotService::requestAccepted, this, accepted);
        connect(&clips, &FramePumpService::clipFailed, this,
                [&](const QString&, const QString&, quint64 id) { QCOMPARE(id, expectedId); outcomes.append("rejected"); });
        connect(&screenshots, &ScreenshotService::skipped, this,
                [&](const QString&, quint64 id) { QCOMPARE(id, expectedId); outcomes.append("rejected"); });
        for (int i = 0; i < 100; ++i) {
            outcomes.clear();
            const auto request = CaptureRequest::create(source);
            expectedId = request.id;
            // Include the event-queue delivery used by cross-thread input dispatch.
            QMetaObject::invokeMethod(this, [&, request] {
                if (replay) clips.saveReplay(request);
                else screenshots.capture(request);
            }, Qt::QueuedConnection);
            QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
            QCOMPARE(outcomes, QStringList({"accepted", "rejected"}));
        }
        QCOMPARE(latency.size(), 100);
        std::sort(latency.begin(), latency.end());
        qInfo() << "request-to-acknowledgement p95 ms:" << latency.at(94);
        QVERIFY(latency.first() >= 0);
        QVERIFY2(latency.at(94) <= 100, "Request-to-acknowledgement p95 exceeded 100 ms");
    }
};
QTEST_GUILESS_MAIN(CaptureAcknowledgementTest)
#include "tst_captureacknowledgement.moc"
