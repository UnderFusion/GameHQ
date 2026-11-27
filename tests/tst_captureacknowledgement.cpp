#include "capture/FramePumpService.h"
#include "capture/FramePumpPipeline.h"
#include "capture/ScreenshotService.h"
#include <QtTest>
#include "config/ConfigManager.h"
#include "config/CaptureLocations.h"
#include "config/ConfigKeys.h"
#include "integration/ExternalGameContext.h"
#include <QDirIterator>
#include <QImage>
#include <QSignalSpy>
#include <QSemaphore>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <windows.h>
#include <algorithm>

class CaptureAcknowledgementTest : public QObject
{
    Q_OBJECT
private slots:
    void busyFramePoolYieldsToQueuedCaptureControls()
    {
        // Windows can supply a new frame as soon as the last one is released.
        // A finite safety limit makes the old drain-until-empty policy fail
        // this test without hanging the test runner.
        struct Pool final : IDirect3D11CaptureFramePool, IDirect3D11CaptureFrame {
            int remaining = 64;
            HRESULT STDMETHODCALLTYPE TryGetNextFrame(IDirect3D11CaptureFrame** frame) override {
                *frame = remaining ? static_cast<IDirect3D11CaptureFrame*>(this) : nullptr;
                if (remaining)
                    --remaining;
                return S_OK;
            }
            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
            ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
            ULONG STDMETHODCALLTYPE Release() override { return 1; }
            HRESULT STDMETHODCALLTYPE GetIids(ULONG*, IID**) override { return E_NOTIMPL; }
            HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING*) override { return E_NOTIMPL; }
            HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel*) override { return E_NOTIMPL; }
            HRESULT STDMETHODCALLTYPE get_Surface(IDirect3DSurface**) override { return E_NOTIMPL; }
            HRESULT STDMETHODCALLTYPE get_SystemRelativeTime(WgcTimeSpan*) override { return E_NOTIMPL; }
            HRESULT STDMETHODCALLTYPE get_ContentSize(WgcSizeInt32*) override { return E_NOTIMPL; }
            HRESULT STDMETHODCALLTYPE Recreate(IDirect3DDevice*, INT32, INT32, WgcSizeInt32) override { return E_NOTIMPL; }
            HRESULT STDMETHODCALLTYPE add_FrameArrived(IUnknown*, EventRegistrationToken*) override { return E_NOTIMPL; }
            HRESULT STDMETHODCALLTYPE remove_FrameArrived(EventRegistrationToken) override { return E_NOTIMPL; }
            HRESULT STDMETHODCALLTYPE CreateCaptureSession(IGraphicsCaptureItem*, IGraphicsCaptureSession**) override { return E_NOTIMPL; }
            HRESULT STDMETHODCALLTYPE get_DispatcherQueue(IDispatcherQueue**) override { return E_NOTIMPL; }
        } pool;
        FramePumpWorker worker;
        worker.m_pipe = new FramePumpWorker::Pipeline;
        worker.m_pipe->framePool = &pool;
        worker.m_pipe->fpsClock.start();
        int remainingAtControl = -1;
        QMetaObject::invokeMethod(&worker, &FramePumpWorker::poll, Qt::QueuedConnection);
        QMetaObject::invokeMethod(&worker, [&] { remainingAtControl = pool.remaining; },
                                  Qt::QueuedConnection);
        QCoreApplication::sendPostedEvents(&worker, QEvent::MetaCall);
        QVERIFY(remainingAtControl > 0);
        QVERIFY(remainingAtControl < 64);
        for (int tick = 0; pool.remaining && tick < 64; ++tick)
            worker.poll();
        QCOMPARE(pool.remaining, 0);
    }

    void trayShutdownDrainsAcceptedWorkWhileGuiCanDispatch()
    {
        QSemaphore entered, guiReply;
        bool receivedReply = false;
        FramePumpService pump(nullptr, nullptr);
        QSignalSpy stopped(&pump, &FramePumpService::shutdownFinished);
        // Model a queued capture operation whose Windows/COM completion
        // requires dispatch on the owner thread. A join on that thread loses
        // the reply. The timeout also lets the test report that regression.
        QMetaObject::invokeMethod(pump.m_worker, [&] {
            entered.release();
            QMetaObject::invokeMethod(&pump, [&] { guiReply.release(); }, Qt::QueuedConnection);
            receivedReply = guiReply.tryAcquire(1, 2000);
        }, Qt::QueuedConnection);
        QVERIFY(entered.tryAcquire(1, 3000));
        pump.shutdownForExit();
        pump.shutdownForExit();
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 5000);
        QVERIFY(pump.m_thread.wait(2000));
        QVERIFY(receivedReply);
    }

    void playnitePauseAndResumePreserveTheReadyGameWindow()
    {
        HWND window = CreateWindowExW(0, L"STATIC", L"capture fixture", 0,
                                      0, 0, 32, 32, HWND_MESSAGE, nullptr, nullptr, nullptr);
        QVERIFY(window);
        const auto cleanup = qScopeGuard([&] { DestroyWindow(window); });
        ForegroundGame game;
        game.valid = game.isGame = game.hasExternalIdentity = true;
        game.hwnd = window;
        game.pid = GetCurrentProcessId();
        game.w = game.h = 32;
        game.externalSource = "playnite";
        game.externalSessionId = "launch-1";
        integration::ExternalGameContext context;
        QString error;
        const QJsonObject info{{"sessionId", game.externalSessionId},
                               {"startedProcessId", int(game.pid)}};
        QVERIFY2(context.upsert(game.externalSource, info, "started", error), qPrintable(error));
        GameDetector::setExternalContext(&context);
        const auto detach = qScopeGuard([] { GameDetector::setExternalContext(nullptr); });

        FramePumpService pump(nullptr, nullptr);
        pump.m_targetGame = game;
        pump.m_owners.setAuto(true);
        const auto generation = pump.m_buffer.requestStart("Game");
        QVERIFY(pump.m_buffer.confirm(generation, ReplayBufferState::Recording));
        QVERIFY(pump.m_buffer.confirm(generation, ReplayBufferState::Ready));
        ForegroundGame menu;
        menu.valid = menu.isExcludedProcess = menu.isPlaynitePauseMenu = true;
        for (int tick = 0; tick < 6; ++tick)
            pump.updateAutoTarget(menu);
        QVERIFY(pump.m_buffer.canSave());
        QCOMPARE(pump.m_buffer.generation(), generation);
        pump.updateAutoTarget(game);
        QCOMPARE(pump.m_targetGame.hwnd, static_cast<void*>(window));
        QCOMPARE(pump.m_buffer.generation(), generation);

        auto wrongOwner = game;
        ++wrongOwner.pid;
        QVERIFY(GameDetector::windowBelongsToProcess(window, game.pid));
        QVERIFY(!GameDetector::windowBelongsToProcess(window, wrongOwner.pid));
        QVERIFY(!GameDetector::canRetainBehindPauseMenu(menu, wrongOwner, nullptr));
        // A recovery request must consider the same paused target on every tick
        // and restart exactly once, after the game resumes.
        pump.updateAutoTarget(menu, true);
        pump.updateAutoTarget(menu);
        QCOMPARE(pump.m_buffer.generation(), generation);
        QVERIFY(pump.m_buffer.canSave());
        pump.updateAutoTarget(game);
        QCOMPARE(pump.m_buffer.generation(), generation + 1);
        pump.updateAutoTarget(game);
        QCOMPARE(pump.m_buffer.generation(), generation + 1);

        // A new Playnite launch can retain both the process and its window.
        auto relaunched = game;
        relaunched.externalSessionId = "launch-2";
        QVERIFY(!GameDetector::sameCaptureTarget(relaunched, game));
        pump.updateAutoTarget(relaunched);
        QCOMPARE(pump.m_buffer.generation(), generation + 2);
        QCOMPARE(pump.m_targetGame.externalSessionId, relaunched.externalSessionId);
        ForegroundGame desktop;
        QVERIFY(!GameDetector::canRetainBehindPauseMenu(desktop, game, nullptr));
        QVERIFY(context.remove(game.externalSource, game.externalSessionId));
        QVERIFY(!GameDetector::canRetainBehindPauseMenu(menu, game, nullptr));
        pump.updateAutoTarget(menu);
        pump.updateAutoTarget(menu);
        QCOMPARE(pump.m_buffer.state(), ReplayBufferState::Stopped);
    }

    void hdrRequestForAWindowThatLostFocusCannotArmCapture()
    {
        HWND window = CreateWindowExW(0, L"STATIC", L"HDR request fixture", 0,
                                      0, 0, 32, 32, HWND_MESSAGE, nullptr, nullptr, nullptr);
        QVERIFY(window);
        const auto cleanup = qScopeGuard([&] { DestroyWindow(window); });
        ForegroundGame requested;
        requested.valid = requested.isGame = true;
        requested.hwnd = window;
        requested.pid = GetCurrentProcessId();
        requested.w = requested.h = 32;
        FramePumpService pump(nullptr, nullptr);
        QSignalSpy failed(&pump, &FramePumpService::hdrScreenshotFailed);
        const auto generation = pump.m_buffer.generation();
        pump.captureHdrScreenshot(requested, 123);
        QCOMPARE(failed.size(), 1);
        QCOMPARE(failed.first().at(1).toULongLong(), 123ULL);
        QCOMPARE(pump.m_buffer.generation(), generation);
        QVERIFY(!pump.m_owners.needsSession());
    }

    void removingAnAllowedGameStopsAutoAndColdManualCapture()
    {
        QTemporaryDir dir(QCoreApplication::applicationDirPath() + "/policy-XXXXXX");
        QVERIFY(dir.isValid());
        ConfigManager config(dir.filePath("config.json"));
        config.setValue(ConfigKeys::CaptureMode, QStringLiteral("whitelist"));
        const auto exe = QCoreApplication::applicationFilePath();
        QVERIFY(config.setCaptureExecutableAllowed(exe, true));
        FramePumpService pump(&config, nullptr);
        auto& target = pump.m_targetGame;
        target.valid = true;
        target.w = target.h = 32;
        target.executablePath = exe;
        QVERIFY(GameDetector::shouldCapture(target, &config));
        pump.m_owners.setAuto(true);
        QVERIFY(pump.m_owners.acquireManual(0, 90));
        const auto generation = pump.m_buffer.requestStart(QStringLiteral("Allowed game"));
        QVERIFY(pump.m_buffer.confirm(generation, ReplayBufferState::Recording));

        QVERIFY(config.setCaptureExecutableAllowed(QFileInfo(exe).canonicalFilePath(), false));
        QCOMPARE(pump.m_buffer.state(), ReplayBufferState::Stopped);
        QVERIFY(!pump.m_owners.needsSession());
        QVERIFY(!pump.m_targetGame.valid);
        QVERIFY(!pump.m_buffer.confirm(generation, ReplayBufferState::Ready));
    }

    void aScreenshotStillEncodingAtExitIsWrittenInFull()
    {
        // t41: destroying the service right after a capture (app exit) must
        // finish the write, not drop it or leave a partial file.
        QTemporaryDir dir(QCoreApplication::applicationDirPath() + "/exit-XXXXXX");
        QVERIFY(dir.isValid());
        ConfigManager config(dir.path() + "/config.json");
        config.setValue(ConfigKeys::StorageScreenshotsRoot, dir.path());
        CaptureLocations locations(&config);
        QImage big(3840, 2160, QImage::Format_RGB32);
        for (int y = 0; y < big.height(); ++y)
            for (int x = 0; x < big.width(); x += 7)
                big.setPixel(x, y, qRgb(x & 0xff, y & 0xff, (x ^ y) & 0xff));
        QElapsedTimer t;
        {
            ScreenshotService shots(&config, &locations);
            shots.saveImage(big, "Fixture", {}, 303);
            t.start();
        }   // destroyed while the encode runs
        const qint64 exitMs = t.elapsed();
        QStringList written;
        QDirIterator it(dir.path(), {"*.png", "*.jpg", "*.jpeg", "*.webp"}, QDir::Files,
                        QDirIterator::Subdirectories);
        while (it.hasNext())
            written.append(it.next());
        qInfo() << "service destroyed during a 4K encode; destructor waited" << exitMs << "ms";
        QCOMPARE(written.size(), 1);
        const QImage back(written.first());
        QVERIFY(!back.isNull());
        QCOMPARE(back.size(), QSize(3840, 2160));
    }

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
        // Both encodes must complete. A worker that loses the folder-create
        // race used to drop its capture with only an unlogged failed() emit, so
        // a bare count timeout hid the reason; carry it into the report.
        QElapsedTimer wait;
        wait.start();
        while (saved.count() < 2 && wait.elapsed() < 5000)
            QTest::qWait(10);
        QStringList reasons;
        for (const auto& f : failed)
            reasons += f.at(0).toString();
        QVERIFY2(saved.count() == 2,
                 qPrintable(QStringLiteral("saved=%1 failed=%2 [%3]")
                                .arg(saved.count())
                                .arg(failed.count())
                                .arg(reasons.join(QStringLiteral("; ")))));
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
