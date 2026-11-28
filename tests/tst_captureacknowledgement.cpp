#include "capture/FramePumpService.h"
#include "capture/FramePumpPipeline.h"
#include "capture/ReplayExporter.h"
#include "capture/HwH264Encoder.h"
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
#include <QMutex>
#include <QRegularExpression>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <windows.h>
#ifndef PSAPI_VERSION
#define PSAPI_VERSION 2
#endif
#include <psapi.h>
#include <tlhelp32.h>
#include <winternl.h>
#include <dwmapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <algorithm>
#include <atomic>
#include <utility>

namespace {

// The fixture window paints RGB(0, 70, 220). Encoded H.264 shifts the exact
// value, so accept any clearly blue pixel and reject desktop, taskbar, title
// bar or black padding.
bool isFixtureBlue(const QColor& c)
{
    return c.blue() > 150 && c.red() < 70 && c.green() < 130 && c.blue() > c.green() + 60;
}

bool isFixtureOrange(const QColor& c)
{
    return c.red() > 170 && c.blue() < 70 && c.green() > 70 && c.green() < 170;
}

// Samples the four corners and edge midpoints, one pixel inside the border.
// The repaint patch in the top-right area is far from every sample.
QString nonFixtureEdge(const QImage& image, bool (*matches)(const QColor&) = isFixtureBlue)
{
    const int w = image.width();
    const int h = image.height();
    const QPoint samples[] = {{1, 1}, {w - 2, 1}, {1, h - 2}, {w - 2, h - 2},
                              {w / 2, 1}, {w / 2, h - 2}, {1, h / 2}, {w - 2, h / 2},
                              {w / 2, h / 2}};
    for (const QPoint& p : samples) {
        const QColor c = image.pixelColor(p);
        if (!matches(c))
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

// Counts recorder warnings that mean the reused hardware encoder (#39) was
// abandoned. Messages arrive from the capture worker thread.
std::atomic<int> g_reuseStops{0};
QtMessageHandler g_previousHandler = nullptr;

void countReuseStops(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    if (message.contains(QLatin1String("hardware encoder reuse stopped"))
        || message.contains(QLatin1String("CreateVideoProcessorInputView failed")))
        g_reuseStops.fetch_add(1);
    if (g_previousHandler)
        g_previousHandler(type, context, message);
}

// Collects the worker's pointer-policy log lines (#41).
QMutex g_pointerLogMutex;
QStringList g_pointerLog;
QtMessageHandler g_pointerPrevious = nullptr;

void collectPointerLog(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    if (message.contains(QLatin1String("mouse pointer")) || message.contains(QLatin1String("one-shot"))
        || message.contains(QLatin1String("after the pointer setting"))
        || message.contains(QLatin1String("experimental HDR path armed"))) {
        QMutexLocker lock(&g_pointerLogMutex);
        g_pointerLog.append(message);
    }
    if (g_pointerPrevious)
        g_pointerPrevious(type, context, message);
}

struct PointerLog
{
    PointerLog() { take(); g_pointerPrevious = qInstallMessageHandler(collectPointerLog); }
    ~PointerLog() { qInstallMessageHandler(g_pointerPrevious); }
    QStringList take()
    {
        QMutexLocker lock(&g_pointerLogMutex);
        return std::exchange(g_pointerLog, {});
    }
};

// Per-type census of this process's kernel handles (b4-handles). Uses the
// documented-by-convention SystemExtendedHandleInformation (64) and
// ObjectTypeInformation (2); names are never queried, so no call can block.
struct HandleEntry { quintptr value; USHORT typeIndex; };

QList<HandleEntry> ownHandles()
{
    struct Entry {
        PVOID object; ULONG_PTR pid; ULONG_PTR handle; ULONG access;
        USHORT creatorBackTraceIndex; USHORT typeIndex; ULONG attributes; ULONG reserved;
    };
    struct Table { ULONG_PTR count; ULONG_PTR reserved; Entry entries[1]; };
    using NtQuerySystemInformationFn = LONG (NTAPI*)(ULONG, PVOID, ULONG, PULONG);
    static const auto query = reinterpret_cast<NtQuerySystemInformationFn>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation")));
    QList<HandleEntry> result;
    if (!query)
        return result;
    QByteArray buffer(1 << 22, Qt::Uninitialized);
    ULONG needed = 0;
    LONG status = 0;
    while ((status = query(64, buffer.data(), ULONG(buffer.size()), &needed)) == LONG(0xC0000004))
        buffer.resize(qMax<qsizetype>(buffer.size() * 2, qsizetype(needed) + (1 << 20)));
    if (status < 0)
        return result;
    const auto* table = reinterpret_cast<const Table*>(buffer.constData());
    const ULONG_PTR self = GetCurrentProcessId();
    for (ULONG_PTR i = 0; i < table->count; ++i) {
        if (table->entries[i].pid == self)
            result.append({ quintptr(table->entries[i].handle), table->entries[i].typeIndex });
    }
    return result;
}

QString handleTypeName(USHORT typeIndex, const QList<HandleEntry>& handles)
{
    static QHash<USHORT, QString> names;
    if (const auto it = names.constFind(typeIndex); it != names.constEnd())
        return *it;
    using NtQueryObjectFn = LONG (NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static const auto query = reinterpret_cast<NtQueryObjectFn>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryObject")));
    QString name = QStringLiteral("type%1").arg(typeIndex);
    for (const HandleEntry& entry : handles) {
        if (entry.typeIndex != typeIndex || !query)
            continue;
        alignas(8) unsigned char info[1024];
        ULONG length = 0;
        if (query(reinterpret_cast<HANDLE>(entry.value), 2, info, sizeof(info), &length) >= 0) {
            const auto* text = reinterpret_cast<const UNICODE_STRING*>(info);
            name = QString::fromWCharArray(text->Buffer, text->Length / 2);
            break;
        }
    }
    names.insert(typeIndex, name);
    return name;
}

// "Event+3 Thread+2" for handles present now but not in `before`.
QString handleGrowth(const QList<HandleEntry>& before, const QList<HandleEntry>& after)
{
    QSet<quintptr> known;
    for (const HandleEntry& entry : before)
        known.insert(entry.value);
    QMap<QString, int> added;
    QMap<QString, int> removed;
    QSet<quintptr> now;
    for (const HandleEntry& entry : after) {
        now.insert(entry.value);
        if (!known.contains(entry.value))
            ++added[handleTypeName(entry.typeIndex, after)];
    }
    for (const HandleEntry& entry : before) {
        if (!now.contains(entry.value))
            ++removed[handleTypeName(entry.typeIndex, before)];
    }
    QStringList parts;
    QSet<QString> types;
    for (auto it = added.cbegin(); it != added.cend(); ++it) types.insert(it.key());
    for (auto it = removed.cbegin(); it != removed.cend(); ++it) types.insert(it.key());
    QStringList sorted(types.cbegin(), types.cend());
    sorted.sort();
    for (const QString& type : sorted) {
        const int net = added.value(type) - removed.value(type);
        if (net != 0)
            parts.append(QStringLiteral("%1%2%3").arg(type, net > 0 ? QStringLiteral("+") : QString())
                             .arg(net));
    }
    return parts.isEmpty() ? QStringLiteral("none") : parts.join(u' ');
}

int threadCount()
{
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return -1;
    THREADENTRY32 entry{sizeof(THREADENTRY32)};
    int count = 0;
    const DWORD self = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry))
        if (entry.th32OwnerProcessID == self)
            ++count;
    CloseHandle(snapshot);
    return count;
}

struct ReuseStopCounter
{
    ReuseStopCounter() { g_reuseStops = 0; g_previousHandler = qInstallMessageHandler(countReuseStops); }
    ~ReuseStopCounter() { qInstallMessageHandler(g_previousHandler); }
    int count() const { return g_reuseStops.load(); }
};

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
        // Pixel checks must not see the owner's pointer; the replay draws it
        // by default (#41).
        config.setValue(ConfigKeys::ReplayCursor, false);
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
        QVERIFY(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)));
        const auto uninitialize = qScopeGuard([] { CoUninitialize(); });
        QVERIFY(SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)));
        const auto shutdown = qScopeGuard([] { MFShutdown(); });
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
        // Pixel checks must not see the owner's pointer; the replay draws it
        // by default (#41).
        config.setValue(ConfigKeys::ReplayCursor, false);
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
        QVERIFY(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)));
        const auto uninitialize = qScopeGuard([] { CoUninitialize(); });
        QVERIFY(SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)));
        const auto shutdown = qScopeGuard([] { MFShutdown(); });
        ReuseStopCounter reuseStops;
        FramePumpService pump(&config, &locations);
        pump.m_autoTimer->stop();
        QStringList restarts;
        QList<bool> pauses;
        // Context for the queued worker signals. Declared after the lists the
        // lambdas capture, so it is destroyed first and Qt discards calls still
        // posted while ~FramePumpService drains the worker (b5-crash).
        QObject workerSignals;
        connect(pump.m_worker, &FramePumpWorker::restartRequested, &workerSignals,
                [&restarts](quint64, const QString& reason) { restarts.append(reason); },
                Qt::QueuedConnection);
        connect(pump.m_worker, &FramePumpWorker::capturePausedChanged, &workerSignals,
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
        QCOMPARE(reuseStops.count(), 0);
    }

    // A static scene makes WGC stop delivering frames. A replay spanning that
    // gap must still export from the reused hardware encoder (#39).
    void aStaticSceneGapStillExportsTheReplay()
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
        windowClass.lpszClassName = L"GameHQStaticSceneFixture";
        QVERIFY(RegisterClassW(&windowClass));
        const auto unregister = qScopeGuard([&] { UnregisterClassW(windowClass.lpszClassName, instance); });
        const HWND game = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, windowClass.lpszClassName,
            L"Static fixture", WS_OVERLAPPEDWINDOW, 200, 220, 380, 260, nullptr, nullptr, instance, nullptr);
        QVERIFY(game);
        const auto destroyGame = qScopeGuard([&] { DestroyWindow(game); });
        const DWORD squareCorners = 1;
        DwmSetWindowAttribute(game, 33, &squareCorners, sizeof(squareCorners));
        QVERIFY(SetWindowPos(game, HWND_BOTTOM, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW));
        UpdateWindow(game);
        QVERIFY(SUCCEEDED(DwmFlush()));
        QTemporaryDir dir(QCoreApplication::applicationDirPath() + "/wgc-static-XXXXXX");
        QVERIFY(dir.isValid());
        ConfigManager config(dir.filePath("config.json"));
        config.setValue(ConfigKeys::CaptureMode, QStringLiteral("whitelist"));
        QVERIFY(config.setCaptureExecutableAllowed(QCoreApplication::applicationFilePath(), true));
        config.setValue(ConfigKeys::ReplayAuto, true);
        config.setValue(ConfigKeys::InternalCaptureExperimentalHdr, false);
        config.setValue(ConfigKeys::AudioSource, QStringLiteral("off"));
        // Pixel checks must not see the owner's pointer; the replay draws it
        // by default (#41).
        config.setValue(ConfigKeys::ReplayCursor, false);
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
        target.gameName = QStringLiteral("Static fixture");
        QVERIFY(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)));
        const auto uninitialize = qScopeGuard([] { CoUninitialize(); });
        QVERIFY(SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)));
        const auto shutdown = qScopeGuard([] { MFShutdown(); });
        ReuseStopCounter reuseStops;
        FramePumpService pump(&config, &locations);
        pump.m_autoTimer->stop();
        pump.pinTarget(target);
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
        pump.restartBuffer();
        QTRY_VERIFY_WITH_TIMEOUT(pump.bufferState() == ReplayBufferState::Recording, 7000);
        QTest::qWait(600);
        repaint.stop();
        QTest::qWait(700);      // static scene: no new WGC frames
        repaint.start(16);
        QTest::qWait(600);
        QSignalSpy saved(&pump, &FramePumpService::clipSaved);
        QSignalSpy clipFailed(&pump, &FramePumpService::clipFailed);
        pump.saveReplay(CaptureRequest::create(CaptureRequest::Source::Ui));
        QTRY_VERIFY_WITH_TIMEOUT(!saved.isEmpty() || !clipFailed.isEmpty(), 15000);
        QVERIFY2(clipFailed.isEmpty(), qPrintable(clipFailed.isEmpty() ? QString()
                                                                       : clipFailed.constFirst().at(1).toString()));
        const QString clipPath = saved.constFirst().at(0).toString();
        QVERIFY(ReplayExporter::hasReadableVideo(clipPath));
        QString error;
        const QList<QImage> frames = decodeAllFrames(clipPath, &error);
        QVERIFY2(frames.size() >= 10, qPrintable(error));
        for (const QImage& frame : frames)
            QVERIFY2(nonFixtureEdge(frame).isEmpty(), qPrintable(nonFixtureEdge(frame)));
        QCOMPARE(reuseStops.count(), 0);
    }

    // Issues #38 and #39: switching the pinned game repeatedly must keep one
    // capture session, attribute every screenshot and replay frame to the
    // pinned window, and return memory and handles to a flat level.
    // "one-shot": screenshot and replay pointer settings differ (#41), so
    // screenshots use one-shot sessions. "live": matching settings, so
    // screenshots read the replay session's frame exactly like Beta 4.
    void pinSwitchesKeepAttributionAndFlatResources_data()
    {
        QTest::addColumn<bool>("screenshotCursor");
        QTest::newRow("one-shot") << false;
        QTest::newRow("live") << true;
    }

    void pinSwitchesKeepAttributionAndFlatResources()
    {
        QFETCH(bool, screenshotCursor);
        if (!qEnvironmentVariableIsSet("GAMEHQ_TEST_WGC_NATIVE"))
            QSKIP("Requires an interactive Windows desktop and a WGC-capable GPU");
        QVERIFY(GameDetector::onInputDesktop());
        const auto previousDpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        QVERIFY(previousDpi);
        const auto restoreDpi = qScopeGuard([&] { SetThreadDpiAwarenessContext(previousDpi); });
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        const HBRUSH blue = CreateSolidBrush(RGB(0, 70, 220));
        const HBRUSH orange = CreateSolidBrush(RGB(230, 120, 0));
        QVERIFY(blue && orange);
        const auto deleteBrushes = qScopeGuard([&] { DeleteObject(blue); DeleteObject(orange); });
        WNDCLASSW blueClass{};
        blueClass.lpfnWndProc = DefWindowProcW;
        blueClass.hInstance = instance;
        blueClass.hbrBackground = blue;
        blueClass.lpszClassName = L"GameHQPinSoakBlue";
        WNDCLASSW orangeClass = blueClass;
        orangeClass.hbrBackground = orange;
        orangeClass.lpszClassName = L"GameHQPinSoakOrange";
        QVERIFY(RegisterClassW(&blueClass));
        QVERIFY(RegisterClassW(&orangeClass));
        const auto unregister = qScopeGuard([&] {
            UnregisterClassW(blueClass.lpszClassName, instance);
            UnregisterClassW(orangeClass.lpszClassName, instance);
        });
        const DWORD squareCorners = 1;
        auto makeWindow = [&](const wchar_t* cls, const wchar_t* title, int x) {
            const HWND hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, cls, title,
                WS_OVERLAPPEDWINDOW, x, 200, 340, 240, nullptr, nullptr, instance, nullptr);
            if (hwnd) {
                DwmSetWindowAttribute(hwnd, 33, &squareCorners, sizeof(squareCorners));
                SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
                UpdateWindow(hwnd);
            }
            return hwnd;
        };
        const HWND gameA = makeWindow(blueClass.lpszClassName, L"Soak A", 120);
        const HWND gameB = makeWindow(orangeClass.lpszClassName, L"Soak B", 520);
        QVERIFY(gameA && gameB);
        const auto destroyWindows = qScopeGuard([&] { DestroyWindow(gameA); DestroyWindow(gameB); });
        QVERIFY(SUCCEEDED(DwmFlush()));

        QTemporaryDir dir(QCoreApplication::applicationDirPath() + "/wgc-soak-XXXXXX");
        QVERIFY(dir.isValid());
        ConfigManager config(dir.filePath("config.json"));
        config.setValue(ConfigKeys::CaptureMode, QStringLiteral("whitelist"));
        QVERIFY(config.setCaptureExecutableAllowed(QCoreApplication::applicationFilePath(), true));
        config.setValue(ConfigKeys::ReplayAuto, true);
        config.setValue(ConfigKeys::InternalCaptureExperimentalHdr, false);
        config.setValue(ConfigKeys::AudioSource, QStringLiteral("off"));
        // Pixel checks must not see the owner's pointer; the replay draws it
        // by default (#41).
        config.setValue(ConfigKeys::ReplayCursor, false);
        // one-shot: settings differ (screenshot pointer on, replay off);
        // live: both off. Neither lets the owner's pointer into a checked pixel.
        config.setValue(ConfigKeys::CaptureScreenshotCursor, !screenshotCursor);
        CaptureLocations locations(&config);
        QVERIFY(locations.setBaseRoot(CaptureLocations::Kind::Clips, dir.path()));
        auto targetFor = [&](HWND hwnd, const QString& name) {
            RECT client{};
            GetClientRect(hwnd, &client);
            ForegroundGame target;
            target.valid = true;
            target.hwnd = hwnd;
            target.pid = GetCurrentProcessId();
            ProcessIdentity::creationTimeOf(GetCurrentProcess(), target.processStarted);
            target.w = client.right;
            target.h = client.bottom;
            target.executablePath = QCoreApplication::applicationFilePath();
            target.gameName = name;
            return target;
        };
        const ForegroundGame targetA = targetFor(gameA, QStringLiteral("Soak A"));
        const ForegroundGame targetB = targetFor(gameB, QStringLiteral("Soak B"));
        QVERIFY(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)));
        const auto uninitialize = qScopeGuard([] { CoUninitialize(); });
        QVERIFY(SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)));
        const auto shutdown = qScopeGuard([] { MFShutdown(); });
        ReuseStopCounter reuseStops;
        FramePumpService pump(&config, &locations);
        pump.m_autoTimer->stop();
        QTimer repaint;
        bool alternate = false;
        connect(&repaint, &QTimer::timeout, this, [&] {
            for (HWND hwnd : {gameA, gameB}) {
                const HDC dc = GetDC(hwnd);
                if (!dc)
                    continue;
                RECT patch{300, 8, 316, 24};
                FillRect(dc, &patch, alternate ? GetSysColorBrush(COLOR_WINDOW)
                                               : (hwnd == gameA ? blue : orange));
                ReleaseDC(hwnd, dc);
            }
            alternate = !alternate;
        });
        repaint.setTimerType(Qt::PreciseTimer);
        repaint.start(16);
        QSignalSpy ready(&pump, &FramePumpService::windowScreenshotReady);
        QSignalSpy failed(&pump, &FramePumpService::windowScreenshotFailed);
        QSignalSpy saved(&pump, &FramePumpService::clipSaved);
        QSignalSpy clipFailed(&pump, &FramePumpService::clipFailed);
        auto sample = [](qint64* privateBytes, qint64* handles) {
            PROCESS_MEMORY_COUNTERS_EX counters{};
            counters.cb = sizeof(counters);
            GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                 sizeof(counters));
            DWORD count = 0;
            GetProcessHandleCount(GetCurrentProcess(), &count);
            *privateBytes = qint64(counters.PrivateUsage);
            *handles = qint64(count);
        };

        constexpr int kSwitches = 20;
        QList<HandleEntry> warmCensus;
        int warmThreads = 0;
        qint64 steadyHandles = 0;
        qint64 steadyEndHandles = 0;
        qint64 warmPrivate = 0, warmHandles = 0, peakPrivate = 0, peakHandles = 0;
        QStringList series;
        quint64 shot = 1;
        for (int i = 0; i < kSwitches; ++i) {
            const bool a = i % 2 == 0;
            const ForegroundGame& target = a ? targetA : targetB;
            const auto matches = a ? isFixtureBlue : isFixtureOrange;
            pump.pinTarget(target);
            QVERIFY(pump.hasPinnedTarget());
            if (i == 0)
                pump.restartBuffer();
            QTRY_VERIFY_WITH_TIMEOUT(pump.bufferState() == ReplayBufferState::Recording, 7000);
            QTest::qWait(400);
            const qsizetype before = ready.size() + failed.size();
            pump.captureWindowScreenshot(shot++);
            QTRY_VERIFY_WITH_TIMEOUT(ready.size() + failed.size() > before, 7000);
            QVERIFY2(failed.size() == 0, qPrintable(failed.isEmpty() ? QString()
                                                                       : failed.last().at(0).toString()));
            const QImage image = qvariant_cast<QImage>(ready.last().at(0));
            const QString bad = nonFixtureEdge(image, matches);
            QVERIFY2(bad.isEmpty(), qPrintable(QStringLiteral("switch %1 (%2): %3")
                                                   .arg(i).arg(target.gameName, bad)));
            QCOMPARE(ready.last().at(1).toString(), target.gameName);
            if (i % 5 == 4) {
                QTest::qWait(500);
                const qsizetype savedBefore = saved.size();
                pump.saveReplay(CaptureRequest::create(CaptureRequest::Source::Ui));
                QTRY_VERIFY_WITH_TIMEOUT(saved.size() > savedBefore || !clipFailed.isEmpty(), 15000);
                QVERIFY2(clipFailed.isEmpty(), qPrintable(clipFailed.isEmpty() ? QString()
                                                                       : clipFailed.constFirst().at(1).toString()));
                QCOMPARE(saved.last().at(1).toString(), target.gameName);
                QString error;
                const QList<QImage> frames = decodeAllFrames(saved.last().at(0).toString(), &error);
                QVERIFY2(frames.size() >= 5, qPrintable(error));
                for (qsizetype f = 0; f < frames.size(); ++f) {
                    const QString wrong = nonFixtureEdge(frames[f], matches);
                    QVERIFY2(wrong.isEmpty(), qPrintable(QStringLiteral("switch %1 clip frame %2/%3: %4")
                                                             .arg(i).arg(f).arg(frames.size()).arg(wrong)));
                }
                const QImage preview(saved.last().at(2).toString());
                QVERIFY(!preview.isNull());
                QVERIFY2(nonFixtureEdge(preview, matches).isEmpty(), "preview belongs to another game");
            }
            qint64 privateBytes = 0, handles = 0;
            sample(&privateBytes, &handles);
            series.append(QStringLiteral("%1:%2MB/%3h").arg(i).arg(privateBytes / (1024 * 1024)).arg(handles));
            if (i == 3) {
                warmPrivate = privateBytes;
                warmHandles = handles;
                warmCensus = ownHandles();
                warmThreads = threadCount();
            }
            // Switch 4 performs the first replay export, a one-time warm-up
            // of the export pipeline; steady growth is measured after it.
            if (i == 5)
                steadyHandles = handles;
            // Switch 19 saves again; its export threads may still be alive at
            // the sample, so the steady window ends before it.
            if (i == kSwitches - 2)
                steadyEndHandles = handles;
            if (i > 3) {
                peakPrivate = std::max(peakPrivate, privateBytes);
                peakHandles = std::max(peakHandles, handles);
            }
        }
        qint64 endPrivate = 0, endHandles = 0;
        sample(&endPrivate, &endHandles);
        const QList<HandleEntry> endCensus = ownHandles();
        const int endThreads = threadCount();
        pump.useAutomaticCapture();
        QVERIFY(!pump.hasPinnedTarget());
        QTRY_VERIFY_WITH_TIMEOUT(pump.bufferState() != ReplayBufferState::Recording, 7000);
        QTest::qWait(1500);
        qint64 idlePrivate = 0, idleHandles = 0;
        sample(&idlePrivate, &idleHandles);
        const QList<HandleEntry> idleCensus = ownHandles();
        qInfo().noquote() << QStringLiteral("handle census (%1): warm->end [%2] threads %3->%4; "
                                            "warm->idle [%5] threads %6")
            .arg(screenshotCursor ? QStringLiteral("live") : QStringLiteral("one-shot"))
            .arg(handleGrowth(warmCensus, endCensus)).arg(warmThreads).arg(endThreads)
            .arg(handleGrowth(warmCensus, idleCensus)).arg(threadCount());
        qInfo().noquote() << QStringLiteral("soak warm=%1MB/%2h peak=%3MB/%4h end=%5MB/%6h idle=%7MB/%8h series %9")
            .arg(warmPrivate >> 20).arg(warmHandles).arg(peakPrivate >> 20).arg(peakHandles)
            .arg(endPrivate >> 20).arg(endHandles).arg(idlePrivate >> 20).arg(idleHandles)
            .arg(series.join(u' '));
        // 16 further switches after warm-up. Beta 2 grew by tens of MB and
        // hundreds of handles per switch (#39). The 0.8.2 Beta 4 baseline on
        // an RTX 5090 is about +4 handles per switch while capture runs, all
        // released when capture stops; fail on anything near the old growth.
        QVERIFY2(endHandles - warmHandles < (kSwitches - 4) * 10, qPrintable(series.join(u' ')));
        // b4-handles: Beta 4 kept ~4.5 handles per switch, mostly from a
        // vendor encoder that leaks on every refused activation. With refused
        // encoders skipped, ~1 per switch remains (Windows keeps an ALPC port
        // per capture item). Allow driver variance, catch a return of the leak.
        const qint64 steadyPerTenSwitches = (steadyEndHandles - steadyHandles) * 10 / (kSwitches - 2 - 5);
        qInfo().noquote() << QStringLiteral("steady growth: %1 handles per 10 switches").arg(steadyPerTenSwitches);
        QVERIFY2(steadyPerTenSwitches < 30, qPrintable(series.join(u' ')));
        QVERIFY2(endPrivate - warmPrivate < 48ll * 1024 * 1024, qPrintable(series.join(u' ')));
        QVERIFY2(idleHandles < warmHandles + 30, qPrintable(series.join(u' ')));
        QVERIFY2(idlePrivate < warmPrivate + 16ll * 1024 * 1024, qPrintable(series.join(u' ')));
        QCOMPARE(reuseStops.count(), 0);
    }

    // #41: screenshot and replay pointer settings are independent although
    // both outputs share one WGC session. The fixture sits under the pointer
    // wherever the owner left it (it is never moved), at the bottom of the
    // z-order and never activated. Every output must keep the game-drawn
    // crosshair; the Windows pointer appears only where its setting is on.
    void cursorSettingsStayIndependentForScreenshotsAndReplays_data()
    {
        QTest::addColumn<bool>("hdr");
        QTest::newRow("sdr") << false;
        QTest::newRow("hdr") << true;
    }

    void cursorSettingsStayIndependentForScreenshotsAndReplays()
    {
        QFETCH(bool, hdr);
        if (!qEnvironmentVariableIsSet("GAMEHQ_TEST_WGC_NATIVE"))
            QSKIP("Requires an interactive Windows desktop and a WGC-capable GPU");
        QVERIFY(GameDetector::onInputDesktop());
        const auto previousDpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        QVERIFY(previousDpi);
        const auto restoreDpi = qScopeGuard([&] { SetThreadDpiAwarenessContext(previousDpi); });
        // Without a visible pointer (no mouse attached, or hidden by the
        // foreground app) its pixels cannot be compared. The policy Windows
        // reports for each session and the session serving each screenshot
        // are still verified; pixel checks then only prove nothing leaks.
        CURSORINFO pointer{sizeof(CURSORINFO)};
        QVERIFY(GetCursorInfo(&pointer));
        const bool pointerVisible = (pointer.flags & CURSOR_SHOWING) != 0;
        if (!pointerVisible)
            qWarning() << "Windows reports no visible mouse pointer: pointer pixels are not observable";
        const POINT anchor = pointer.ptScreenPos;
        // Pointer at client (120, 100): the cursor image extends right/down.
        constexpr int kHotX = 120, kHotY = 100;
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        const HBRUSH blue = CreateSolidBrush(RGB(0, 70, 220));
        const HBRUSH magenta = CreateSolidBrush(RGB(255, 0, 255));
        QVERIFY(blue && magenta);
        const auto deleteBrushes = qScopeGuard([&] { DeleteObject(blue); DeleteObject(magenta); });
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = DefWindowProcW;
        windowClass.hInstance = instance;
        windowClass.hbrBackground = blue;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = L"GameHQCursorPolicyFixture";
        QVERIFY(RegisterClassW(&windowClass));
        const auto unregister = qScopeGuard([&] { UnregisterClassW(windowClass.lpszClassName, instance); });
        RECT frame{0, 0, 360, 260};
        AdjustWindowRectEx(&frame, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW);
        const HWND game = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, windowClass.lpszClassName,
            L"Cursor fixture", WS_OVERLAPPEDWINDOW, 0, 0, frame.right - frame.left, frame.bottom - frame.top,
            nullptr, nullptr, instance, nullptr);
        QVERIFY(game);
        const auto destroyGame = qScopeGuard([&] { DestroyWindow(game); });
        const DWORD squareCorners = 1;
        DwmSetWindowAttribute(game, 33, &squareCorners, sizeof(squareCorners));
        QVERIFY(SetWindowPos(game, HWND_BOTTOM, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW));
        // Place the client so the pointer's hotspot lands on (kHotX, kHotY).
        POINT origin{0, 0};
        QVERIFY(ClientToScreen(game, &origin));
        RECT outer{};
        QVERIFY(GetWindowRect(game, &outer));
        QVERIFY(SetWindowPos(game, nullptr, outer.left + anchor.x - kHotX - origin.x,
                             outer.top + anchor.y - kHotY - origin.y, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE));
        RECT client{};
        QVERIFY(GetClientRect(game, &client));
        const QSize clientSize(client.right, client.bottom);
        QVERIFY(clientSize.width() >= 300 && clientSize.height() >= 200);
        UpdateWindow(game);
        QVERIFY(SUCCEEDED(DwmFlush()));

        // Painted by the "game": a magenta crosshair far from the pointer, a
        // phase stripe that identifies which policy step a frame belongs to,
        // and a toggling patch that keeps WGC delivering frames.
        const QRect crosshair(250, 160, 40, 40);
        QColor phaseColour(Qt::black);
        bool alternate = false;
        QTimer repaint;
        connect(&repaint, &QTimer::timeout, this, [&] {
            const HDC dc = GetDC(game);
            if (!dc)
                return;
            RECT h{crosshair.left(), crosshair.center().y() - 2, crosshair.right() + 1, crosshair.center().y() + 3};
            RECT v{crosshair.center().x() - 2, crosshair.top(), crosshair.center().x() + 3, crosshair.bottom() + 1};
            FillRect(dc, &h, magenta);
            FillRect(dc, &v, magenta);
            const HBRUSH stripe = CreateSolidBrush(RGB(phaseColour.red(), phaseColour.green(), phaseColour.blue()));
            RECT band{8, clientSize.height() - 32, 72, clientSize.height() - 8};
            FillRect(dc, &band, stripe);
            DeleteObject(stripe);
            RECT patch{clientSize.width() - 24, 8, clientSize.width() - 8, 24};
            FillRect(dc, &patch, alternate ? blue : GetSysColorBrush(COLOR_WINDOW));
            ReleaseDC(game, dc);
            alternate = !alternate;
        });
        repaint.setTimerType(Qt::PreciseTimer);
        repaint.start(16);

        // Pixels around the hotspot that are not fixture blue: the pointer.
        const auto pointerPixels = [&](const QImage& image) {
            int count = 0;
            for (int y = kHotY - 2; y < kHotY + 30 && y < image.height(); ++y)
                for (int x = kHotX - 2; x < kHotX + 22 && x < image.width(); ++x)
                    if (!isFixtureBlue(image.pixelColor(x, y)))
                        ++count;
            return count;
        };
        const auto crosshairIntact = [&](const QImage& image) {
            const QColor c = image.pixelColor(crosshair.center());
            const QColor arm = image.pixelColor(crosshair.left() + 4, crosshair.center().y());
            return c.red() > 180 && c.blue() > 180 && c.green() < 90
                && arm.red() > 180 && arm.blue() > 180 && arm.green() < 90;
        };
        const auto phaseOf = [&](const QImage& image) {
            return image.pixelColor(40, clientSize.height() - 20);
        };
        const auto samePhase = [](const QColor& a, const QColor& b) {
            return std::abs(a.red() - b.red()) < 50 && std::abs(a.green() - b.green()) < 50
                && std::abs(a.blue() - b.blue()) < 50;
        };
        const auto pointerStill = [&] {
            POINT now{};
            return GetCursorPos(&now) && now.x == anchor.x && now.y == anchor.y;
        };

        QTemporaryDir dir(QCoreApplication::applicationDirPath() + "/wgc-cursor-XXXXXX");
        QVERIFY(dir.isValid());
        ConfigManager config(dir.filePath("config.json"));
        config.setValue(ConfigKeys::CaptureMode, QStringLiteral("whitelist"));
        QVERIFY(config.setCaptureExecutableAllowed(QCoreApplication::applicationFilePath(), true));
        config.setValue(ConfigKeys::ReplayAuto, true);
        config.setValue(ConfigKeys::ReplayFps, 30);
        config.setValue(ConfigKeys::InternalCaptureExperimentalHdr, hdr);
        config.setValue(ConfigKeys::AudioSource, QStringLiteral("off"));
        // Defaults are what existing users get: screenshot off, replay on.
        QCOMPARE(config.value(ConfigKeys::CaptureScreenshotCursor, true).toBool(), false);
        QCOMPARE(config.value(ConfigKeys::ReplayCursor, false).toBool(), true);
        CaptureLocations locations(&config);
        QVERIFY(locations.setBaseRoot(CaptureLocations::Kind::Clips, dir.path()));
        ForegroundGame target;
        target.valid = true;
        target.hwnd = game;
        target.pid = GetCurrentProcessId();
        QVERIFY(ProcessIdentity::creationTimeOf(GetCurrentProcess(), target.processStarted));
        target.w = clientSize.width();
        target.h = clientSize.height();
        target.executablePath = QCoreApplication::applicationFilePath();
        target.gameName = QStringLiteral("Cursor fixture");
        QVERIFY(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)));
        const auto uninitialize = qScopeGuard([] { CoUninitialize(); });
        QVERIFY(SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)));
        const auto shutdown = qScopeGuard([] { MFShutdown(); });
        ReuseStopCounter reuseStops;
        PointerLog pointerLog;
        FramePumpService pump(&config, &locations);
        pump.m_autoTimer->stop();
        QStringList restarts;
        QObject workerSignals;   // destroyed before `restarts` (b5-crash)
        connect(pump.m_worker, &FramePumpWorker::restartRequested, &workerSignals,
                [&restarts](quint64, const QString& reason) { restarts.append(reason); },
                Qt::QueuedConnection);
        pump.pinTarget(target);
        QVERIFY(pump.hasPinnedTarget());
        pump.restartBuffer();
        QTRY_VERIFY_WITH_TIMEOUT(pump.bufferState() == ReplayBufferState::Recording, 7000);
        {
            const QStringList start = pointerLog.take();
            if (start.join(u'\n').contains(QLatin1String("cannot change mouse pointer capture")))
                QSKIP("This Windows build cannot control pointer capture");
            const bool hdrArmed = start.join(u'|').contains(QLatin1String("experimental HDR path armed"));
            if (hdr != hdrArmed)
                QSKIP("The FP16 HDR capture path needs an HDR-active display and GPU support");
            QVERIFY2(start.contains(QStringLiteral("FramePump: mouse pointer captured in the replay session")),
                     qPrintable(start.join(u'|')));
        }
        bool replaySessionCursor = true;

        QSignalSpy ready(&pump, &FramePumpService::windowScreenshotReady);
        QSignalSpy failed(&pump, &FramePumpService::windowScreenshotFailed);
        quint64 nextShot = 1;
        const auto shoot = [&](QImage* image, QString* reason) {
            ready.clear();
            failed.clear();
            const quint64 id = nextShot++;
            QElapsedTimer clock;
            clock.start();
            pump.captureWindowScreenshot(id);
            while (ready.isEmpty() && failed.isEmpty() && clock.elapsed() < 7000)
                QTest::qWait(5);
            if (!failed.isEmpty()) {
                *reason = failed.constFirst().at(0).toString();
                return qint64(-1);
            }
            if (ready.isEmpty()) {
                *reason = QStringLiteral("screenshot %1 timed out").arg(id);
                return qint64(-1);
            }
            *image = qvariant_cast<QImage>(ready.constFirst().at(0));
            return clock.elapsed();
        };
        const QString evidence = qEnvironmentVariable("GAMEHQ_TEST_EVIDENCE_DIR");

        struct Step { const char* name; bool screenshot; bool replay; QColor phase; };
        const Step steps[] = {
            {"defaults: screenshot off, replay on", false, true, QColor(220, 40, 40)},
            {"both on", true, true, QColor(40, 200, 60)},
            {"screenshot on, replay off", true, false, QColor(230, 220, 40)},
            {"both off", false, false, QColor(40, 220, 220)},
        };
        QStringList timings;
        int pixelSteps = 0;
        for (const Step& step : steps) {
            bool verified = false;
            for (int attempt = 0; attempt < 3 && !verified; ++attempt) {
                // A visible pointer that moves cannot be compared at a fixed
                // spot. After three tries the step still verifies everything
                // except pointer pixels; it never moves the pointer itself.
                const bool lastAttempt = attempt == 2;
                config.setValue(ConfigKeys::CaptureScreenshotCursor, step.screenshot);
                config.setValue(ConfigKeys::ReplayCursor, step.replay);
                // The policy reaches the worker as a queued call; paint the new
                // phase only after it so every frame of this phase follows it.
                QTest::qWait(150);
                phaseColour = step.phase;
                QTest::qWait(600);
                // Windows' read-back of the replay session's pointer setting.
                const QStringList applied = pointerLog.take();
                if (step.replay != replaySessionCursor) {
                    const QString line = QStringLiteral("FramePump: mouse pointer %1 in the replay session")
                        .arg(step.replay ? QStringLiteral("captured") : QStringLiteral("hidden"));
                    QVERIFY2(applied.contains(line), qPrintable(QStringLiteral("%1: %2")
                                                                    .arg(step.name, applied.join(u'|'))));
                    replaySessionCursor = step.replay;
                } else {
                    QVERIFY2(applied.isEmpty(), qPrintable(applied.join(u'|')));
                }
                bool comparePixels = pointerVisible && pointerStill();
                if (pointerVisible && !comparePixels && !lastAttempt)
                    continue;
                for (int s = 0; s < 3; ++s) {
                    QImage image;
                    QString reason;
                    const qint64 ms = shoot(&image, &reason);
                    QVERIFY2(ms >= 0, qPrintable(QStringLiteral("%1: %2").arg(step.name, reason)));
                    timings.append(QString::number(ms));
                    QCOMPARE(image.size(), clientSize);
                    QVERIFY2(crosshairIntact(image), step.name);
                    QVERIFY2(samePhase(phaseOf(image), step.phase), step.name);
                    const int pixels = pointerPixels(image);
                    if (!evidence.isEmpty())
                        image.save(QDir(evidence).filePath(QStringLiteral("cursor-shot-%1%2-%3.png")
                            .arg(int(step.screenshot)).arg(int(step.replay)).arg(s)));
                    // A differing policy must be served by a one-shot session
                    // with the screenshot's own setting; a matching one by the
                    // replay session's frame.
                    const QStringList served = pointerLog.take();
                    const QString oneShot = QStringLiteral("one-shot session (pointer %1,")
                        .arg(step.screenshot ? QStringLiteral("on") : QStringLiteral("off"));
                    const bool usedOneShot = served.size() == 1 && served.constFirst().contains(oneShot);
                    // The replay session's frame must postdate its last
                    // pointer-setting change, never a queued older frame.
                    static const QRegularExpression liveAge(
                        QStringLiteral("frame (-?\\d+) ms after the pointer setting"));
                    const QRegularExpressionMatch live = served.size() == 1
                        ? liveAge.match(served.constFirst()) : QRegularExpressionMatch();
                    const bool usedLive = live.hasMatch() && live.captured(1).toLongLong() >= 0;
                    QVERIFY2(step.screenshot != step.replay ? usedOneShot : usedLive,
                             qPrintable(QStringLiteral("%1: %2").arg(step.name, served.join(u'|'))));
                    if (pointerVisible && !comparePixels)
                        {}  // moving pointer: pixels not comparable
                    else if (!pointerVisible)
                        QVERIFY2(pixels == 0, qPrintable(QStringLiteral("%1: %2 foreign px").arg(step.name).arg(pixels)));
                    else if (step.screenshot)
                        QVERIFY2(pixels >= 20, qPrintable(QStringLiteral("%1: pointer missing (%2 px)")
                                                              .arg(step.name).arg(pixels)));
                    else
                        QVERIFY2(pixels == 0, qPrintable(QStringLiteral("%1: pointer leaked (%2 px)")
                                                             .arg(step.name).arg(pixels)));
                }
                QTest::qWait(300);
                QSignalSpy saved(&pump, &FramePumpService::clipSaved);
                QSignalSpy clipFailed(&pump, &FramePumpService::clipFailed);
                pump.saveReplay(CaptureRequest::create(CaptureRequest::Source::Ui));
                QTRY_VERIFY_WITH_TIMEOUT(!saved.isEmpty() || !clipFailed.isEmpty(), 15000);
                QVERIFY2(clipFailed.isEmpty(), qPrintable(clipFailed.isEmpty() ? QString()
                                                              : clipFailed.constFirst().at(1).toString()));
                if (comparePixels && !pointerStill()) {
                    if (!lastAttempt)
                        continue;
                    comparePixels = false;
                }
                QString error;
                const QList<QImage> frames = decodeAllFrames(saved.constFirst().at(0).toString(), &error);
                int phaseFrames = 0;
                for (qsizetype f = 0; f < frames.size(); ++f) {
                    if (!samePhase(phaseOf(frames[f]), step.phase))
                        continue;
                    ++phaseFrames;
                    QVERIFY2(crosshairIntact(frames[f]), qPrintable(QStringLiteral("%1: frame %2 lost the crosshair")
                                                                       .arg(step.name).arg(f)));
                    const int pixels = pointerPixels(frames[f]);
                    if (!evidence.isEmpty() && phaseFrames == 1)
                        frames[f].save(QDir(evidence).filePath(QStringLiteral("cursor-clip-%1%2.png")
                            .arg(int(step.screenshot)).arg(int(step.replay))));
                    if (pointerVisible && !comparePixels)
                        {}  // moving pointer: pixels not comparable
                    else if (!pointerVisible)
                        QVERIFY2(pixels <= 2, qPrintable(QStringLiteral("%1: clip frame %2 has %3 foreign px")
                                                             .arg(step.name).arg(f).arg(pixels)));
                    else if (step.replay)
                        QVERIFY2(pixels >= 20, qPrintable(QStringLiteral("%1: clip frame %2 lacks the pointer (%3 px)")
                                                              .arg(step.name).arg(f).arg(pixels)));
                    else
                        QVERIFY2(pixels <= 2, qPrintable(QStringLiteral("%1: clip frame %2 shows the pointer (%3 px)")
                                                             .arg(step.name).arg(f).arg(pixels)));
                }
                // The phase spans all three screenshots: none of them may have
                // changed the pointer in the recording.
                QVERIFY2(phaseFrames >= 15, qPrintable(QStringLiteral("%1: only %2 of %3 frames in phase %4")
                                                           .arg(step.name).arg(phaseFrames).arg(frames.size())
                                                           .arg(error)));
                qInfo().noquote() << QStringLiteral("%1: 3 screenshots and %2 clip frames verified (%3)")
                                         .arg(step.name).arg(phaseFrames)
                                         .arg(comparePixels ? QStringLiteral("pointer pixels compared")
                                              : pointerVisible ? QStringLiteral("pointer moved, pixels not compared")
                                                               : QStringLiteral("no visible pointer"));
                if (comparePixels)
                    ++pixelSteps;
                verified = true;
            }
            QVERIFY(verified);
        }
        qInfo().noquote() << QStringLiteral("pointer pixels compared in %1 of 4 steps").arg(pixelSteps);
        QVERIFY2(restarts.isEmpty(), qPrintable(restarts.join(u"; ")));
        QCOMPARE(reuseStops.count(), 0);
        if (!pointerVisible)
            qWarning() << "pointer pixels were not observable; policy read-back and session routing verified";

        // Repeated one-shot screenshots (pointer off while the replay draws
        // it) must not accumulate sessions, textures or handles.
        config.setValue(ConfigKeys::CaptureScreenshotCursor, false);
        config.setValue(ConfigKeys::ReplayCursor, true);
        QTest::qWait(300);
        auto sample = [](qint64* privateBytes, qint64* handles) {
            PROCESS_MEMORY_COUNTERS_EX counters{};
            counters.cb = sizeof(counters);
            GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                 sizeof(counters));
            *privateBytes = qint64(counters.PrivateUsage);
            DWORD count = 0;
            GetProcessHandleCount(GetCurrentProcess(), &count);
            *handles = count;
        };
        qint64 warmPrivate = 0, warmHandles = 0, endPrivate = 0, endHandles = 0;
        constexpr int kShots = 60;
        for (int s = 0; s < kShots; ++s) {
            if (s == 10)
                sample(&warmPrivate, &warmHandles);
            QImage image;
            QString reason;
            QVERIFY2(shoot(&image, &reason) >= 0, qPrintable(QStringLiteral("soak %1: %2").arg(s).arg(reason)));
            QVERIFY(crosshairIntact(image));
        }
        ready.clear();
        QTest::qWait(500);
        sample(&endPrivate, &endHandles);
        qInfo().noquote() << QStringLiteral("cursor soak: %1 one-shot screenshots, warm %2MB/%3h end %4MB/%5h, "
                                            "latency ms [%6]")
            .arg(kShots).arg(warmPrivate >> 20).arg(warmHandles).arg(endPrivate >> 20).arg(endHandles)
            .arg(timings.join(u' '));
        QVERIFY2(endHandles - warmHandles < 10, qPrintable(QStringLiteral("%1 -> %2 handles")
                                                              .arg(warmHandles).arg(endHandles)));
        QVERIFY2(endPrivate - warmPrivate < 24ll * 1024 * 1024, "private bytes grew");
        QVERIFY(pump.bufferState() == ReplayBufferState::Recording
                || pump.bufferState() == ReplayBufferState::Ready);

        // Replay off: the screenshot-only session itself follows the
        // screenshot setting, so no one-shot session is needed.
        config.setValue(ConfigKeys::ReplayAuto, false);
        pump.restartBuffer();
        QTest::qWait(500);
        pointerLog.take();
        for (const bool shotCursor : {true, false, true}) {
            config.setValue(ConfigKeys::CaptureScreenshotCursor, shotCursor);
            QTest::qWait(300);
            QImage image;
            QString reason;
            QVERIFY2(shoot(&image, &reason) >= 0, qPrintable(QStringLiteral("replay off: ") + reason));
            QVERIFY(crosshairIntact(image));
            QTest::qWait(100);
            const QStringList lines = pointerLog.take();
            // The live session's frame must have been composed after the
            // setting took effect; a static scene may fall back to a fresh
            // one-shot session with the same setting instead.
            static const QRegularExpression age(QStringLiteral("frame (-?\\d+) ms after the pointer setting"));
            bool fresh = false;
            for (const QString& line : lines) {
                const QRegularExpressionMatch m = age.match(line);
                if (m.hasMatch())
                    fresh = m.captured(1).toLongLong() >= 0;
                else if (line.contains(QStringLiteral("one-shot session (pointer %1,")
                                           .arg(shotCursor ? QStringLiteral("on") : QStringLiteral("off"))))
                    fresh = true;
            }
            QVERIFY2(fresh, qPrintable(lines.join(u'|')));
            QVERIFY2(lines.contains(QStringLiteral("FramePump: mouse pointer %1 in the screenshot session")
                                        .arg(shotCursor ? QStringLiteral("captured") : QStringLiteral("hidden"))),
                     qPrintable(lines.join(u'|')));
            const int pixels = pointerPixels(image);
            if (pointerVisible && !pointerStill())
                {}  // moving pointer: pixels not comparable
            else if (!pointerVisible || !shotCursor)
                QVERIFY2(pixels == 0, qPrintable(QStringLiteral("replay off: %1 foreign px").arg(pixels)));
            else
                QVERIFY2(pixels >= 20, qPrintable(QStringLiteral("replay off: pointer missing (%1 px)").arg(pixels)));
        }
        QVERIFY(pump.bufferState() != ReplayBufferState::Recording);
        config.setValue(ConfigKeys::ReplayAuto, true);
        config.setValue(ConfigKeys::CaptureScreenshotCursor, false);
        pump.restartBuffer();
        QTRY_VERIFY_WITH_TIMEOUT(pump.bufferState() == ReplayBufferState::Recording, 7000);
        QTest::qWait(300);

        // A request cancelled by a target change fails cleanly and leaves no
        // one-shot session behind.
        failed.clear();
        pump.captureWindowScreenshot(nextShot++);
        pump.useAutomaticCapture();
        QTRY_VERIFY_WITH_TIMEOUT(!failed.isEmpty() || !ready.isEmpty(), 7000);
        QTRY_VERIFY_WITH_TIMEOUT(pump.bufferState() != ReplayBufferState::Recording, 7000);
        QVERIFY(restarts.isEmpty());
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

    // b4-handles attribution: repeats each layer of a capture pipeline
    // build/teardown in isolation and reports which kernel handle types
    // outlive it. Diagnostic only; asserts nothing about drivers.
    void handleGrowthByPipelineLayer()
    {
        if (!qEnvironmentVariableIsSet("GAMEHQ_TEST_HANDLE_PROBE"))
            QSKIP("Diagnostic probe; set GAMEHQ_TEST_HANDLE_PROBE with GAMEHQ_TEST_WGC_NATIVE");
        QVERIFY(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)));
        const auto uninitialize = qScopeGuard([] { CoUninitialize(); });
        QVERIFY(SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_FULL)));
        const auto shutdown = qScopeGuard([] { MFShutdown(); });
        const HWND window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"STATIC", L"probe",
                                            WS_POPUP, 40, 40, 320, 240, nullptr, nullptr, nullptr, nullptr);
        QVERIFY(window);
        const auto destroy = qScopeGuard([&] { DestroyWindow(window); });
        SetWindowPos(window, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        QTemporaryDir dir(QCoreApplication::applicationDirPath() + "/handle-probe-XXXXXX");
        QVERIFY(dir.isValid());
        FramePumpWorker worker;

        enum class Layer { Device, Session, SessionNoAccess, Encoder, EnumOnly, EncoderOnly,
                           BareActivate, ItemOnly };
        QString encoderName;
        auto build = [&](Layer layer) {
            auto pipe = std::make_unique<FramePumpWorker::Pipeline>();
            pipe->processStarted = 0;
            if (layer == Layer::EnumOnly) {
                MFT_REGISTER_TYPE_INFO out{ MFMediaType_Video, MFVideoFormat_H264 };
                IMFActivate** acts = nullptr;
                UINT32 count = 0;
                if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                                     MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                                     nullptr, &out, &acts, &count)))
                    return false;
                for (UINT32 i = 0; i < count; ++i)
                    acts[i]->Release();
                CoTaskMemFree(acts);
                return true;
            }
            if (layer == Layer::BareActivate) {
                // Nothing of GameHQ's: activate the first hardware encoder,
                // then shut it down exactly as Media Foundation documents.
                MFT_REGISTER_TYPE_INFO out{ MFMediaType_Video, MFVideoFormat_H264 };
                IMFActivate** acts = nullptr;
                UINT32 count = 0;
                if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                                     MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                                     nullptr, &out, &acts, &count)) || count == 0)
                    return false;
                WCHAR* name = nullptr;
                UINT32 length = 0;
                if (SUCCEEDED(acts[0]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &length))) {
                    encoderName = QString::fromWCharArray(name);
                    CoTaskMemFree(name);
                }
                IMFTransform* mft = nullptr;
                const bool activated = SUCCEEDED(acts[0]->ActivateObject(IID_PPV_ARGS(&mft))) && mft;
                if (mft)
                    mft->Release();
                acts[0]->ShutdownObject();
                for (UINT32 i = 0; i < count; ++i)
                    acts[i]->Release();
                CoTaskMemFree(acts);
                return activated;
            }
            if (layer == Layer::EncoderOnly) {
                HwH264Encoder encoder;
                QString why;
                const bool opened = encoder.open({320, 240, 30, 4000000}, &why);
                encoder.close();
                return opened;
            }
            if (!worker.createDevices(pipe.get()))
                return false;
            if (layer == Layer::ItemOnly) {
                int w = 0, h = 0;
                ProcessIdentity::creationTimeOf(GetCurrentProcess(), pipe->processStarted);
                return worker.createCaptureItem(pipe.get(), window, GetCurrentProcessId(), &w, &h);
            }
            if (layer == Layer::Session || layer == Layer::SessionNoAccess) {
                int w = 0, h = 0;
                pipe->processStarted = 0;
                ProcessIdentity::creationTimeOf(GetCurrentProcess(), pipe->processStarted);
                if (!worker.createCaptureItem(pipe.get(), window, GetCurrentProcessId(), &w, &h))
                    return false;
                pipe->clientSize = QSize(w, h);
                if (!worker.createSession(pipe.get(), window, w, h, false,
                                          CaptureBorder::SessionPolicy(layer == Layer::Session)))
                    return false;
                QTest::qWait(60);
            }
            if (layer == Layer::Encoder) {
                D3D11_TEXTURE2D_DESC desc{};
                desc.Width = 320; desc.Height = 240; desc.MipLevels = desc.ArraySize = 1;
                desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
                desc.Usage = D3D11_USAGE_DEFAULT;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                ID3D11Texture2D* texture = nullptr;
                if (FAILED(pipe->d3dDevice->CreateTexture2D(&desc, nullptr, &texture)))
                    return false;
                auto* recorder = new SegmentRecorder();
                const bool begun = recorder->begin(320, 240, 320, 240, 30, 4, 5, 30,
                                                   dir.filePath(QStringLiteral("cache")),
                                                   pipe->d3dDevice, pipe->d3dCtx, 0, 0,
                                                   SegmentRecorder::CacheHistory::SessionOnly);
                for (int f = 0; begun && f < 20; ++f)
                    recorder->writeFrame(texture, pipe->d3dDevice, pipe->d3dCtx, qint64(f) * 333333);
                recorder->end();
                delete recorder;
                texture->Release();
                if (!begun)
                    return false;
            }
            return true;
        };
        const struct { const char* name; Layer layer; } layers[] = {
            {"D3D11 device + WinRT bridge", Layer::Device},
            {"device + WGC item, frame pool, session", Layer::Session},
            {"device + WGC session without border access request", Layer::SessionNoAccess},
            {"device + H.264 segment recorder", Layer::Encoder},
            {"MFTEnumEx hardware H.264 only", Layer::EnumOnly},
            {"HwH264Encoder open + close only", Layer::EncoderOnly},
            {"bare MFT ActivateObject + ShutdownObject", Layer::BareActivate},
            {"device + WGC capture item only", Layer::ItemOnly},
        };
        // Every hardware H.264 encoder on its own: activate + shut down.
        {
            MFT_REGISTER_TYPE_INFO out{ MFMediaType_Video, MFVideoFormat_H264 };
            IMFActivate** acts = nullptr;
            UINT32 count = 0;
            QVERIFY(SUCCEEDED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                                        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                                        nullptr, &out, &acts, &count)));
            for (UINT32 index = 0; index < count; ++index) {
                WCHAR* name = nullptr;
                UINT32 length = 0;
                QString label = QStringLiteral("#%1").arg(index);
                if (SUCCEEDED(acts[index]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &length))) {
                    label += QLatin1Char(' ') + QString::fromWCharArray(name);
                    CoTaskMemFree(name);
                }
                auto once = [&] {
                    IMFTransform* mft = nullptr;
                    const HRESULT hr = acts[index]->ActivateObject(IID_PPV_ARGS(&mft));
                    if (mft)
                        mft->Release();
                    acts[index]->ShutdownObject();
                    return hr;
                };
                const HRESULT first = once();
                once();
                QTest::qWait(300);
                const QList<HandleEntry> before = ownHandles();
                for (int i = 0; i < 20; ++i)
                    once();
                QTest::qWait(1500);
                qInfo().noquote() << QStringLiteral("probe encoder %1 (activate hr=0x%2) x20: [%3]")
                    .arg(label).arg(quint32(first), 8, 16, QLatin1Char('0'))
                    .arg(handleGrowth(before, ownHandles()));
            }
            for (UINT32 index = 0; index < count; ++index)
                acts[index]->Release();
            CoTaskMemFree(acts);
        }
        for (const auto& entry : layers) {
            for (int i = 0; i < 3; ++i)
                QVERIFY2(build(entry.layer), entry.name);
            QTest::qWait(300);
            const QList<HandleEntry> before = ownHandles();
            const int threadsBefore = threadCount();
            constexpr int kRounds = 20;
            for (int i = 0; i < kRounds; ++i)
                QVERIFY2(build(entry.layer), entry.name);
            QTest::qWait(1500);
            const QList<HandleEntry> after = ownHandles();
            qInfo().noquote() << QStringLiteral("probe %1 x%2: %3 -> %4 handles [%5] threads %6->%7")
                .arg(QLatin1String(entry.name) + (entry.layer == Layer::BareActivate
                                                      ? QStringLiteral(" (%1)").arg(encoderName) : QString())).arg(kRounds).arg(before.size()).arg(after.size())
                .arg(handleGrowth(before, after)).arg(threadsBefore).arg(threadCount());
        }
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
