// FramePumpWorker: thread/apartment lifecycle, capture pipeline bring-up and
// teardown, and the per-frame poll. The replay save stages live in
// ReplaySaveCoordinator.cpp; the GUI-thread front-end in FramePumpService.cpp.
#include "capture/FramePumpPipeline.h"

#include "capture/wgc_shims.h"
#include "capture/AudioCapture.h"
#include "capture/AudioSourcePolicy.h"
#include "capture/CaptureBorderState.h"
#include "capture/CaptureUtil.h"
#include "capture/HdrCapabilities.h"
#include "capture/hdr/GpuToneMapper.h"
#include "capture/hdr/HdrToneMapMath.h"
#include "capture/SegmentRecorder.h"
#include "capture/ReplayExportTask.h"
#include "config/Paths.h"
#include "core/GameIdentity.h"

#include <QDebug>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImage>
#include <QScopeGuard>
#include <QSize>
#include <QThread>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <memory>

#include <algorithm>
#include <cstring>
#include <cwchar>

#include <roapi.h>       // RoInitialize / RoUninitialize / RoGetActivationFactory
#include <winstring.h>   // WindowsCreateString / WindowsDeleteString

// ===========================================================================
//  FramePumpWorker — all WGC/D3D work, runs on a dedicated MTA thread
// ===========================================================================

namespace { void sweepStaleReplayCache(); }   // defined below with the helpers

namespace {

// GraphicsCaptureAccess.RequestAccessAsync(Borderless), resolved once per process.
// Must run on the MTA worker thread. Returns an AppCapabilityAccessStatus value, or
// CaptureBorder::AccessNotRequested when the request could not even be started.
//
// p4-1 measured UserPromptRequired here for this unpackaged build — the OS will not
// hand Borderless access to a process without package identity, and there is nowhere
// to show the consent prompt from a background capture thread. We record that answer
// honestly instead of ignoring it.
int requestBorderlessAccess()
{
    static const int status = []() -> int {
        HSTRING cls = nullptr;
        if (FAILED(WindowsCreateString(kWgcCaptureAccessClass,
                                       UINT32(wcslen(kWgcCaptureAccessClass)), &cls)))
            return CaptureBorder::AccessNotRequested;
        const auto releaseCls = qScopeGuard([&] { WindowsDeleteString(cls); });

        IGraphicsCaptureAccessStatics* statics = nullptr;
        HRESULT hr = RoGetActivationFactory(cls, IID_IGraphicsCaptureAccessStatics,
                                            reinterpret_cast<void**>(&statics));
        if (FAILED(hr) || !statics) {
            qInfo().nospace() << "FramePump: GraphicsCaptureAccess unavailable hr=0x"
                              << Qt::hex << quint32(hr);
            return CaptureBorder::AccessNotRequested;
        }

        IAsyncOperationAppCapabilityAccessStatus* op = nullptr;
        hr = statics->RequestAccessAsync(GraphicsCaptureAccessKind_Borderless, &op);
        statics->Release();
        if (FAILED(hr) || !op) {
            qInfo().nospace() << "FramePump: RequestAccessAsync(Borderless) failed hr=0x"
                              << Qt::hex << quint32(hr);
            return CaptureBorder::AccessNotRequested;
        }
        const auto releaseOp = qScopeGuard([&] { op->Release(); });

        IAsyncInfoShim* info = nullptr;
        if (FAILED(op->QueryInterface(IID_IAsyncInfoShim, reinterpret_cast<void**>(&info))) || !info)
            return CaptureBorder::AccessNotRequested;
        const auto releaseInfo = qScopeGuard([&] { info->Close(); info->Release(); });

        // Bounded wait: this runs once, during pipeline bring-up, and must never
        // wedge the worker if the broker never answers.
        INT32 asyncStatus = WgcAsyncStatus_Started;
        for (int i = 0; i < 100; ++i) {          // <= ~2 s
            if (FAILED(info->get_Status(&asyncStatus)))
                return CaptureBorder::AccessNotRequested;
            if (asyncStatus != WgcAsyncStatus_Started)
                break;
            Sleep(20);
        }
        if (asyncStatus != WgcAsyncStatus_Completed)
            return CaptureBorder::AccessNotRequested;

        INT32 result = CaptureBorder::AccessNotRequested;
        if (FAILED(op->GetResults(&result)))
            return CaptureBorder::AccessNotRequested;
        return int(result);
    }();
    return status;
}

} // namespace

FramePumpWorker::FramePumpWorker(QObject* parent)
    : QObject(parent)
{
}

FramePumpWorker::~FramePumpWorker()
{
    finishExport();
    teardown();
}

void FramePumpWorker::onThreadStarted()
{
    // Fresh worker thread → uninitialised apartment → MTA is available.
    const HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
    m_apartmentReady = SUCCEEDED(hr);
    if (!m_apartmentReady)
        qWarning().nospace() << "FramePump: RoInitialize(MTA) failed hr=0x"
                             << Qt::hex << quint32(hr);
    else
        qInfo() << "FramePump: worker thread MTA ready";

    // One-time cleanup of segments orphaned by a previous session (Step 9);
    // file IO stays off the GUI thread.
    sweepStaleReplayCache();
}

void FramePumpWorker::onThreadFinished()
{
    finishExport();
    teardown();
    if (m_apartmentReady) {
        RoUninitialize();
        m_apartmentReady = false;
    }
}

void FramePumpWorker::finishExport()
{
    ++m_exportGeneration;
    m_exportTask.reset(); // waits without relying on the worker event loop
    m_exportBusy = false;
}

void FramePumpWorker::teardown()
{
    if (m_pipe) {
        delete m_pipe;   // ~Pipeline releases every WinRT/D3D object + the timer
        m_pipe = nullptr;
    }
}

namespace
{
QSize fitInsideEven(QSize source, QSize limit)
{
    if (source.width() < 2 || source.height() < 2)
        return source;
    if (limit.width() < 2 || limit.height() < 2)
        return source;
    if (source.width() <= limit.width() && source.height() <= limit.height())
        return QSize(source.width() & ~1, source.height() & ~1);
    const double scale = std::min(double(limit.width()) / double(source.width()),
                                  double(limit.height()) / double(source.height()));
    int w = std::max(2, int(source.width() * scale)) & ~1;
    int h = std::max(2, int(source.height() * scale)) & ~1;
    return QSize(w, h);
}

long long qpcNow100ns()
{
    return CaptureUtil::qpcNow100ns();
}

QImage readBgraTexture(ID3D11Texture2D* texture, ID3D11Device* device,
                       ID3D11DeviceContext* context)
{
    if (!texture || !device || !context)
        return {};

    D3D11_TEXTURE2D_DESC sourceDesc{};
    texture->GetDesc(&sourceDesc);
    if (sourceDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM
        || sourceDesc.Width == 0 || sourceDesc.Height == 0)
        return {};

    D3D11_TEXTURE2D_DESC stagingDesc = sourceDesc;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.SampleDesc.Quality = 0;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    ID3D11Texture2D* staging = nullptr;
    if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &staging)) || !staging)
        return {};

    context->CopyResource(staging, texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT mapHr = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(mapHr)) {
        staging->Release();
        return {};
    }

    QImage image(int(sourceDesc.Width), int(sourceDesc.Height), QImage::Format_ARGB32);
    const size_t rowBytes = size_t(sourceDesc.Width) * 4;
    for (UINT y = 0; y < sourceDesc.Height; ++y) {
        std::memcpy(image.scanLine(int(y)),
                    static_cast<const unsigned char*>(mapped.pData)
                        + size_t(y) * mapped.RowPitch,
                    rowBytes);
    }
    context->Unmap(staging, 0);
    staging->Release();
    return image;
}

// Step 9 — stale replay-cache cleanup. On worker start, drop segment files a
// previous session left behind (same 10-minute threshold the ring restore in
// SegmentRecorder::begin uses, so a quick app restart keeps its ring) and
// prune emptied per-game cache folders.
void sweepStaleReplayCache()
{
    const QString root = Paths::replayCacheDir();
    const qint64 nowSecs = QDateTime::currentSecsSinceEpoch();
    int removed = 0;
    QDirIterator it(root, QStringList() << QStringLiteral("*_clip.mp4"),
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString f = it.next();
        if (nowSecs - QFileInfo(f).lastModified().toSecsSinceEpoch()
                > CaptureUtil::kStaleSegmentMaxAgeSecs
            && SegmentLease::removeIfUnleased(f))
            ++removed;
    }
    QDir rootDir(root);
    const QStringList games = rootDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& g : games) {
        QDir gdir(rootDir.filePath(g));
        const QStringList subs = gdir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString& sub : subs) {
            if (QDir(gdir.filePath(sub)).isEmpty())
                gdir.rmdir(sub);
        }
        if (gdir.isEmpty())
            rootDir.rmdir(g);
    }
    if (removed > 0)
        qInfo() << "FramePump: stale replay-cache sweep removed" << removed
                << "orphaned segment(s)";
}

} // namespace

// Every start-stage failure carries the command generation back to the GUI.
bool FramePumpWorker::failStep(const char* step, long hr)
{
    const QString reason = QStringLiteral("%1 failed hr=0x%2")
                               .arg(QLatin1String(step))
                               .arg(quint32(hr), 8, 16, QLatin1Char('0'));
    qWarning() << "FramePump:" << reason;
    reportBufferState(ReplayBufferState::Failed, reason);
    return false;
}

// 1-2. D3D11 device (BGRA support is mandatory for WGC) bridged to a WinRT
//      IDirect3DDevice.
bool FramePumpWorker::createDevices(Pipeline* pipe)
{
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                   D3D11_SDK_VERSION, &pipe->d3dDevice, nullptr,
                                   &pipe->d3dCtx);
    if (FAILED(hr)) return failStep("D3D11CreateDevice", hr);

    // The bridge export lives in d3d11.dll but the mingw import lib may not expose
    // the symbol, so resolve it at runtime (exactly as the proven spike does).
    // Resolved once per process (function-local static) — the previous per-arm
    // LoadLibraryW leaked a module reference on every start.
    static const auto pfnBridge = []() -> PFN_CreateDirect3D11DeviceFromDXGIDevice {
        const HMODULE d3dll = LoadLibraryExW(L"d3d11.dll", nullptr,
                                             LOAD_LIBRARY_SEARCH_SYSTEM32);
        return d3dll ? reinterpret_cast<PFN_CreateDirect3D11DeviceFromDXGIDevice>(
                           GetProcAddress(d3dll, "CreateDirect3D11DeviceFromDXGIDevice"))
                     : nullptr;
    }();
    if (!pfnBridge)
        return failStep("resolve CreateDirect3D11DeviceFromDXGIDevice", E_FAIL);

    IDXGIDevice* dxgiDevice = nullptr;
    hr = pipe->d3dDevice->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice));
    if (FAILED(hr)) return failStep("QI IDXGIDevice", hr);

    IInspectable* inspDevice = nullptr;
    hr = pfnBridge(dxgiDevice, &inspDevice);
    dxgiDevice->Release();
    if (FAILED(hr) || !inspDevice)
        return failStep("CreateDirect3D11DeviceFromDXGIDevice", hr);

    hr = inspDevice->QueryInterface(IID_IDirect3DDevice, reinterpret_cast<void**>(&pipe->winrtDevice));
    inspDevice->Release();
    if (FAILED(hr)) return failStep("QI IDirect3DDevice", hr);
    return true;
}

// 3. HWND → GraphicsCaptureItem via the interop activation factory.
bool FramePumpWorker::createCaptureItem(Pipeline* pipe, void* hwndPtr, int* outW, int* outH)
{
    IGraphicsCaptureItemInterop* interop = nullptr;
    HSTRING itemClass = nullptr;
    WindowsCreateString(kWgcCaptureItemClass, UINT32(wcslen(kWgcCaptureItemClass)), &itemClass);
    HRESULT hr = RoGetActivationFactory(itemClass, IID_IGraphicsCaptureItemInterop,
                                        reinterpret_cast<void**>(&interop));
    if (itemClass) WindowsDeleteString(itemClass);
    if (FAILED(hr) || !interop)
        return failStep("RoGetActivationFactory(interop)", hr);

    hr = interop->CreateForWindow(static_cast<HWND>(hwndPtr), IID_IGraphicsCaptureItem,
                                  reinterpret_cast<void**>(&pipe->item));
    interop->Release();
    if (FAILED(hr) || !pipe->item) return failStep("CreateForWindow", hr);

    WgcSizeInt32 size{ 0, 0 };
    hr = pipe->item->get_Size(&size);
    if (FAILED(hr)) return failStep("item->get_Size", hr);
    qInfo() << "FramePump: capture item size" << size.Width << "x" << size.Height;
    *outW = size.Width;
    *outH = size.Height;
    return true;
}

// 3b. Rolling H.264 buffer (Step 4/5) into the TEMPORARY segment ring in
//     replay-cache/ (sized to ~lengthSeconds, oldest deleted). This is NOT the
//     saved clip — Share-hold snapshots this ring and remuxes it into one file
//     under <capturesRoot>/<Game>/Clips/ (Step 6/8, saveReplayOnWorker).
//
// A failed recorder may still serve HDR screenshots through capture-only WGC,
// but must report Failed for replay instead of claiming that it is recording.
void FramePumpWorker::attachRecorder(Pipeline* pipe, unsigned long pid, int srcW, int srcH,
                                     int encodeWidth, int encodeHeight, int fps, int bitrateMbps,
                                     int segmentSeconds, int lengthSeconds, int audioSource)
{
    // Record the configured source: the game's own process tree by default,
    // the default output on request, or nothing. When audio is wanted it is
    // never silently dropped, and game audio never silently becomes desktop
    // audio: a clip without the requested sound looks like a success to the
    // user, so a start that keeps failing fails the replay buffer with a
    // reason the save notification can show.
    const auto source = AudioSourcePolicy::Source(audioSource);
    if (source != AudioSourcePolicy::Source::Off) {
        const bool game = source == AudioSourcePolicy::Source::Game;
        const AudioCapture::Mode mode = game ? AudioCapture::Mode::Game
                                             : AudioCapture::Mode::Desktop;
        const bool supported = !game || AudioCapture::processLoopbackSupported();
        pipe->audioFailure = !game ? ReplayBufferState::audioFailure()
                           : supported ? ReplayBufferState::gameAudioFailure()
                                       : ReplayBufferState::gameAudioUnsupported();
        constexpr int kAudioStartAttempts = 3;
        for (int attempt = 1; supported && attempt <= kAudioStartAttempts && !pipe->audio;
             ++attempt) {
            auto audio = std::make_unique<AudioCapture>();
            if (audio->start(mode, pid)) {
                pipe->audio = audio.release();   // owned by Pipeline from here
                break;
            }
            audio.reset();
            qWarning().noquote() << QStringLiteral("FramePump: %1 start failed (attempt %2/%3)")
                                        .arg(game ? QStringLiteral("game audio")
                                                  : QStringLiteral("desktop audio"))
                                        .arg(attempt).arg(kAudioStartAttempts);
            if (attempt < kAudioStartAttempts)
                QThread::msleep(250);   // endpoint switches settle within a few hundred ms
        }
        if (!pipe->audio) {
            qWarning().noquote() << QStringLiteral(
                "FramePump: %1 is selected but cannot be captured — replay buffer fails "
                "instead of recording video-only or another source")
                .arg(game ? QStringLiteral("game audio") : QStringLiteral("desktop audio"));
            reportBufferState(ReplayBufferState::Failed, pipe->audioFailure);
            pipe->encClock.start();
            pipe->encStartQpc100ns = qpcNow100ns();
            return;
        }
        pipe->audioBuf.resize(int(pipe->audio->bufferFrames() * pipe->audio->channels()));
        qInfo() << "FramePump: audio attached";
    }
    const QSize encodeSize = fitInsideEven(QSize(srcW, srcH), QSize(encodeWidth, encodeHeight));
    QString cacheProfile = QStringLiteral("%1-%2x%3-%4fps")
                               // Never restore desktop-audio segments into a
                               // game-audio ring (or the reverse) after a switch.
                               .arg(!pipe->audio ? QStringLiteral("video")
                                    : pipe->audio->mode() == AudioCapture::Mode::Game
                                        ? QStringLiteral("gameaudio")
                                        : QStringLiteral("audio"))
                               .arg(encodeSize.width())
                               .arg(encodeSize.height())
                               .arg(fps);
    if (pipe->audio) {
        cacheProfile += QStringLiteral("-%1hz-%2ch")
                            .arg(pipe->audio->sampleRate())
                            .arg(pipe->audio->channels());
    }
    const QString cacheDir = Paths::replayCacheDir() + QLatin1Char('/')
                             + GameIdentity::pathComponent(pipe->gameName) + QLatin1Char('/')
                             + cacheProfile;
    // restoreRing() and pruning work inside this folder, so it must never
    // resolve outside the replay cache.
    if (!GameIdentity::isInsideRoot(Paths::replayCacheDir(), cacheDir)) {
        qWarning() << "FramePump: refusing a replay cache folder outside the cache root"
                   << cacheDir << "— capture-only";
        reportBufferState(ReplayBufferState::Failed,
                          QStringLiteral("Replay recorder could not start"));
    } else if (pipe->recorder = new SegmentRecorder();
               !pipe->recorder->begin(srcW, srcH, encodeSize.width(), encodeSize.height(),
                               fps, bitrateMbps, segmentSeconds, lengthSeconds, cacheDir,
                               pipe->d3dDevice, pipe->d3dCtx,
                               pipe->audio ? pipe->audio->sampleRate() : 0,
                               pipe->audio ? pipe->audio->channels() : 0)) {
        qWarning() << "FramePump: segment recorder failed to start — capture-only";
        delete pipe->recorder;
        pipe->recorder = nullptr;
        reportBufferState(ReplayBufferState::Failed,
                          QStringLiteral("Replay recorder could not start"));
    } else if (pipe->audio && !pipe->recorder->hasAudio()) {
        // The AAC encoder refused the audio input; the recorder would carry on
        // video-only, which is exactly the silent clip this must prevent.
        qWarning() << "FramePump: recorder has no audio track although audio is enabled";
        delete pipe->recorder;
        pipe->recorder = nullptr;
        reportBufferState(ReplayBufferState::Failed, pipe->audioFailure);
    }
    pipe->encClock.start();
    pipe->encStartQpc100ns = qpcNow100ns();   // A/V share this epoch
}

// 4-5. Free-threaded frame pool (no DispatcherQueue → pollable from this thread),
//      then the capture session.
//
// t24 experimental HDR gate: format selection ONLY happens here, and only
// when hdrExperimentalEnabled is true (ConfigKeys::InternalCaptureExperimentalHdr,
// hidden/default-on). All three of these must hold before FP16 is even
// attempted: the config flag, HdrCapabilities reporting the target display
// as HDR-active, and the GPU tone-mapper initializing successfully. Any
// failure anywhere in that chain falls straight back to the original
// hardcoded BGRA8 pool below — the SDR path this function has always taken
// is otherwise completely unchanged.
bool FramePumpWorker::createSession(Pipeline* pipe, void* hwndVoid, int srcW, int srcH,
                                    bool hdrExperimentalEnabled,
                                    const CaptureBorder::SessionPolicy& borderPolicy)
{
    HWND hwnd = reinterpret_cast<HWND>(hwndVoid);
    IDirect3D11CaptureFramePoolStatics2* poolStatics2 = nullptr;
    HSTRING poolClass = nullptr;
    WindowsCreateString(kWgcFramePoolClass, UINT32(wcslen(kWgcFramePoolClass)), &poolClass);
    HRESULT hr = RoGetActivationFactory(poolClass, IID_IDirect3D11CaptureFramePoolStatics2,
                                        reinterpret_cast<void**>(&poolStatics2));
    if (poolClass) WindowsDeleteString(poolClass);
    if (FAILED(hr) || !poolStatics2)
        return failStep("RoGetActivationFactory(FramePoolStatics2)", hr);

    DirectXPixelFormat poolFormat = DirectXPixelFormat_B8G8R8A8UIntNormalized;
    capture::HdrOutputInfo output;
    if (hdrExperimentalEnabled)
        output = capture::HdrCapabilities::forWindow(hwnd);
    const bool displayHdrActive = output.valid && output.hdrActive;
    // Only probe the GPU (a real COM call) when the cheaper checks already
    // passed — shouldAttemptFp16Capture() re-checks all three anyway.
    const bool gpuFp16Supported = displayHdrActive
        && capture::hdr::GpuToneMapper::checkFp16Support(pipe->d3dDevice);

    if (capture::hdr::shouldAttemptFp16Capture(hdrExperimentalEnabled, displayHdrActive,
                                               gpuFp16Supported)) {
        auto mapper = std::make_unique<capture::hdr::GpuToneMapper>();
        const float sdrWhiteNits = output.sdrWhiteLevelNits > 0.0f
            ? output.sdrWhiteLevelNits
            : capture::hdr::kDefaultSdrWhiteLevelNits;
        if (mapper->init(pipe->d3dDevice, pipe->d3dCtx, UINT(srcW), UINT(srcH),
                         sdrWhiteNits)) {
            pipe->hdrToneMapper = mapper.release();   // owned by Pipeline from here
            poolFormat = DirectXPixelFormat_R16G16B16A16Float;
            qInfo().noquote() << QStringLiteral(
                "FramePump: experimental HDR path armed (%1) — capturing FP16, "
                "tone-mapping to BGRA8 before encode (SDR white %2 nits)")
                .arg(output.describe())
                .arg(sdrWhiteNits, 0, 'f', 0);
        } else {
            mapper.reset();
            qWarning() << "FramePump: experimental HDR tone-mapper init failed — "
                          "falling back to BGRA8 SDR capture";
        }
    } else if (hdrExperimentalEnabled && displayHdrActive) {
        qInfo() << "FramePump: GPU lacks FP16 texture/sample support — "
                   "falling back to BGRA8 SDR capture";
    }

    // 4 buffers (was 2): the pump stalls for tens of ms every 5 s while the
    // segment writer finalizes + reopens (encoder MFT re-init). With only 2
    // buffers WGC drops frames during that stall — a periodic hitch in every
    // saved clip. 4 buffers ride it out; the drain loop in poll() catches up.
    const WgcSizeInt32 size{ srcW, srcH };
    hr = poolStatics2->CreateFreeThreaded(pipe->winrtDevice, poolFormat,
                                          4 /*buffers*/, size, &pipe->framePool);

    // FP16 pool creation itself failing (rare: CheckFormatSupport said yes
    // but WGC disagrees) must not abort the whole capture attempt — tear
    // down the tone-mapper and re-arm with the proven BGRA8 SDR path in the
    // SAME call, exactly like every other gate above.
    if ((FAILED(hr) || !pipe->framePool) && pipe->hdrToneMapper) {
        qWarning().nospace() << "FramePump: CreateFreeThreaded(FP16) failed hr=0x"
                             << Qt::hex << quint32(hr) << Qt::dec
                             << " — re-arming with BGRA8 SDR capture";
        delete pipe->hdrToneMapper;
        pipe->hdrToneMapper = nullptr;
        poolFormat = DirectXPixelFormat_B8G8R8A8UIntNormalized;
        hr = poolStatics2->CreateFreeThreaded(pipe->winrtDevice, poolFormat,
                                              4 /*buffers*/, size, &pipe->framePool);
    }
    poolStatics2->Release();
    if (FAILED(hr) || !pipe->framePool)
        return failStep("CreateFreeThreaded", hr);
    pipe->poolW = srcW;
    pipe->poolH = srcH;
    pipe->hdrToneMapActive = (pipe->hdrToneMapper != nullptr);

    hr = pipe->framePool->CreateCaptureSession(pipe->item, &pipe->session);
    if (FAILED(hr) || !pipe->session) return failStep("CreateCaptureSession", hr);

    // Try to suppress the yellow WGC capture border BEFORE StartCapture — that is the
    // documented order, and the only one that can take effect for the very first frame.
    //
    // put_IsBorderRequired(false) returns S_OK and reads back false even when the
    // process holds no Borderless access at all (measured in p4-1: this unpackaged
    // build gets UserPromptRequired), so the HRESULT is NOT evidence. Collect every
    // fact and let CaptureBorder::derive() decide; it is the only thing allowed to
    // conclude Hidden, and it never does so on a pre-22000 build.
    const CaptureBorder::Facts border = borderPolicy.collect([pipe] {
        CaptureBorder::Facts facts;
        facts.osBuild = CaptureUtil::windowsBuildNumber();

        IGraphicsCaptureSession3* session3 = nullptr;
        facts.sessionInterfaceAvailable =
            SUCCEEDED(pipe->session->QueryInterface(IID_IGraphicsCaptureSession3,
                                                    reinterpret_cast<void**>(&session3)))
            && session3 != nullptr;

        if (facts.sessionInterfaceAvailable) {
            facts.accessStatus = requestBorderlessAccess();
            facts.setterSucceeded = SUCCEEDED(session3->put_IsBorderRequired(0));
            unsigned char required = 1;
            facts.readBackSucceeded = SUCCEEDED(session3->get_IsBorderRequired(&required));
            facts.readBackBorderRequired = (required != 0);
            session3->Release();
        }
        return facts;
    });

    pipe->borderState  = CaptureBorder::derive(border);
    pipe->borderDetail = CaptureBorder::describe(border, pipe->borderState);
    qInfo().noquote() << "FramePump: capture border" << pipe->borderDetail;
    emit borderStateChanged(m_pumpGeneration, int(pipe->borderState), pipe->borderDetail);

    hr = pipe->session->StartCapture();
    if (FAILED(hr)) return failStep("StartCapture", hr);
    return true;
}

void FramePumpWorker::startPump(quint64 generation, qulonglong hwndVal, unsigned long pid, int encodeWidth,
                                int encodeHeight, int fps, int bitrateMbps, int segmentSeconds,
                                int lengthSeconds, const QString& gameName,
                                const QString& executablePath, int audioSource,
                                bool hdrExperimentalEnabled, CaptureBorder::SessionPolicy borderPolicy)
{
    if (generation <= m_pumpGeneration)
        return; // obsolete or duplicate command
    stopPump();
    m_pumpGeneration = generation;
    m_bufferState = ReplayBufferState::Starting;
    if (m_updatePreparing) {
        reportBufferState(ReplayBufferState::Failed,
                          QStringLiteral("Replay capture is paused for an update"));
        return;
    }
    if (!m_apartmentReady) {
        reportBufferState(ReplayBufferState::Failed,
                          QStringLiteral("Capture worker apartment is not initialized"));
        qWarning() << "FramePump: cannot start — worker apartment not initialised";
        return;
    }

    HWND hwnd = reinterpret_cast<HWND>(static_cast<quintptr>(hwndVal));
    if (!hwnd || !IsWindow(hwnd)) {
        failStep("capture window", E_HANDLE);
        return;
    }
    // Every early return below destroys the half-built pipeline; on success
    // ownership moves to m_pipe.
    auto ownedPipe = std::make_unique<Pipeline>();
    Pipeline* const pipe = ownedPipe.get();
    pipe->gameName = gameName;
    pipe->executablePath = executablePath;

    int srcW = 0, srcH = 0;
    if (!createDevices(pipe) || !createCaptureItem(pipe, hwnd, &srcW, &srcH))
        return;
    if (srcW < 2 || srcH < 2) {
        failStep("capture size", E_INVALIDARG);
        return;
    }
    attachRecorder(pipe, pid, srcW, srcH, encodeWidth, encodeHeight, fps, bitrateMbps,
                   segmentSeconds, lengthSeconds, audioSource);
    if (!createSession(pipe, hwnd, srcW, srcH, hdrExperimentalEnabled, borderPolicy))
        return;

    // 6. Poll timer on this (worker) thread. PreciseTimer: the default coarse
    // timer has 5% slack, which at 16 ms drifts the poll cadence enough to
    // overflow the frame pool between ticks.
    pipe->timer = new QTimer();   // no parent → affinity = this thread; owned by Pipeline
    pipe->timer->setTimerType(Qt::PreciseTimer);
    pipe->timer->setInterval(16); // ~60 Hz; the pool itself caps at the monitor rate
    connect(pipe->timer, &QTimer::timeout, this, &FramePumpWorker::poll);
    pipe->fpsClock.start();

    m_pipe = ownedPipe.release();
    m_pipe->timer->start();
    // Readiness is lifecycle bookkeeping, not part of WGC frame arrival or
    // the encoder hot path. Continue checking after Ready to detect re-open failure.
    pipe->readinessTimer = new QTimer();
    pipe->readinessTimer->setInterval(250);
    connect(pipe->readinessTimer, &QTimer::timeout, this, &FramePumpWorker::checkReplayReadiness);
    pipe->readinessTimer->start();
    if (pipe->recorder && pipe->recorder->isActive())
        reportBufferState(ReplayBufferState::Recording);
    checkReplayReadiness();
    qInfo() << "FramePump: started (hwnd" << Qt::hex << hwndVal << Qt::dec << ")";

    // createSession() already logged the experimental-HDR decision when the
    // flag was on (armed, or why it fell back). This just covers the plain
    // case: flag off, target happens to be HDR — still BGRA8 SDR, by design.
    if (!pipe->hdrToneMapActive) {
        const capture::HdrOutputInfo output = capture::HdrCapabilities::forWindow(hwnd);
        if (output.valid && output.hdrActive)
            qInfo().noquote() << QStringLiteral(
                "FramePump: capture target is on an HDR display (%1) — capturing BGRA8 SDR, "
                "highlights will clip (internal.capture.experimental_hdr is off or unavailable)"
                ).arg(output.describe());
        else if (output.valid)
            qInfo().noquote() << QStringLiteral("FramePump: capture target display is SDR (%1)")
                                     .arg(output.deviceName);
    }
}

void FramePumpWorker::reportBufferState(ReplayBufferState::State state, const QString& reason)
{
    if (m_bufferState == state)
        return;
    m_bufferState = state;
    m_audioFailure = state == ReplayBufferState::Failed && ReplayBufferState::isAudioFailure(reason)
        ? reason : QString();
    emit bufferStateChanged(m_pumpGeneration, state, reason);
}

void FramePumpWorker::checkReplayReadiness()
{
    if (!m_pipe)
        return;
    if (!m_pipe->recorder || !m_pipe->recorder->isActive()) {
        reportBufferState(ReplayBufferState::Failed, QStringLiteral("Replay recorder is not active"));
    } else if (m_bufferState == ReplayBufferState::Recording
               && m_pipe->recorder->hasClosedMedia()) {
        reportBufferState(ReplayBufferState::Ready);
    } else if (m_bufferState == ReplayBufferState::Ready
               && !m_pipe->recorder->hasClosedMedia()) {
        reportBufferState(ReplayBufferState::Recording);
    }
}

void FramePumpWorker::stopPumpForGeneration(quint64 generation)
{
    if (generation <= m_pumpGeneration)
        return;
    stopPump();
    m_pumpGeneration = generation;
}

void FramePumpWorker::stopPump()
{
    m_bufferState = ReplayBufferState::Stopped;
    if (!m_pipe)
        return;
    if (m_hdrScreenshotPending) {
        m_hdrScreenshotPending = false;
        emit hdrScreenshotFailed(m_pumpGeneration, m_hdrRequestId,
            QStringLiteral("HDR screenshot cancelled because the capture target changed"));
    }
    teardown();
    qInfo() << "FramePump: stopped";
}

void FramePumpWorker::poll()
{
    if (!m_pipe || !m_pipe->framePool)
        return;

    if (m_pipe->audio && m_pipe->recorder && m_pipe->recorder->hasAudio()) {
        for (;;) {
            long long t100 = 0;
            const unsigned ch = m_pipe->audio->channels();
            const unsigned maxFrames = ch ? unsigned(m_pipe->audioBuf.size()) / ch : 0;
            const unsigned got = m_pipe->audio->poll(m_pipe->audioBuf.data(), maxFrames, &t100);
            if (got == 0)
                break;
            // Audio timestamps are relative to the AUDIO capture's own start;
            // re-express them on the video clock (encClock epoch) so the
            // recorder can keep both streams on one timeline.
            const qint64 rel = m_pipe->audio->startQpc100ns() + t100
                             - m_pipe->encStartQpc100ns;
            m_pipe->recorder->writeAudio(m_pipe->audioBuf.constData(), got, rel);
        }
        if (m_pipe->audio->deviceInvalidated() || m_pipe->audio->failed()) {
            // A Windows HDR/display-mode switch can rebuild the HDMI/DP audio
            // endpoint; other read errors can leave the stream dead. Stop this
            // timer immediately so the error is not logged every 16 ms, drop
            // the now-incomplete A/V segment, and let the GUI-thread service
            // rebuild both WGC and WASAPI. A re-arm that cannot start audio
            // again fails the buffer explicitly (attachRecorder).
            if (m_pipe->timer)
                m_pipe->timer->stop();
            m_pipe->recorder->discardCurrentSegment();
            const bool invalidated = m_pipe->audio->deviceInvalidated();
            // A stream that dies again right after every re-arm would loop
            // forever; past the budget, fail explicitly.
            if (!invalidated && !m_audioRearmBudget.allow(qpcNow100ns() / 10000)) {
                qWarning() << "FramePump: audio capture keeps failing — replay buffer stopped";
                reportBufferState(ReplayBufferState::Failed, m_pipe->audioFailure);
                return;
            }
            qWarning() << "FramePump: audio capture" << (invalidated ? "endpoint was invalidated"
                                                                     : "stopped delivering")
                       << "— requesting clean re-arm";
            emit restartRequested(m_pumpGeneration, invalidated
                ? QStringLiteral("Windows audio endpoint changed")
                : QStringLiteral("Windows audio capture failed"));
            return;
        }
        // Nothing playing is not a failure: keep the AAC track continuous with
        // digital silence, leaving packet latency headroom so late real
        // packets are not overtaken.
        constexpr qint64 kAudioLatencyHeadroom100ns = 2000000;   // 200 ms
        m_pipe->recorder->padAudioSilence(qpcNow100ns() - m_pipe->encStartQpc100ns
                                          - kAudioLatencyHeadroom100ns);
    } else if (m_pipe->audio && m_pipe->recorder && m_pipe->recorder->audioLost()) {
        // The encoder dropped the audio track mid-session; never keep recording
        // video-only segments while system audio is enabled.
        if (m_pipe->timer)
            m_pipe->timer->stop();
        m_pipe->recorder->discardCurrentSegment();
        // Same budget as read failures: an encoder that keeps dropping audio
        // must end in the explicit audio failure, not endless re-arms.
        if (!m_audioRearmBudget.allow(qpcNow100ns() / 10000)) {
            qWarning() << "FramePump: audio encoder keeps failing — replay buffer stopped";
            reportBufferState(ReplayBufferState::Failed, m_pipe->audioFailure);
            return;
        }
        qWarning() << "FramePump: recorder lost its audio track — requesting clean re-arm";
        emit restartRequested(m_pumpGeneration, QStringLiteral("Replay audio encoder failed"));
        return;
    }

    // Drain EVERY frame buffered since the last tick, not just one. With one
    // frame per tick, any tick that arrives late (or a poll cycle spent in a
    // segment roll) leaves frames queued until the pool overflows and WGC
    // starts dropping — erratic gaps in the recording. The fps throttle in
    // writeFrame keeps the encode cost bounded regardless of drain rate.
    for (;;) {
        IDirect3D11CaptureFrame* frame = nullptr;
        const HRESULT hr = m_pipe->framePool->TryGetNextFrame(&frame);
        if (FAILED(hr)) {
            // Keeping the 16 ms timer running here logged this every tick.
            // Stop it and rebuild; a pool that keeps failing ends as an
            // explicit failure instead of an endless re-arm loop.
            if (m_pipe->timer)
                m_pipe->timer->stop();
            failStep("TryGetNextFrame", hr);
            if (m_captureRearmBudget.allow(qpcNow100ns() / 10000))
                emit restartRequested(m_pumpGeneration,
                                      QStringLiteral("Windows capture stopped delivering frames"));
            return;
        }
        if (!frame)          // pool drained — nothing more this tick
            break;

        WgcSizeInt32 content{ 0, 0 };
        if (SUCCEEDED(frame->get_ContentSize(&content)) && content.Width > 0 && content.Height > 0
            && (content.Width != m_pipe->poolW || content.Height != m_pipe->poolH)) {
            // A window being dragged changes size every frame; wait for the
            // new size to hold before paying for a re-arm.
            constexpr qint64 kContentSizeSettleMs = 500;
            if (content.Width != m_pipe->pendingContentSize.Width
                || content.Height != m_pipe->pendingContentSize.Height) {
                m_pipe->pendingContentSize = content;
                m_pipe->pendingContentClock.start();
            } else if (m_pipe->pendingContentClock.elapsed() >= kContentSizeSettleMs) {
                frame->Release();
                if (m_pipe->timer)
                    m_pipe->timer->stop();
                qInfo().noquote() << QStringLiteral(
                    "FramePump: capture content changed from %1x%2 to %3x%4 — re-arming")
                    .arg(m_pipe->poolW).arg(m_pipe->poolH)
                    .arg(content.Width).arg(content.Height);
                emit restartRequested(m_pumpGeneration, QStringLiteral("Game resolution changed"));
                return;
            }
        } else {
            m_pipe->pendingContentSize = { 0, 0 };
        }

        IDirect3DSurface* surface = nullptr;
        if (SUCCEEDED(frame->get_Surface(&surface)) && surface) {
            IDirect3DDxgiInterfaceAccess* access = nullptr;
            if (SUCCEEDED(surface->QueryInterface(IID_IDirect3DDxgiInterfaceAccess,
                                                  reinterpret_cast<void**>(&access))) && access) {
                ID3D11Texture2D* tex = nullptr;
                if (SUCCEEDED(access->GetInterface(__uuidof(ID3D11Texture2D),
                                                   reinterpret_cast<void**>(&tex))) && tex) {
                    D3D11_TEXTURE2D_DESC desc{};
                    tex->GetDesc(&desc);
                    m_pipe->lastW = desc.Width;
                    m_pipe->lastH = desc.Height;
                    m_pipe->lastFmt = int(desc.Format);
                    if (!m_pipe->firstFrameLogged) {
                        qInfo() << "FramePump: first frame" << desc.Width << "x" << desc.Height
                                << "DXGI_FORMAT=" << int(desc.Format);
                        m_pipe->firstFrameLogged = true;
                    }
                    // t24: tone-map FP16 -> BGRA8 before the recorder ever sees the
                    // frame. writeFrame() below is completely unaware this stage
                    // exists — it always receives BGRA8, exactly as before.
                    ID3D11Texture2D* encodeTex = tex;
                    bool skipFrame = false;
                    if (m_pipe->hdrToneMapActive) {
                        encodeTex = m_pipe->hdrToneMapper->apply(tex);
                        if (!encodeTex) {
                            skipFrame = true;
                            if (!m_pipe->hdrToneMapWarned) {
                                qWarning() << "FramePump: HDR tone-map apply() failed — "
                                              "dropping frames until it recovers (SDR path unaffected)";
                                m_pipe->hdrToneMapWarned = true;
                            }
                        }
                    }
                    // 0.5 Step 4 — feed the live texture to the H.264 segment writer
                    // (throttled to the target fps inside writeFrame).
                    if (m_hdrScreenshotPending) {
                        m_hdrScreenshotPending = false;
                        if (!skipFrame && m_pipe->hdrToneMapActive) {
                            const QImage image = readBgraTexture(
                                encodeTex, m_pipe->d3dDevice, m_pipe->d3dCtx);
                            if (!image.isNull()) {
                                qInfo() << "Screenshot: captured tone-mapped HDR frame"
                                        << image.width() << "x" << image.height();
                                emit hdrScreenshotReady(m_pumpGeneration, m_hdrRequestId, image, m_pipe->gameName,
                                                        m_pipe->executablePath);
                            } else {
                                emit hdrScreenshotFailed(m_pumpGeneration, m_hdrRequestId,
                                    QStringLiteral("could not read back the tone-mapped HDR frame"));
                            }
                        } else {
                            emit hdrScreenshotFailed(m_pumpGeneration, m_hdrRequestId,
                                QStringLiteral("HDR tone mapper is not producing frames"));
                        }
                    }
                    if (m_pipe->recorder && !skipFrame) {
                        const qint64 t100 = m_pipe->encClock.nsecsElapsed() / 100;
                        m_pipe->recorder->writeFrame(encodeTex, m_pipe->d3dDevice,
                                                     m_pipe->d3dCtx, t100);
                    }
                    tex->Release();
                }
                access->Release();
            }
            surface->Release();
        }
        frame->Release();
        ++m_pipe->frameCount;
    }
    if (m_pipe->fpsClock.elapsed() >= 1000) {
        qInfo().noquote() << QStringLiteral("FramePump: %1x%2 DXGI_FORMAT=%3 fps=%4")
                                 .arg(m_pipe->lastW).arg(m_pipe->lastH)
                                 .arg(m_pipe->lastFmt).arg(m_pipe->frameCount);
        m_pipe->frameCount = 0;
        m_pipe->fpsClock.restart();
    }
}

void FramePumpWorker::captureScreenshotOnWorker(quint64 generation, quint64 requestId)
{
    if (generation != m_pumpGeneration) {
        emit hdrScreenshotFailed(generation, requestId,
            QStringLiteral("HDR screenshot cancelled because the capture target changed"));
        return;
    }
    if (!m_pipe || !m_pipe->hdrToneMapActive) {
        emit hdrScreenshotFailed(m_pumpGeneration, requestId,
            QStringLiteral("HDR replay frame pump is not active for the foreground game"));
        return;
    }
    if (m_hdrScreenshotPending) {
        emit hdrScreenshotFailed(m_pumpGeneration, requestId, QStringLiteral("an HDR screenshot is already pending"));
        return;
    }
    m_hdrRequestId = requestId;
    m_hdrScreenshotPending = true;
}
