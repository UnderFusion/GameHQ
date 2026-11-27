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
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
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

namespace {

// The fixture window paints RGB(0, 70, 220). Encoded H.264 shifts the exact
// value, so accept any clearly blue pixel and reject desktop, taskbar, title
// bar or black padding.
bool isFixtureBlue(const QColor& c)
{
    return c.blue() > 150 && c.red() < 70 && c.green() < 130 && c.blue() > c.green() + 60;
}

// Samples the four corners and edge midpoints, one pixel inside the border.
// The repaint patch in the top-right area is far from every sample.
QString nonFixtureEdge(const QImage& image)
{
    const int w = image.width();
    const int h = image.height();
    const QPoint samples[] = {{1, 1}, {w - 2, 1}, {1, h - 2}, {w - 2, h - 2},
                              {w / 2, 1}, {w / 2, h - 2}, {1, h / 2}, {w - 2, h / 2},
                              {w / 2, h / 2}};
    for (const QPoint& p : samples) {
        const QColor c = image.pixelColor(p);
        if (!isFixtureBlue(c))
            return QStringLiteral("(%1,%2)=%3").arg(p.x()).arg(p.y()).arg(c.name());
    }
    return {};
}

// Decodes every video frame of an MP4 to RGB32 through Media Foundation.
// The caller owns COM and MFStartup.
QList<QImage> decodeAllFrames(const QString& path, QString* error)
{
    QList<QImage> frames;
    IMFAttributes* attributes = nullptr;
    if (FAILED(MFCreateAttributes(&attributes, 1))) {
        *error = QStringLiteral("MFCreateAttributes failed");
        return frames;
    }
    attributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    IMFSourceReader* reader = nullptr;
    const HRESULT opened = MFCreateSourceReaderFromURL(path.toStdWString().c_str(), attributes, &reader);
    attributes->Release();
    if (FAILED(opened)) {
        *error = QStringLiteral("open failed 0x%1").arg(quint32(opened), 8, 16, QLatin1Char('0'));
        return frames;
    }
    const auto releaseReader = qScopeGuard([&] { reader->Release(); });
    IMFMediaType* rgb = nullptr;
    MFCreateMediaType(&rgb);
    rgb->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    rgb->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    const HRESULT typed = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, rgb);
    rgb->Release();
    if (FAILED(typed)) {
        *error = QStringLiteral("RGB32 output refused");
        return frames;
    }
    IMFMediaType* current = nullptr;
    if (FAILED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &current))) {
        *error = QStringLiteral("no current type");
        return frames;
    }
    UINT32 width = 0, height = 0;
    MFGetAttributeSize(current, MF_MT_FRAME_SIZE, &width, &height);
    UINT32 strideValue = width * 4;
    const bool hasStride = SUCCEEDED(current->GetUINT32(MF_MT_DEFAULT_STRIDE, &strideValue));
    current->Release();
    const LONG stride = hasStride ? LONG(qint32(strideValue)) : LONG(width * 4);
    for (int guard = 0; guard < 10000; ++guard) {
        DWORD flags = 0;
        IMFSample* sample = nullptr;
        if (FAILED(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, nullptr,
                                      &sample))) {
            *error = QStringLiteral("ReadSample failed after %1 frames").arg(frames.size());
            break;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (sample)
                sample->Release();
            break;
        }
        if (!sample)
            continue;
        IMFMediaBuffer* buffer = nullptr;
        if (SUCCEEDED(sample->GetBufferByIndex(0, &buffer))) {
            QImage image(int(width), int(height), QImage::Format_RGB32);
            bool copied = false;
            IMF2DBuffer* planar = nullptr;
            if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&planar)))) {
                BYTE* scan0 = nullptr;
                LONG pitch = 0;
                if (SUCCEEDED(planar->Lock2D(&scan0, &pitch))) {
                    for (UINT32 y = 0; y < height; ++y)
                        memcpy(image.scanLine(int(y)), scan0 + LONG(y) * pitch, size_t(width) * 4);
                    planar->Unlock2D();
                    copied = true;
                }
                planar->Release();
            }
            BYTE* data = nullptr;
            DWORD length = 0;
            if (!copied && SUCCEEDED(buffer->Lock(&data, nullptr, &length))) {
                const LONG absolute = std::abs(stride);
                if (length >= DWORD(absolute) * height) {
                    for (UINT32 y = 0; y < height; ++y) {
                        const BYTE* row = stride >= 0 ? data + y * absolute
                                                      : data + (height - 1 - y) * absolute;
                        memcpy(image.scanLine(int(y)), row, size_t(width) * 4);
                    }
                    copied = true;
                }
                buffer->Unlock();
            }
            if (copied)
                frames.append(image);
            buffer->Release();
        }
        sample->Release();
    }
    return frames;
}

} // namespace

class CaptureAcknowledgementTest : public QObject
{
    Q_OBJECT
private slots:
    void staleWorkerCommandsCannotReadOrStopTheCurrentTarget()
    {
        FramePumpWorker worker;
        worker.m_pumpGeneration = 8;
        worker.m_pipe = new FramePumpWorker::Pipeline;
        worker.m_pipe->gameName = QStringLiteral("Current game");
        auto* current = worker.m_pipe;
        QSignalSpy shotFailed(&worker, &FramePumpWorker::windowScreenshotFailed);
        QSignalSpy replayFailed(&worker, &FramePumpWorker::clipFailed);
        QSignalSpy saveFinished(&worker, &FramePumpWorker::saveRequestFinished);
        worker.captureScreenshotOnWorker(7, 11);
        worker.saveReplayOnWorker({}, 7, 12, 13);
        worker.stopPumpForGeneration(7);
        QCOMPARE(worker.m_pipe, current);
        QCOMPARE(worker.m_pumpGeneration, 8ULL);
        QVERIFY(!worker.m_screenshotPending);
        QCOMPARE(shotFailed.size(), 1);
        QCOMPARE(shotFailed.first().at(1).toULongLong(), 11ULL);
        QCOMPARE(replayFailed.size(), 1);
        QCOMPARE(saveFinished.size(), 1);
        QCOMPARE(saveFinished.first().at(0).toULongLong(), 12ULL);
    }

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
        const QImage preview(saved.constFirst().at(2).toString());
        QCOMPARE(preview.size(), QSize(client.right & ~1, client.bottom & ~1));
        QVERIFY(preview.pixelColor(preview.width() / 2, preview.height() / 2).blue() > 100);
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
        QImage replayFrame;
        QVERIFY(ReplayExporter::grabThumbnail(clipPath, replayFrame));
        QCOMPARE(replayFrame.size(), preview.size());
        const QColor middle = replayFrame.pixelColor(replayFrame.width() / 2, replayFrame.height() / 2);
        const QColor edge = replayFrame.pixelColor(2, 2);
        QVERIFY(middle.blue() > middle.green() && middle.green() > middle.red());
        QVERIFY(edge.blue() > edge.green() && edge.green() > edge.red());
    }

    // Issue #40: a game that moves or resizes while capture is active. The
    // fixture stays at the bottom of the z-order without activation, so other
    // windows genuinely cover it and the desktop focus is never touched.
    void captureFollowsTheClientWhileTheWindowMovesAndResizes()
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
        windowClass.lpszClassName = L"GameHQMovingCaptureFixture";
        QVERIFY(RegisterClassW(&windowClass));
        const auto unregister = qScopeGuard([&] { UnregisterClassW(windowClass.lpszClassName, instance); });
        const HWND game = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, windowClass.lpszClassName,
            L"Moving fixture", WS_OVERLAPPEDWINDOW, 160, 160, 380, 260, nullptr, nullptr, instance, nullptr);
        QVERIFY(game);
        const auto destroyGame = qScopeGuard([&] { DestroyWindow(game); });
        // Windows 11 rounds the bottom corners of framed windows, which also
        // rounds the client's bottom corners in WGC. Square the fixture so
        // every edge pixel is checked strictly (DWMWA_WINDOW_CORNER_PREFERENCE
        // = 33, DWMWCP_DONOTROUND = 1; older builds ignore the attribute).
        const DWORD squareCorners = 1;
        DwmSetWindowAttribute(game, 33, &squareCorners, sizeof(squareCorners));
        QVERIFY(SetWindowPos(game, HWND_BOTTOM, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW));
        UpdateWindow(game);
        QVERIFY(SUCCEEDED(DwmFlush()));
        MONITORINFO monitor{sizeof(MONITORINFO)};
        QVERIFY(GetMonitorInfoW(MonitorFromWindow(game, MONITOR_DEFAULTTONEAREST), &monitor));
        const RECT screen = monitor.rcMonitor;
        const RECT work = monitor.rcWork;

        QTemporaryDir dir(QCoreApplication::applicationDirPath() + "/wgc-move-XXXXXX");
        QVERIFY(dir.isValid());
        ConfigManager config(dir.filePath("config.json"));
        config.setValue(ConfigKeys::CaptureMode, QStringLiteral("whitelist"));
        QVERIFY(config.setCaptureExecutableAllowed(QCoreApplication::applicationFilePath(), true));
        config.setValue(ConfigKeys::ReplayAuto, true);
        config.setValue(ConfigKeys::InternalCaptureExperimentalHdr, false);
        config.setValue(ConfigKeys::AudioSource, QStringLiteral("off"));
        CaptureLocations locations(&config);
        QVERIFY(locations.setBaseRoot(CaptureLocations::Kind::Clips, dir.path()));
        RECT client{};
        QVERIFY(GetClientRect(game, &client));
        const QSize clientSize(client.right, client.bottom);
        ForegroundGame target;
        target.valid = true;
        target.hwnd = game;
        target.pid = GetCurrentProcessId();
        QVERIFY(ProcessIdentity::creationTimeOf(GetCurrentProcess(), target.processStarted));
        target.w = client.right;
        target.h = client.bottom;
        target.executablePath = QCoreApplication::applicationFilePath();
        target.gameName = QStringLiteral("Moving fixture");
        FramePumpService pump(&config, &locations);
        pump.m_autoTimer->stop();
        QStringList restarts;
        connect(pump.m_worker, &FramePumpWorker::restartRequested, this,
                [&restarts](quint64, const QString& reason) { restarts.append(reason); },
                Qt::QueuedConnection);
        QList<bool> pauses;
        connect(pump.m_worker, &FramePumpWorker::capturePausedChanged, this,
                [&pauses](quint64, bool paused) { pauses.append(paused); }, Qt::QueuedConnection);
        pump.pinTarget(target);
        QVERIFY(pump.hasPinnedTarget());

        // Keep WGC delivering frames: toggle a small patch away from the edges.
        QTimer repaint;
        bool alternate = false;
        connect(&repaint, &QTimer::timeout, this, [&] {
            RECT now{};
            GetClientRect(game, &now);
            const HDC dc = GetDC(game);
            if (!dc)
                return;
            RECT patch{now.right - 24, 8, now.right - 8, 24};
            FillRect(dc, &patch, alternate ? blue : GetSysColorBrush(COLOR_WINDOW));
            ReleaseDC(game, dc);
            alternate = !alternate;
        });
        repaint.setTimerType(Qt::PreciseTimer);
        repaint.start(16);
        pump.restartBuffer();
        QTRY_VERIFY_WITH_TIMEOUT(pump.bufferState() == ReplayBufferState::Recording, 7000);

        QSignalSpy ready(&pump, &FramePumpService::windowScreenshotReady);
        QSignalSpy failed(&pump, &FramePumpService::windowScreenshotFailed);
        quint64 nextShot = 1;
        auto shoot = [&](QImage* image, QString* reason) {
            const qsizetype before = ready.size() + failed.size();
            const quint64 id = nextShot++;
            pump.captureWindowScreenshot(id);
            QElapsedTimer clock;
            clock.start();
            while (ready.size() + failed.size() == before && clock.elapsed() < 7000)
                QTest::qWait(20);
            if (failed.size() + ready.size() == before) {
                *reason = QStringLiteral("screenshot %1 timed out").arg(id);
                return false;
            }
            if (!failed.isEmpty() && failed.last().at(1).toULongLong() == id) {
                *reason = failed.last().at(0).toString();
                return false;
            }
            *image = qvariant_cast<QImage>(ready.last().at(0));
            return true;
        };
        auto verifyClip = [&](const QSize& expected, QString* reason) {
            QSignalSpy saved(&pump, &FramePumpService::clipSaved);
            QSignalSpy clipFailed(&pump, &FramePumpService::clipFailed);
            pump.saveReplay(CaptureRequest::create(CaptureRequest::Source::Ui));
            QElapsedTimer clock;
            clock.start();
            while (saved.isEmpty() && clipFailed.isEmpty() && clock.elapsed() < 15000)
                QTest::qWait(20);
            if (saved.isEmpty()) {
                *reason = clipFailed.isEmpty() ? QStringLiteral("save timed out")
                                               : clipFailed.constFirst().at(1).toString();
                return false;
            }
            const QString clipPath = saved.constFirst().at(0).toString();
            if (!ReplayExporter::hasReadableVideo(clipPath)) {
                *reason = QStringLiteral("unreadable clip");
                return false;
            }
            const QImage preview(saved.constFirst().at(2).toString());
            if (preview.isNull() || !nonFixtureEdge(preview).isEmpty()) {
                *reason = QStringLiteral("preview %1").arg(preview.isNull() ? QStringLiteral("missing")
                                                                            : nonFixtureEdge(preview));
                return false;
            }
            QString decodeError;
            const QList<QImage> frames = decodeAllFrames(clipPath, &decodeError);
            if (frames.size() < 10) {
                *reason = QStringLiteral("only %1 frames decoded %2").arg(frames.size()).arg(decodeError);
                return false;
            }
            const QString evidence = qEnvironmentVariable("GAMEHQ_TEST_EVIDENCE_DIR");
            if (!evidence.isEmpty()) {
                static int clipIndex = 0;
                QFile::copy(clipPath, QDir(evidence).filePath(
                    QStringLiteral("moving-fixture-%1.mp4").arg(++clipIndex)));
            }
            QStringList badFrames;
            for (qsizetype i = 0; i < frames.size(); ++i) {
                const QString bad = nonFixtureEdge(frames[i]);
                if (!bad.isEmpty())
                    badFrames.append(QStringLiteral("%1:%2").arg(i).arg(bad));
            }
            if (!badFrames.isEmpty())
                qWarning().noquote() << "foreign frames" << frames.size() << badFrames.join(u' ');
            const QSize even(expected.width() & ~1, expected.height() & ~1);
            for (qsizetype i = 0; i < frames.size(); ++i) {
                if (frames[i].size() != even) {
                    *reason = QStringLiteral("frame %1 is %2x%3, expected %4x%5").arg(i)
                        .arg(frames[i].width()).arg(frames[i].height()).arg(even.width()).arg(even.height());
                    return false;
                }
                const QString bad = nonFixtureEdge(frames[i]);
                if (!bad.isEmpty()) {
                    *reason = QStringLiteral("frame %1 of %2 has foreign pixel %3")
                        .arg(i).arg(frames.size()).arg(bad);
                    return false;
                }
            }
            qInfo().noquote() << QStringLiteral("clip %1: %2 frames at %3x%4 verified")
                .arg(QFileInfo(clipPath).fileName()).arg(frames.size()).arg(even.width()).arg(even.height());
            return true;
        };
        QVERIFY(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)));
        const auto uninitialize = qScopeGuard([] { CoUninitialize(); });
        QVERIFY(SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)));
        const auto shutdown = qScopeGuard([] { MFShutdown(); });

        // Moves keep the client size, so neither the pipeline nor the
        // encoder may restart. Each position keeps most of the window on the
        // starting monitor so its DPI, and therefore its client size, holds.
        struct Move { const char* name; int x; int y; };
        const Move moves[] = {
            {"left edge off screen", int(screen.left) - 120, 200},
            {"below the work area", int(work.right) - 260, int(work.bottom) - 90},
            {"below the monitor", int(screen.left) + 200, int(screen.bottom) - 110},
            {"delayed reposition back", 160, 160},
        };
        for (const Move& move : moves) {
            QVERIFY(SetWindowPos(game, nullptr, move.x, move.y, 0, 0,
                                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE));
            QVERIFY(SUCCEEDED(DwmFlush()));
            QTest::qWait(300);
            RECT moved{};
            QVERIFY(GetClientRect(game, &moved));
            QCOMPARE(QSize(moved.right, moved.bottom), clientSize);
            QImage image;
            QString reason;
            QVERIFY2(shoot(&image, &reason), qPrintable(QStringLiteral("%1: %2").arg(move.name, reason)));
            QCOMPARE(image.size(), clientSize);
            const QString bad = nonFixtureEdge(image);
            QVERIFY2(bad.isEmpty(), qPrintable(QStringLiteral("%1: %2").arg(move.name, bad)));
        }
        QVERIFY2(restarts.isEmpty(), qPrintable(restarts.join(u"; ")));
        QCOMPARE(pump.bufferState(), ReplayBufferState::Recording);
        QString clipReason;
        QVERIFY2(verifyClip(clientSize, &clipReason), qPrintable(QStringLiteral("moves: ") + clipReason));
        QVERIFY2(restarts.isEmpty(), qPrintable(restarts.join(u"; ")));

        // A resize must re-arm once at the new client size; frames of the
        // old size must never reach the replay or a screenshot.
        QVERIFY(SetWindowPos(game, nullptr, 0, 0, 470, 330, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE));
        RECT resized{};
        QVERIFY(GetClientRect(game, &resized));
        const QSize resizedClient(resized.right, resized.bottom);
        QVERIFY(resizedClient != clientSize);
        QTRY_VERIFY_WITH_TIMEOUT(!restarts.isEmpty(), 5000);
        QVERIFY2(restarts.size() == 1, qPrintable(restarts.join(u"; ")));
        QCOMPARE(restarts.constFirst(), QStringLiteral("Game resolution changed"));
        QTest::qWait(200);
        QTRY_VERIFY_WITH_TIMEOUT(pump.bufferState() == ReplayBufferState::Recording, 7000);
        QImage resizedShot;
        QVERIFY2(shoot(&resizedShot, &clipReason), qPrintable(QStringLiteral("resize: ") + clipReason));
        QCOMPARE(resizedShot.size(), resizedClient);
        QVERIFY2(nonFixtureEdge(resizedShot).isEmpty(), qPrintable(nonFixtureEdge(resizedShot)));
        QTest::qWait(900);
        QVERIFY2(verifyClip(resizedClient, &clipReason), qPrintable(QStringLiteral("resize: ") + clipReason));
        QCOMPARE(restarts.size(), 1);

        // Minimize stops valid frames. Capture must keep the pinned target and
        // resume cleanly after a restore without activation.
        ShowWindow(game, SW_SHOWMINNOACTIVE);
        QTest::qWait(3000);
        QVERIFY(pump.hasPinnedTarget());
        ShowWindow(game, SW_SHOWNOACTIVATE);
        QVERIFY(SetWindowPos(game, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE));
        QVERIFY(SUCCEEDED(DwmFlush()));
        QTRY_VERIFY_WITH_TIMEOUT(pump.bufferState() == ReplayBufferState::Recording
                                     && !pump.m_capturePaused, 10000);
        QTest::qWait(1200);
        QImage restoredShot;
        QVERIFY2(shoot(&restoredShot, &clipReason), qPrintable(QStringLiteral("restore: ") + clipReason));
        QCOMPARE(restoredShot.size(), resizedClient);
        QVERIFY2(nonFixtureEdge(restoredShot).isEmpty(), qPrintable(nonFixtureEdge(restoredShot)));
        QVERIFY2(verifyClip(resizedClient, &clipReason), qPrintable(QStringLiteral("restore: ") + clipReason));
        qInfo().noquote() << QStringLiteral("restarts=[%1] pauses=%2")
            .arg(restarts.join(u"; ")).arg(pauses.size());
        QVERIFY(pump.hasPinnedTarget());
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
