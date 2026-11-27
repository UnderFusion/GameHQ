#include "capture/FramePumpService.h"
#include "capture/FramePumpPipeline.h"
#include "capture/ReplayExporter.h"
#include "capture/ScreenshotService.h"
#include "capture/WindowCaptureGeometry.h"
#include <QtTest>
#include "config/ConfigManager.h"
#include "config/CaptureLocations.h"
#include "config/ConfigKeys.h"
#include "core/ProcessIdentity.h"
#include "integration/ExternalGameContext.h"
#include <QDirIterator>
#include <QImage>
#include <QSignalSpy>
#include <QSemaphore>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <windows.h>
#include <dwmapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
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
        const HWND window = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"pool fixture",
            WS_POPUP | WS_VISIBLE, -32000, -32000, 32, 32, nullptr, nullptr, nullptr, nullptr);
        QVERIFY(window);
        const auto cleanup = qScopeGuard([&] { DestroyWindow(window); });
        worker.m_pipe = new FramePumpWorker::Pipeline;
        worker.m_pipe->hwnd = window;
        worker.m_pipe->pid = GetCurrentProcessId();
        worker.m_pipe->validationClock.start();
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
        pump.m_targetHasReplay = true;
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

    void screenshotWithoutAnEligibleWindowCannotArmCapture()
    {
        QTemporaryDir dir(QCoreApplication::applicationDirPath() + "/policy-XXXXXX");
        QVERIFY(dir.isValid());
        ConfigManager config(dir.filePath("config.json"));
        config.setValue(ConfigKeys::CaptureMode, QStringLiteral("whitelist"));
        FramePumpService pump(&config, nullptr);
        QSignalSpy skipped(&pump, &FramePumpService::windowScreenshotSkipped);
        const auto generation = pump.m_buffer.generation();
        pump.captureWindowScreenshot(123);
        QCOMPARE(skipped.size(), 1);
        QCOMPARE(skipped.first().at(1).toULongLong(), 123ULL);
        QCOMPARE(pump.m_buffer.generation(), generation);
        QVERIFY(!pump.m_owners.needsSession());
    }

    void backgroundAndPinResolveTheSameWindowForEveryCapture()
    {
        QTemporaryDir dir(QCoreApplication::applicationDirPath() + "/policy-XXXXXX");
        QVERIFY(dir.isValid());
        ConfigManager config(dir.filePath("config.json"));
        config.setValue(ConfigKeys::CaptureMode, QStringLiteral("always"));
        config.setValue(ConfigKeys::ReplayAuto, false);
        HWND first = CreateWindowExW(0, L"STATIC", L"Game A", 0, 0, 0, 32, 32,
                                     HWND_MESSAGE, nullptr, nullptr, nullptr);
        HWND second = CreateWindowExW(0, L"STATIC", L"Game B", 0, 0, 0, 32, 32,
                                            HWND_MESSAGE, nullptr, nullptr, nullptr);
        QVERIFY(first && second);
        const auto cleanup = qScopeGuard([&] {
            if (first)
                DestroyWindow(first);
            if (second)
                DestroyWindow(second);
        });
        FILETIME created{}, exited{}, kernel{}, user{};
        QVERIFY(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user));
        ForegroundGame gameA;
        gameA.valid = gameA.isGame = true;
        gameA.hwnd = first;
        gameA.pid = GetCurrentProcessId();
        gameA.processStarted = (quint64(created.dwHighDateTime) << 32) | created.dwLowDateTime;
        gameA.w = gameA.h = 32;
        ForegroundGame gameB = gameA;
        gameB.hwnd = second;
        const auto desktopApp = [&] {
            auto app = gameB;
            app.isGame = false;
            return app;
        }();

        FramePumpService pump(&config, nullptr);
        pump.m_autoTimer->stop();
        QCOMPARE(pump.selectTarget(gameA).hwnd, gameA.hwnd);
        QVERIFY(!pump.selectTarget({}).valid);
        config.setValue(ConfigKeys::CaptureBackground, true);
        QCOMPARE(pump.selectTarget(gameA).hwnd, gameA.hwnd);
        QCOMPARE(pump.selectTarget(desktopApp).hwnd, gameA.hwnd);
        pump.m_pinnedTarget = gameA;
        QCOMPARE(pump.selectTarget(gameB).hwnd, gameA.hwnd);
        auto wrongProcessInstance = gameA;
        ++wrongProcessInstance.processStarted;
        QVERIFY(!GameDetector::canRetainCaptureTarget(wrongProcessInstance, &config));
        QVERIFY(DestroyWindow(first));
        first = nullptr;
        QCOMPARE(pump.selectTarget({}).hwnd, gameB.hwnd);
        QVERIFY(!pump.hasPinnedTarget());
        QVERIFY(DestroyWindow(second));
        second = nullptr;
        QVERIFY(!pump.selectTarget({}).valid);
    }

    void clientCropUsesTheWindowOriginRegardlessOfScreenPosition()
    {
        const QRect frame(-64, 108, 816, 638);
        const QRect client(-56, 138, 800, 600);
        QCOMPARE(WindowCaptureGeometry::clientCrop(frame, client, frame.size()), QRect(8, 30, 800, 600));
        QCOMPARE(WindowCaptureGeometry::clientCrop(client, client, client.size()), QRect(0, 0, 800, 600));
        QVERIFY(WindowCaptureGeometry::clientCrop(frame, client, QSize(816, 600)).isEmpty());
        QVERIFY(WindowCaptureGeometry::clientCrop(frame, client.translated(20, 0), frame.size()).isEmpty());
    }

    void retiredReplaySegmentSurvivesUntilTheLastExportLeaseEnds()
    {
        QTemporaryDir dir(QCoreApplication::applicationDirPath() + "/retired-XXXXXX");
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("old_clip.mp4"));
        QFile segment(path);
        QVERIFY(segment.open(QIODevice::WriteOnly));
        segment.close();
        SegmentLease first({path});
        SegmentLease second({path});
        SegmentLease::removeWhenUnleased(path);
        QVERIFY(QFile::exists(path));
        first = SegmentLease{};
        QVERIFY(QFile::exists(path));
        second = SegmentLease{};
        QVERIFY(!QFile::exists(path));
    }

    void captureClientThroughAnOccludingWindow()
    {
        if (!qEnvironmentVariableIsSet("GAMEHQ_TEST_WGC_NATIVE"))
            QSKIP("Requires an interactive Windows desktop and a WGC-capable GPU");
        QVERIFY(GameDetector::onInputDesktop());
        const auto previousDpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        QVERIFY(previousDpi);
        const auto restoreDpi = qScopeGuard([&] { SetThreadDpiAwarenessContext(previousDpi); });
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        const HBRUSH blue = CreateSolidBrush(RGB(0, 70, 220));
        QVERIFY(blue);
        const auto deleteBrush = qScopeGuard([&] { DeleteObject(blue); });
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = DefWindowProcW;
        windowClass.hInstance = instance;
        windowClass.hbrBackground = blue;
        windowClass.lpszClassName = L"GameHQClientCaptureFixture";
        QVERIFY(RegisterClassW(&windowClass));
        const auto unregister = qScopeGuard([&] { UnregisterClassW(windowClass.lpszClassName, instance); });
        const HWND game = CreateWindowExW(WS_EX_NOACTIVATE, windowClass.lpszClassName, L"Window frame",
            WS_OVERLAPPEDWINDOW, 120, 120, 380, 260, nullptr, nullptr, instance, nullptr);
        QVERIFY(game);
        const auto destroyGame = qScopeGuard([&] { DestroyWindow(game); });
        ShowWindow(game, SW_SHOWNOACTIVATE);
        UpdateWindow(game);
        // Populate the native window's backing surface before moving it
        // partly off screen. A freshly exposed GDI window may paint only the
        // pixels the desktop compositor requests.
        QVERIFY(SUCCEEDED(DwmFlush()));
        QVERIFY(SetWindowPos(game, nullptr, -36, 120, 0, 0,
                            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW));
        const HWND cover = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"STATIC", L"Cover",
            WS_POPUP | WS_VISIBLE, 80, 170, 180, 100, nullptr, nullptr, instance, nullptr);
        QVERIFY(cover);
        const auto destroyCover = qScopeGuard([&] { DestroyWindow(cover); });
        UpdateWindow(cover);

        QTemporaryDir dir(QCoreApplication::applicationDirPath() + "/wgc-XXXXXX");
        QVERIFY(dir.isValid());
        ConfigManager config(dir.filePath("config.json"));
        config.setValue(ConfigKeys::CaptureMode, QStringLiteral("whitelist"));
        QVERIFY(config.setCaptureExecutableAllowed(QCoreApplication::applicationFilePath(), true));
        config.setValue(ConfigKeys::ReplayAuto, false);
        config.setValue(ConfigKeys::InternalCaptureExperimentalHdr, false);
        config.setValue(ConfigKeys::AudioSource, QStringLiteral("off"));
        CaptureLocations locations(&config);
        QVERIFY(locations.setBaseRoot(CaptureLocations::Kind::Clips, dir.path()));
        RECT client{};
        QVERIFY(GetClientRect(game, &client));
        ForegroundGame target;
        target.valid = true;
        target.hwnd = game;
        target.pid = GetCurrentProcessId();
        QVERIFY(ProcessIdentity::creationTimeOf(GetCurrentProcess(), target.processStarted));
        target.w = client.right;
        target.h = client.bottom;
        target.executablePath = QCoreApplication::applicationFilePath();
        target.gameName = QStringLiteral("Fixture");
        FramePumpService pump(&config, &locations);
        pump.m_autoTimer->stop();
        pump.pinTarget(target);
        QVERIFY(pump.hasPinnedTarget());
        QSignalSpy ready(&pump, &FramePumpService::windowScreenshotReady);
        QSignalSpy failed(&pump, &FramePumpService::windowScreenshotFailed);
        pump.captureWindowScreenshot(1);
        QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty() || !failed.isEmpty(), 7000);
        QVERIFY2(failed.isEmpty(), qPrintable(failed.value(0).value(0).toString()));
        const QImage image = qvariant_cast<QImage>(ready.constFirst().at(0));
        QCOMPARE(image.size(), QSize(client.right, client.bottom));
        const QColor corner = image.pixelColor(1, 1);
        const QColor uncovered = image.pixelColor(image.width() - 2, image.height() / 2);
        QVERIFY2(uncovered.blue() > uncovered.green() && uncovered.green() > uncovered.red(),
                 qPrintable(uncovered.name()));
        QCOMPARE(image.pixelColor(image.width() / 2, image.height() / 2).rgb(), uncovered.rgb());
        QCOMPARE(corner.rgb(), uncovered.rgb());
        QCOMPARE(ready.constFirst().at(1).toString(), target.gameName);
        QCOMPARE(ready.constFirst().at(3).toULongLong(), 1ULL);

        // Change a small visible patch so WGC delivers new frames while the
        // same pinned window remains underneath the cover.
        QTimer repaint;
        bool alternate = false;
        connect(&repaint, &QTimer::timeout, this, [&] {
            const HDC dc = GetDC(game);
            if (!dc)
                return;
            RECT patch{client.right - 24, 8, client.right - 8, 24};
            FillRect(dc, &patch, alternate ? blue : GetSysColorBrush(COLOR_WINDOW));
            ReleaseDC(game, dc);
            alternate = !alternate;
        });
        repaint.setTimerType(Qt::PreciseTimer);
        repaint.start(16);
        config.setValue(ConfigKeys::InternalCaptureExperimentalHdr, true);
        config.setValue(ConfigKeys::ReplayAuto, true);
        pump.restartBuffer();
        QTRY_VERIFY_WITH_TIMEOUT(pump.bufferState() == ReplayBufferState::Recording
                                  || pump.bufferState() == ReplayBufferState::Ready, 7000);
        pump.captureWindowScreenshot(2);
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 2, 5000);
        const QImage mapped = qvariant_cast<QImage>(ready.last().at(0));
        QCOMPARE(mapped.size(), image.size());
        const QColor mappedCenter = mapped.pixelColor(mapped.width() / 2, mapped.height() / 2);
        QVERIFY(mappedCenter.blue() > mappedCenter.green() && mappedCenter.green() > mappedCenter.red());
        QCOMPARE(ready.last().at(3).toULongLong(), 2ULL);
        QTest::qWait(850);
        QSignalSpy saved(&pump, &FramePumpService::clipSaved);
        QSignalSpy clipFailed(&pump, &FramePumpService::clipFailed);
        pump.saveReplay(CaptureRequest::create(CaptureRequest::Source::Ui));
        QTRY_VERIFY_WITH_TIMEOUT(!saved.isEmpty() || !clipFailed.isEmpty(), 10000);
        QVERIFY2(clipFailed.isEmpty(), qPrintable(clipFailed.value(0).value(1).toString()));
        QCOMPARE(saved.constFirst().at(1).toString(), target.gameName);
        const QString clipPath = saved.constFirst().at(0).toString();
        QVERIFY(ReplayExporter::hasReadableVideo(clipPath));
        QVERIFY(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)));
        const auto uninitialize = qScopeGuard([] { CoUninitialize(); });
        QVERIFY(SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)));
        const auto shutdown = qScopeGuard([] { MFShutdown(); });
        IMFSourceReader* reader = nullptr;
        QVERIFY(SUCCEEDED(MFCreateSourceReaderFromURL(clipPath.toStdWString().c_str(), nullptr, &reader)));
        const auto releaseReader = qScopeGuard([&] { reader->Release(); });
        IMFMediaType* video = nullptr;
        QVERIFY(SUCCEEDED(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &video)));
        const auto releaseVideo = qScopeGuard([&] { video->Release(); });
        UINT32 width = 0, height = 0;
        QVERIFY(SUCCEEDED(MFGetAttributeSize(video, MF_MT_FRAME_SIZE, &width, &height)));
        QCOMPARE(QSize(int(width), int(height)), QSize(client.right & ~1, client.bottom & ~1));
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
