#include "capture/FramePumpService.h"

#include "capture/wgc_shims.h"
#include "capture/AudioCapture.h"
#include "capture/CaptureBorderState.h"
#include "capture/CapturePublisher.h"
#include "capture/CaptureUtil.h"
#include "capture/HdrCapabilities.h"
#include "capture/hdr/GpuToneMapper.h"
#include "capture/hdr/HdrToneMapMath.h"
#include "config/ConfigKeys.h"
#include "capture/SegmentRecorder.h"
#include "capture/ReplayExporter.h"
#include "capture/ReplayExportTask.h"
#include "config/CaptureLocations.h"
#include "config/ConfigManager.h"
#include "config/Paths.h"
#include "core/GameIdentity.h"
#include "games/GameDetector.h"
#include "storage/ThumbnailService.h"

#include <QDebug>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QMetaObject>
#include <QSize>
#include <QScopeGuard>
#include <QThread>
#include <QVector>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <memory>
#include <utility>

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

// Real OS build, not the marketing version: GetVersionEx() lies to unmanifested
// processes, RtlGetVersion() does not. 0 means "could not read it".
unsigned long windowsBuildNumber()
{
    static const unsigned long build = []() -> unsigned long {
        using PfnRtlGetVersion = LONG (WINAPI*)(PRTL_OSVERSIONINFOW);
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        const auto pfn = ntdll ? reinterpret_cast<PfnRtlGetVersion>(
                                     GetProcAddress(ntdll, "RtlGetVersion"))
                               : nullptr;
        RTL_OSVERSIONINFOW info{};
        info.dwOSVersionInfoSize = sizeof(info);
        if (!pfn || pfn(&info) != 0)
            return 0;
        return info.dwBuildNumber;
    }();
    return build;
}

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

// How long an unfinished clip file may sit in a Clips folder before it counts
// as abandoned. Same 10 minutes the segment cache uses.
constexpr qint64 kStaleClipPartMaxAgeSecs = 600;

// Holds every WinRT/D3D pointer plus the poll timer and fps counters for one
// active capture. Destroying it releases everything in reverse-construction
// order (see docs/replay-buffer.md).
struct FramePumpWorker::Pipeline
{
    ID3D11Device*                 d3dDevice   = nullptr;
    ID3D11DeviceContext*          d3dCtx      = nullptr;
    IDirect3DDevice*              winrtDevice = nullptr;
    IGraphicsCaptureItem*         item        = nullptr;
    IDirect3D11CaptureFramePool*  framePool   = nullptr;
    IGraphicsCaptureSession*      session     = nullptr;
    QTimer*                       readinessTimer = nullptr;
    QTimer*                       timer       = nullptr;
    SegmentRecorder*              recorder    = nullptr;   // 0.5 Step 4 H.264 segment writer
    AudioCapture*                 audio       = nullptr;    // 0.5 Step 7 WASAPI loopback
    QVector<float>                audioBuf;
    QString                       gameName;                // for the saved-clip Clips/ path
    QString                       executablePath;          // for sidebar game icon metadata

    // What actually happened to the Windows capture border for this session (p4-2).
    // Never assumed — derived from the OS build, the access request and the read-back.
    CaptureBorder::State          borderState  = CaptureBorder::Unknown;
    QString                       borderDetail;

    // t24 experimental HDR tone-map stage (isolated, hidden flag, see
    // ConfigKeys::InternalCaptureExperimentalHdr). hdrToneMapActive is only
    // ever true when the frame pool itself was created FP16 AND the mapper
    // initialized — the SDR path (BGRA8 pool, no mapper) is completely
    // untouched otherwise.
    capture::hdr::GpuToneMapper*  hdrToneMapper     = nullptr;
    bool                          hdrToneMapActive  = false;
    bool                          hdrToneMapWarned  = false;  // log a failed apply() once, not per-frame

    QElapsedTimer fpsClock;
    QElapsedTimer encClock;   // monotonic PTS source for the recorder
    long long encStartQpc100ns = 0;   // QPC epoch of encClock's t=0 (A/V alignment)
    int  frameCount = 0;
    UINT lastW = 0, lastH = 0;
    int  lastFmt = 0;
    bool firstFrameLogged = false;

    ~Pipeline()
    {
        if (readinessTimer) { readinessTimer->stop(); delete readinessTimer; }
        if (timer) { timer->stop(); delete timer; timer = nullptr; }
        // Finalize + release the recorder BEFORE the D3D device/context it borrows
        // (its staging texture was created from d3dDevice).
        if (recorder) { recorder->end(); delete recorder; recorder = nullptr; }
        if (audio) { audio->stop(); delete audio; audio = nullptr; }
        // Also before d3dDevice/d3dCtx — GpuToneMapper's shaders/textures were
        // created from them.
        if (hdrToneMapper) { delete hdrToneMapper; hdrToneMapper = nullptr; }
        if (session)     { session->Release();     session = nullptr; }
        if (framePool)   { framePool->Release();    framePool = nullptr; }
        if (item)        { item->Release();         item = nullptr; }
        if (winrtDevice) { winrtDevice->Release();  winrtDevice = nullptr; }
        if (d3dCtx)      { d3dCtx->Release();        d3dCtx = nullptr; }
        if (d3dDevice)   { d3dDevice->Release();     d3dDevice = nullptr; }
    }
};

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
QSize parseResolution(const QString& value, const QSize& fallback)
{
    const QStringList parts = value.toLower().split(QLatin1Char('x'));
    if (parts.size() != 2)
        return fallback;
    bool okW = false;
    bool okH = false;
    const int w = parts.at(0).trimmed().toInt(&okW);
    const int h = parts.at(1).trimmed().toInt(&okH);
    if (!okW || !okH || w < 2 || h < 2)
        return fallback;
    return QSize(w, h);
}

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
        const HMODULE d3dll = LoadLibraryW(L"d3d11.dll");
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
                                     int segmentSeconds, int lengthSeconds, bool audioEnabled)
{
    // Audio (Step 7) is opt-in: untested WASAPI code crashed the worker thread on
    // every game-arm in dev.74. Gate behind config "audio.enabled" (default false)
    // so video capture stays stable; audio only starts when explicitly enabled.
    if (audioEnabled) {
        pipe->audio = new AudioCapture();
        if (pipe->audio->start(pid)) {
            const unsigned frames = pipe->audio->sampleRate() / 10;
            pipe->audioBuf.resize(int(frames * pipe->audio->channels()));
            qInfo() << "FramePump: audio attached";
        } else {
            delete pipe->audio;
            pipe->audio = nullptr;
            qInfo() << "FramePump: audio start failed — continuing video-only";
        }
    }
    const QSize encodeSize = fitInsideEven(QSize(srcW, srcH), QSize(encodeWidth, encodeHeight));
    QString cacheProfile = QStringLiteral("%1-%2x%3-%4fps")
                               .arg(pipe->audio ? QStringLiteral("audio")
                                                : QStringLiteral("video"))
                               .arg(encodeSize.width())
                               .arg(encodeSize.height())
                               .arg(fps);
    if (pipe->audio) {
        cacheProfile += QStringLiteral("-%1hz-%2ch")
                            .arg(pipe->audio->sampleRate())
                            .arg(pipe->audio->channels());
    }
    const QString cacheDir = Paths::replayCacheDir() + QLatin1Char('/')
                             + GameIdentity::folderName(pipe->gameName) + QLatin1Char('/')
                             + cacheProfile;
    pipe->recorder = new SegmentRecorder();
    if (!pipe->recorder->begin(srcW, srcH, encodeSize.width(), encodeSize.height(),
                               fps, bitrateMbps, segmentSeconds, lengthSeconds, cacheDir,
                               pipe->d3dDevice, pipe->d3dCtx,
                               pipe->audio ? pipe->audio->sampleRate() : 0,
                               pipe->audio ? pipe->audio->channels() : 0)) {
        qWarning() << "FramePump: segment recorder failed to start — capture-only";
        delete pipe->recorder;
        pipe->recorder = nullptr;
        reportBufferState(ReplayBufferState::Failed,
                          QStringLiteral("Replay recorder could not start"));
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
        auto* mapper = new capture::hdr::GpuToneMapper();
        const float sdrWhiteNits = output.sdrWhiteLevelNits > 0.0f
            ? output.sdrWhiteLevelNits
            : capture::hdr::kDefaultSdrWhiteLevelNits;
        if (mapper->init(pipe->d3dDevice, pipe->d3dCtx, UINT(srcW), UINT(srcH),
                         sdrWhiteNits)) {
            pipe->hdrToneMapper = mapper;
            poolFormat = DirectXPixelFormat_R16G16B16A16Float;
            qInfo().noquote() << QStringLiteral(
                "FramePump: experimental HDR path armed (%1) — capturing FP16, "
                "tone-mapping to BGRA8 before encode (SDR white %2 nits)")
                .arg(output.describe())
                .arg(sdrWhiteNits, 0, 'f', 0);
        } else {
            delete mapper;
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
        facts.osBuild = windowsBuildNumber();

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
                                const QString& executablePath, bool audioEnabled,
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
    auto pipe = new Pipeline();
    pipe->gameName = gameName;
    pipe->executablePath = executablePath;

    int srcW = 0, srcH = 0;
    if (!createDevices(pipe) || !createCaptureItem(pipe, hwnd, &srcW, &srcH)) {
        delete pipe;
        return;
    }
    if (srcW < 2 || srcH < 2) {
        failStep("capture size", E_INVALIDARG);
        delete pipe;
        return;
    }
    attachRecorder(pipe, pid, srcW, srcH, encodeWidth, encodeHeight, fps, bitrateMbps,
                   segmentSeconds, lengthSeconds, audioEnabled);
    if (!createSession(pipe, hwnd, srcW, srcH, hdrExperimentalEnabled, borderPolicy)) {
        delete pipe;
        return;
    }

    // 6. Poll timer on this (worker) thread. PreciseTimer: the default coarse
    // timer has 5% slack, which at 16 ms drifts the poll cadence enough to
    // overflow the frame pool between ticks.
    pipe->timer = new QTimer();   // no parent → affinity = this thread; owned by Pipeline
    pipe->timer->setTimerType(Qt::PreciseTimer);
    pipe->timer->setInterval(16); // ~60 Hz; the pool itself caps at the monitor rate
    connect(pipe->timer, &QTimer::timeout, this, &FramePumpWorker::poll);
    pipe->fpsClock.start();

    m_pipe = pipe;
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
        if (m_pipe->audio->deviceInvalidated()) {
            // A Windows HDR/display-mode switch can rebuild the HDMI/DP audio
            // endpoint. Stop this timer immediately so the invalid HRESULT is
            // not logged every 16 ms, drop the now-incomplete A/V segment, and
            // let the GUI-thread service rebuild both WGC and WASAPI.
            if (m_pipe->timer)
                m_pipe->timer->stop();
            m_pipe->recorder->discardCurrentSegment();
            qWarning() << "FramePump: audio endpoint was invalidated — requesting clean re-arm";
            emit restartRequested(m_pumpGeneration, QStringLiteral("Windows audio endpoint changed"));
            return;
        }
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
            failStep("TryGetNextFrame", hr);
            return;
        }
        if (!frame)          // pool drained — nothing more this tick
            break;

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

// Preflight: refuse when the buffer is not running or an export is in flight.
bool FramePumpWorker::saveGuard(const QString& saveId)
{
    checkReplayReadiness();
    if (!ReplayBufferState::canSave(m_bufferState)) {
        emit clipFailed(QStringLiteral("Replay"), ReplayBufferState::saveRejection(m_bufferState));
        return false;
    }
    qInfo().noquote() << QStringLiteral("ReplaySave[%1]: request begin pipe=%2 recorder=%3 active=%4")
                             .arg(saveId)
                             .arg(m_pipe ? QStringLiteral("yes") : QStringLiteral("no"))
                             .arg(m_pipe && m_pipe->recorder ? QStringLiteral("yes") : QStringLiteral("no"))
                             .arg(m_pipe && m_pipe->recorder && m_pipe->recorder->isActive()
                                  ? QStringLiteral("yes") : QStringLiteral("no"));

    if (!m_pipe || !m_pipe->recorder || !m_pipe->recorder->isActive()) {
        qInfo() << "FramePump: save-replay ignored — buffer not running (always-on recording is controlled in Settings → Replay)";
        emit clipFailed(QStringLiteral("Replay"), QStringLiteral("Replay buffer is not running"));
        return false;
    }
    if (m_exportBusy) {
        qInfo() << "FramePump: save-replay ignored — an export is already in progress";
        emit clipFailed(m_pipe->gameName.isEmpty() ? QStringLiteral("Replay") : m_pipe->gameName,
                        QStringLiteral("A replay save is already in progress"));
        return false;
    }
    if (m_updatePreparing) {
        emit clipFailed(m_pipe->gameName.isEmpty() ? QStringLiteral("Replay") : m_pipe->gameName,
                        QStringLiteral("Replay capture is paused for an update"));
        return false;
    }
    return true;
}

// Freeze the ring (finalizes the in-flight segment, keeps recording).
// The snapshot itself leases its paths before recording resumes. Empty result
// means the ring had nothing; the failure signal is already emitted.
SegmentLease FramePumpWorker::freezeRing(const QString& saveId)
{
    QElapsedTimer snapshotTimer;
    snapshotTimer.start();
    SegmentLease lease = m_pipe->recorder->snapshotForSave();
    checkReplayReadiness();
    const QStringList& segs = lease.paths();
    qInfo().noquote() << QStringLiteral("ReplaySave[%1]: frozen segments=%2 elapsedMs=%3")
                             .arg(saveId).arg(segs.size()).arg(snapshotTimer.elapsed());
    for (int i = 0; i < segs.size(); ++i) {
        const QFileInfo fi(segs.at(i));
        qInfo().noquote() << QStringLiteral("ReplaySave[%1]: snapshot segment %2 path=%3 exists=%4 bytes=%5")
                                 .arg(saveId).arg(i).arg(segs.at(i))
                                 .arg(fi.exists() ? QStringLiteral("yes") : QStringLiteral("no"))
                                 .arg(fi.exists() ? fi.size() : -1);
    }
    if (segs.isEmpty()) {
        qWarning() << "FramePump: save-replay — ring empty, nothing to save";
        const QString game = m_pipe->gameName.isEmpty() ? QStringLiteral("Replay")
                                                        : m_pipe->gameName;
        emit clipFailed(game, QStringLiteral("Replay buffer is empty"));
        return {};
    }
    if (!m_pipe->recorder->hasClosedMedia()) {
        emit clipFailed(m_pipe->gameName, ReplayBufferState::saveRejection(m_bufferState));
        return {};
    }
    return lease;
}

// Export-thread fallback preview from the newest leased segment. Static and
// value-only: decoding must never access the capture worker or its pipeline.
QString FramePumpWorker::instantThumbnail(const QString& lastSegment, const QString& thumbPath,
                                          const QString& saveId)
{
    QString instantThumb;
    QImage instantFrame;
    QElapsedTimer instantThumbTimer;
    instantThumbTimer.start();
    if (ReplayExporter::grabThumbnail(lastSegment, instantFrame) && !instantFrame.isNull()) {
        const QImage scaled = instantFrame.width() > 640
            ? instantFrame.scaledToWidth(640, Qt::SmoothTransformation) : instantFrame;
        if (ThumbnailService::saveThumbnail(scaled, thumbPath, "PNG"))
            instantThumb = thumbPath;
    }
    qInfo().noquote() << QStringLiteral("ReplaySave[%1]: instant thumbnail ok=%2 path=%3 elapsedMs=%4")
                             .arg(saveId)
                             .arg(instantThumb.isEmpty() ? QStringLiteral("no") : QStringLiteral("yes"))
                             .arg(instantThumb)
                             .arg(instantThumbTimer.elapsed());
    return instantThumb;
}

// Remux + final thumbnail on their OWN thread: previously they ran right
// here on the capture thread, so every save paused recording (and any
// pad/frame processing) for the whole export. The task owns the snapshot lease
// and releases it even if the worker completion callback is never delivered.
void FramePumpWorker::runExport(SegmentLease lease,
                                const CapturePublisher::Reservation& reservation,
                                const QString& thumbPath,
                                const QString& game, const QString& exePath,
                                const QString& saveId, quint64 requestId)
{
    const QString outPath = reservation.finalPath;
    const QString partialPath = reservation.pendingPath;
    struct ExportResult {
        bool ok = false;
        QString finalThumb;
    };
    auto result = std::make_shared<ExportResult>();
    m_exportBusy = true;
    emit exportBusyChanged(true);
    const quint64 generation = ++m_exportGeneration;

    m_exportTask = std::make_unique<ReplayExportTask>(std::move(lease),
        [reservation, outPath, partialPath, thumbPath, saveId, result]
        (const QStringList& segs) {
            const auto discardOnFailure = qScopeGuard([&] {
                if (!result->ok)
                    CapturePublisher::discard(reservation);
            });
            const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            const auto uninitialize = qScopeGuard([hrCo] {
                if (SUCCEEDED(hrCo))
                    CoUninitialize();
            });
            QElapsedTimer t;
            t.start();
            // Both preview and final decode/scale/encode stay under the export
            // task's lease. The capture worker only schedules this work.
            QDir().mkpath(QFileInfo(thumbPath).absolutePath());
            if (!segs.isEmpty())
                result->finalThumb = instantThumbnail(segs.last(), thumbPath, saveId);
            // The reserved .part already exists and the MP4 sink writer
            // truncates it, so the name stays claimed for the whole export.
            if (ReplayExporter::concat(segs, partialPath, saveId)) { // remux, no re-encode
                qInfo().noquote() << QStringLiteral(
                    "ReplaySave[%1]: remux ok output=%2 segments=%3 elapsedMs=%4 bytes=%5")
                    .arg(saveId).arg(partialPath).arg(segs.size()).arg(t.elapsed())
                    .arg(QFileInfo(partialPath).size());
                // Final thumbnail: the clip's actual first frame, replacing
                // the instant preview (same file path).
                QImage frame;
                if (ReplayExporter::grabThumbnail(partialPath, frame) && !frame.isNull()) {
                    const QImage scaled = frame.width() > 640
                        ? frame.scaledToWidth(640, Qt::SmoothTransformation) : frame;
                    if (ThumbnailService::saveThumbnail(scaled, thumbPath, "PNG"))
                        result->finalThumb = thumbPath;
                }
                // Publish only a completely finalized file. A crash before
                // this point leaves an ignored *.part, never a gallery MP4.
                // The rename refuses to clobber, so an unrelated clip that
                // somehow holds the name survives instead of being replaced.
                QString publishError;
                result->ok = CapturePublisher::publish(reservation, &publishError);
                if (!result->ok)
                    qWarning() << "ReplaySave: could not publish finalized clip" << outPath
                               << publishError;
            } else {
                qWarning().noquote() << QStringLiteral(
                    "ReplaySave[%1]: failed - remux failed output=%2 elapsedMs=%3")
                    .arg(saveId).arg(outPath).arg(t.elapsed());
            }
        });

    // Completion runs back on THIS worker thread (context object = this), so
    // recorder access needs no locking; the connection dissolves safely if
    // the worker is destroyed first.
    connect(m_exportTask.get(), &QThread::finished, this,
            [this, result, outPath, game, exePath, saveId, generation, requestId] {
                if (generation != m_exportGeneration)
                    return;
                m_exportTask.reset(); // finished; join and destroy on its owner thread
                m_exportBusy = false;
                emit exportBusyChanged(false);
                if (m_pipe && m_pipe->recorder)
                    m_pipe->recorder->trimRing();
                if (result->ok) {
                    qInfo().noquote() << QStringLiteral("ReplaySave[%1]: published").arg(saveId);
                    emit clipSaved(outPath, game, result->finalThumb, exePath);
                } else {
                    qWarning() << "FramePump: save-replay — remux failed for" << outPath;
                    emit clipFailed(game, QStringLiteral("Could not export a complete replay clip"));
                }
                emit saveRequestFinished(requestId);
                if (m_updatePreparing) {
                    stopPump();
                    emit updateReady(m_updateGeneration);
                }
            }, Qt::QueuedConnection);
    m_exportTask->start();
}

void FramePumpWorker::prepareForUpdate(quint64 generation)
{
    m_updateGeneration = generation;
    m_updatePreparing = true;
    if (m_exportBusy)
        return;
    stopPump();
    emit updateReady(m_updateGeneration);
}

void FramePumpWorker::cancelUpdatePreparation()
{
    m_updatePreparing = false;
}

void FramePumpWorker::saveReplayOnWorker(const QString& clipsBaseRoot, quint64 generation,
                                        quint64 requestId, quint64 chainId)
{
    QElapsedTimer saveTimer;
    saveTimer.start();
    // Measure the next turn of this worker's event loop, not just thread start.
    // This is save-request instrumentation; poll()/frame arrival are untouched.
    const auto reportReturn = qScopeGuard([this, saveTimer, chainId, requestId] {
        QMetaObject::invokeMethod(this, [saveTimer, chainId, requestId] {
            qInfo().noquote() << QStringLiteral("ReplaySave[%1]: worker resumed elapsedMs=%2")
                .arg(chainId ? chainId : requestId).arg(saveTimer.elapsed());
        }, Qt::QueuedConnection);
    });
    bool handedToExport = false;
    const auto finishRejectedRequest = qScopeGuard([&] {
        if (!handedToExport)
            emit saveRequestFinished(requestId);
    });
    if (generation != m_pumpGeneration) {
        emit clipFailed(QStringLiteral("Replay"),
                        QStringLiteral("Replay buffer changed before the save could start"));
        return;
    }
    // The chain id from the press, so every line below joins the same chain the
    // service already logged. Falls back to the lease token for callers that
    // have no request behind them.
    const QString saveId = QString::number(chainId ? chainId : requestId);
    if (!saveGuard(saveId))
        return;

    // Recording may contain a short first segment, even before Ready. Freeze
    // finalizes that footage and rejects an empty/unusable snapshot itself.
    SegmentLease lease = freezeRing(saveId);
    if (lease.isEmpty())
        return;

    const QString game = m_pipe->gameName.isEmpty() ? QStringLiteral("Unknown Game")
                                                    : m_pipe->gameName;

    const QString clipsDir = clipsBaseRoot + QLatin1Char('/')
                             + GameIdentity::folderName(game) + QStringLiteral("/Clips");
    if (!QDir().mkpath(clipsDir)) {
        emit clipFailed(game, QStringLiteral("Could not create the selected clips folder"));
        return;
    }
    // The name is claimed before anything is written, so a second save inside
    // the same second is handed <stamp>_2.mp4 instead of the first clip's name.
    // Nothing below may delete or overwrite a file it did not reserve.
    const CapturePublisher::Reservation reservation = CapturePublisher::reserve(
        clipsDir, QStringLiteral("yyyy-MM-dd_HH-mm-ss"), QStringLiteral(".mp4"));
    if (!reservation.isValid()) {
        emit clipFailed(game, QStringLiteral("Could not reserve a name for the clip"));
        return;
    }
    // Derived from the reserved clip name, so the preview inherits the same
    // _2 and stays the "<clip base>_clip.png" pair ThumbnailService reattaches.
    const QString thumbPath = CapturePublisher::companionPath(
        reservation.finalPath, Paths::thumbnailsDir(), QStringLiteral("_clip.png"));
    qInfo().noquote() << QStringLiteral("ReplaySave[%1]: output path=%2 game=%3")
                             .arg(saveId).arg(reservation.finalPath).arg(game);

    // Receipt already happened at service entry. No image work is needed to
    // announce that export started; clipSaved delivers the completed thumbnail.
    emit clipSaving(game, QString(), m_pipe->executablePath);

    runExport(std::move(lease), reservation, thumbPath,
              game, m_pipe->executablePath, saveId, requestId);
    handedToExport = true;
    qInfo().noquote() << QStringLiteral("ReplaySave[%1]: exporting totalSoFarMs=%2")
                             .arg(saveId).arg(saveTimer.elapsed());
}

// ===========================================================================
//  FramePumpService — GUI-thread front-end
// ===========================================================================

FramePumpService::FramePumpService(ConfigManager* config, CaptureLocations* locations,
                                   QObject* parent)
    : QObject(parent)
    , m_config(config)
    , m_locations(locations)
{
    // A crash or a power cut mid-export leaves a reserved *.part behind. It is
    // invisible to the gallery, but nothing else would ever remove it. Only
    // files older than the threshold go: an export takes seconds, never ten
    // minutes, so a running one can never be swept away.
    if (m_locations)
        CapturePublisher::sweepStale(m_locations->clipsBaseRoot(), kStaleClipPartMaxAgeSecs);

    m_worker = new FramePumpWorker();   // no parent → we move it to m_thread
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::started, m_worker, &FramePumpWorker::onThreadStarted);
    // DirectConnection: runs on the worker thread as run() unwinds, after wait() begins.
    connect(&m_thread, &QThread::finished, m_worker, &FramePumpWorker::onThreadFinished,
            Qt::DirectConnection);
    connect(&m_buffer, &ReplayBufferState::stateChanged, this, &FramePumpService::bufferStateChanged);
    connect(&m_buffer, &ReplayBufferState::stateChanged, this, &FramePumpService::bufferStatusChanged);
    connect(&m_buffer, &ReplayBufferState::recordingStateChanged,
            this, &FramePumpService::recordingStateChanged);
    connect(&m_buffer, &ReplayBufferState::failed, this, &FramePumpService::failed);
    connect(&m_buffer, &ReplayBufferState::stateChanged, this,
            [this](ReplayBufferState::State state, const QString&) {
                if (state == ReplayBufferState::Starting || state == ReplayBufferState::Stopped
                    || state == ReplayBufferState::Failed) {
                    m_borderState = CaptureBorder::Unknown;
                    m_borderDetail.clear();
                    emit captureBorderStateChanged();
                }
                if (state == ReplayBufferState::Stopped || state == ReplayBufferState::Failed)
                    m_targetHwnd = 0;
                if (state == ReplayBufferState::Stopped)
                    m_targetGame = {};
            });
    connect(m_worker, &FramePumpWorker::bufferStateChanged, this,
            [this](quint64 generation, ReplayBufferState::State state, const QString& reason) {
                m_buffer.confirm(generation, state, reason);
            });
    connect(m_worker, &FramePumpWorker::borderStateChanged, this,
            [this](quint64 generation, int state, const QString& detail) {
                // Only the live generation may speak for the border, same fencing rule
                // the buffer state uses.
                if (generation != m_buffer.generation())
                    return;
                const auto next = static_cast<CaptureBorder::State>(state);
                if (next == m_borderState && detail == m_borderDetail)
                    return;
                m_borderState = next;
                m_borderDetail = detail;
                emit captureBorderStateChanged();
            });
    connect(m_worker, &FramePumpWorker::restartRequested, this,
            [this](quint64 generation, const QString& reason) {
                if (generation != m_buffer.generation() || m_preparingForUpdate)
                    return;
                qInfo().noquote() << QStringLiteral("FramePump: %1 — rebuilding capture pipeline")
                                         .arg(reason);
                restartBuffer();
            });
    connect(m_worker, &FramePumpWorker::clipSaving, this, &FramePumpService::clipSaving);
    // There is one admitted replay export at a time. Immediate rejections carry
    // their own request id and must never overwrite the active export identity.
    connect(m_worker, &FramePumpWorker::clipSaved, this,
            [this](const QString& path, const QString& game, const QString& thumb, const QString& exe) {
                emit clipSaved(path, game, thumb, exe, m_activeReplayOperation);
            });
    connect(m_worker, &FramePumpWorker::clipFailed, this,
            [this](const QString& game, const QString& reason) {
                emit clipFailed(game, reason, m_activeReplayOperation);
            });
    connect(m_worker, &FramePumpWorker::saveRequestFinished, this, [this](quint64 requestId) {
        if (m_owners.finishSave(requestId)) {
            m_activeReplayOperation = 0;
            ownersChanged("save finished", requestId);
        }
    });
    connect(m_worker, &FramePumpWorker::hdrScreenshotReady, this,
            [this](quint64, quint64 requestId, const QImage& image, const QString& game, const QString& exePath) {
                if (!m_owners.finishHdr(requestId))
                    return;
                const auto operationId = std::exchange(m_activeHdrOperation, 0);
                ownersChanged("HDR finished", requestId);
                emit hdrScreenshotReady(image, game, exePath, operationId);
            });
    connect(m_worker, &FramePumpWorker::hdrScreenshotFailed, this,
            [this](quint64, quint64 requestId, const QString& reason) {
                if (!m_owners.finishHdr(requestId))
                    return;
                const auto operationId = std::exchange(m_activeHdrOperation, 0);
                ownersChanged("HDR failed", requestId);
                emit hdrScreenshotFailed(reason, operationId);
            });
    connect(m_worker, &FramePumpWorker::exportBusyChanged, this, [this](bool busy) {
        if (m_exportBusy == busy)
            return;
        m_exportBusy = busy;
        emit exportBusyChanged(busy);
    });
    connect(m_worker, &FramePumpWorker::updateReady, this, [this](quint64 generation) {
        if (!m_preparingForUpdate || generation != m_buffer.generation())
            return;
        m_buffer.requestStop();
        emit updateReady();
    });

    // Always-on auto-arm (replay.auto, default true): a light poll of the foreground
    // window arms the buffer whenever a game is focused — no manual keypress needed.
    m_autoEnabled = m_config ? m_config->value(ConfigKeys::ReplayAuto, true).toBool() : true;
    m_ownerClock.start();
    m_autoTimer = new QTimer(this);
    m_autoTimer->setInterval(1500);
    connect(m_autoTimer, &QTimer::timeout, this, &FramePumpService::autoTick);
    m_autoTimer->start();

    m_thread.start();
}

void FramePumpService::ownersChanged(const char* reason, quint64 requestId)
{
    qInfo().noquote() << QStringLiteral("ReplayOwner[%1]: %2 auto=%3 manual=%4 hdr=%5 savePending=%6")
        .arg(requestId).arg(QLatin1String(reason))
        .arg(m_owners.owns(ReplayBufferOwners::Auto))
        .arg(m_owners.owns(ReplayBufferOwners::ManualSave))
        .arg(m_owners.owns(ReplayBufferOwners::HdrScreenshot)).arg(m_owners.savePending());
    if (!m_preparingForUpdate && !m_owners.needsSession())
        stopBuffer();
}

void FramePumpService::saveReplay(const CaptureRequest& request)
{
    emit requestAccepted(request, CaptureRequest::Kind::Replay);
    const QString chain = request.tag();
    // Every exit below logs. Before this, the four early rejections emitted
    // clipFailed() and nothing else, so a save that died here left no trace of
    // the press at all - the exact shape of the reported "View/Back does
    // nothing" symptom.
    qInfo().noquote() << QStringLiteral("ReplaySave[%1]: accepted").arg(chain);
    const auto reject = [this, &chain, &request](const QString& game, const QString& reason) {
        qWarning().noquote() << QStringLiteral("ReplaySave[%1]: failed - %2").arg(chain, reason);
        emit clipFailed(game, reason, request.id);
    };
    if (m_preparingForUpdate) {
        reject(QStringLiteral("Replay"), QStringLiteral("Replay capture is paused for an update"));
        return;
    }
    const int idleSeconds = m_config
        ? m_config->value(ConfigKeys::ReplayManualIdleSeconds, 90).toInt() : 90;
    const quint64 requestId = m_owners.acquireManual(m_ownerClock.elapsed(), idleSeconds);
    if (!requestId) {
        reject(QStringLiteral("Replay"), QStringLiteral("A replay save is already in progress"));
        return;
    }
    ownersChanged("manual save requested", requestId);
    if (!m_buffer.startRequested())
        startBuffer(m_targetGame.valid);
    if (!m_buffer.canSave()) {
        // A cold arm keeps its owner until the next save or bounded idle expiry.
        // No pending save is silently deferred until readiness.
        reject(m_buffer.gameName().isEmpty() ? QStringLiteral("Replay") : m_buffer.gameName(),
               ReplayBufferState::saveRejection(m_buffer.state()));
        return;
    }
    m_owners.beginSave(requestId);
    m_activeReplayOperation = request.id;
    ownersChanged("save dispatched", requestId);
    qInfo().noquote() << QStringLiteral("ReplaySave[%1]: armed generation=%2 owner=%3 game=%4")
                             .arg(chain).arg(m_buffer.generation()).arg(requestId)
                             .arg(m_buffer.gameName().isEmpty() ? QStringLiteral("?")
                                                                : m_buffer.gameName());
    const QString clipsBaseRoot = m_locations ? m_locations->clipsBaseRoot() : Paths::capturesRoot();
    if (!QMetaObject::invokeMethod(m_worker, "saveReplayOnWorker", Qt::QueuedConnection,
                                  Q_ARG(QString, clipsBaseRoot), Q_ARG(quint64, m_buffer.generation()),
                                  Q_ARG(quint64, requestId), Q_ARG(quint64, request.id))) {
        m_owners.finishSave(requestId);
        m_activeReplayOperation = 0;
        ownersChanged("save dispatch failed", requestId);
        reject(QStringLiteral("Replay"), QStringLiteral("Could not dispatch replay save"));
    }
}

void FramePumpService::captureHdrScreenshot(qulonglong hwnd, quint64 operationId)
{
    if (m_preparingForUpdate) {
        emit hdrScreenshotFailed(QStringLiteral("screenshot capture is paused for an update"), operationId);
        return;
    }
    if (m_owners.owns(ReplayBufferOwners::ManualSave) && m_targetGame.valid
        && qulonglong(reinterpret_cast<quintptr>(m_targetGame.hwnd)) != hwnd) {
        emit hdrScreenshotFailed(QStringLiteral("HDR screenshot target differs from the active manual replay session"), operationId);
        return;
    }
    const quint64 requestId = m_owners.acquireHdr();
    if (!requestId) {
        emit hdrScreenshotFailed(QStringLiteral("an HDR screenshot is already pending"), operationId);
        return;
    }
    m_activeHdrOperation = operationId;
    ownersChanged("HDR requested", requestId);
    if (!m_buffer.startRequested() || m_targetHwnd != hwnd) {
        if (m_targetHwnd != hwnd && !m_owners.owns(ReplayBufferOwners::ManualSave))
            m_targetGame = GameDetector::current();
        startBuffer(m_targetGame.valid);
    }
    if (!m_buffer.startRequested() || m_targetHwnd != hwnd) {
        m_owners.finishHdr(requestId);
        m_activeHdrOperation = 0;
        ownersChanged("HDR arm failed", requestId);
        emit hdrScreenshotFailed(QStringLiteral("could not arm HDR capture on the foreground game"), operationId);
        return;
    }
    if (!QMetaObject::invokeMethod(m_worker, "captureScreenshotOnWorker", Qt::QueuedConnection,
                                  Q_ARG(quint64, m_buffer.generation()), Q_ARG(quint64, requestId))) {
        m_owners.finishHdr(requestId);
        m_activeHdrOperation = 0;
        ownersChanged("HDR dispatch failed", requestId);
        emit hdrScreenshotFailed(QStringLiteral("could not dispatch HDR capture"), operationId);
    }
}

void FramePumpService::prepareForUpdate()
{
    if (m_preparingForUpdate)
        return;
    m_preparingForUpdate = true;
    m_owners.setAuto(false);
    m_owners.releaseColdManual(); // explicit shutdown cancels an idle arm, never an export
    ownersChanged("update preparation");
    emit preparingForUpdateChanged(true);
    if (m_autoTimer)
        m_autoTimer->stop();
    if (m_exportBusy)
        emit updateWaitingForExport();
    QMetaObject::invokeMethod(m_worker, "prepareForUpdate", Qt::QueuedConnection,
                              Q_ARG(quint64, m_buffer.generation()));
}

void FramePumpService::cancelUpdatePreparation()
{
    if (!m_preparingForUpdate)
        return;
    m_preparingForUpdate = false;
    emit preparingForUpdateChanged(false);
    QMetaObject::invokeMethod(m_worker, "cancelUpdatePreparation", Qt::QueuedConnection);
    if (m_autoTimer && !m_autoTimer->isActive())
        m_autoTimer->start();
}

void FramePumpService::restartBuffer()
{
    m_autoEnabled = m_config ? m_config->value(ConfigKeys::ReplayAuto, true).toBool() : true;
    if (m_preparingForUpdate)
        return;
    if (!m_autoEnabled)
        m_owners.setAuto(false);
    else if (GameDetector::shouldCapture(GameDetector::current(), m_config
                 ? m_config->value(ConfigKeys::CaptureMode, QStringLiteral("only_in_games")).toString()
                 : QStringLiteral("only_in_games")))
        m_owners.setAuto(true);
    ownersChanged("settings or capture restart", m_owners.manualToken());
    if (m_owners.needsSession())
        startBuffer(m_targetGame.valid); // worker replaces the pipe; owners and export leases survive
}

FramePumpService::~FramePumpService()
{
    // Tear the pipeline down on the worker thread while its event loop still runs,
    // then stop the thread (its finished handler does RoUninitialize).
    QMetaObject::invokeMethod(m_worker, "stopPump", Qt::BlockingQueuedConnection);
    m_thread.quit();
    m_thread.wait();
    delete m_worker;
}

void FramePumpService::autoTick()
{
    if (m_preparingForUpdate)
        return;
    const quint64 manualToken = m_owners.manualToken();
    if (m_owners.expireIdle(m_ownerClock.elapsed()))
        ownersChanged("manual idle expired", manualToken);
    const QString mode = m_config
        ? m_config->value(ConfigKeys::CaptureMode, QStringLiteral("only_in_games")).toString()
        : QStringLiteral("only_in_games");
    const ForegroundGame g = GameDetector::current();
    const bool isGame = GameDetector::shouldCapture(g, mode);
    if (g.valid && !g.isExcludedProcess)
        emit foregroundGameDetected(g.gameName, g.executablePath);
    if (m_autoEnabled && isGame) {
        m_noGameTicks = 0;
        if (!m_owners.owns(ReplayBufferOwners::Auto)) {
            m_owners.setAuto(true);
            ownersChanged("auto acquired");
        }
        // An explicit session owns its target until its operation/idle deadline ends.
        if (m_owners.hasExplicitOwner())
            return;
        const auto hwnd = qulonglong(reinterpret_cast<quintptr>(g.hwnd));
        if (!m_buffer.startRequested() || m_targetHwnd != hwnd) {
            m_targetGame = g;
            startBuffer(true);
        }
    } else {
        if (!g.valid || g.isExcludedProcess)
            emit foregroundGameDetected(QString(), QString());
        if (m_owners.owns(ReplayBufferOwners::Auto) && ++m_noGameTicks >= 2) {
            m_owners.setAuto(false);
            ownersChanged("auto released after foreground grace");
        }
    }
}

void FramePumpService::startBuffer(bool rearm)
{
    if ((!rearm && m_buffer.startRequested()) || m_preparingForUpdate || !m_owners.needsSession())
        return;
    const QString mode = m_config
        ? m_config->value(ConfigKeys::CaptureMode, QStringLiteral("only_in_games")).toString()
        : QStringLiteral("only_in_games");
    const ForegroundGame g = rearm && m_targetGame.valid ? m_targetGame : GameDetector::current();
    const QString gameName = g.gameName.isEmpty() ? QStringLiteral("Unknown Game") : g.gameName;
    const quint64 generation = m_buffer.requestStart(gameName);
    if (!GameDetector::shouldCapture(g, mode)) {
        m_buffer.confirm(generation, ReplayBufferState::Failed,
                         QStringLiteral("No eligible foreground game for replay capture"));
        return;
    }
    m_targetGame = g;
    m_targetHwnd = qulonglong(reinterpret_cast<quintptr>(g.hwnd));
    m_noGameTicks = 0;
    const int fps  = m_config ? m_config->value(ConfigKeys::ReplayFps, 30).toInt() : 30;
    const int mbps = m_config ? m_config->value(ConfigKeys::ReplayBitrateMbps, 14).toInt() : 14;
    const int seg  = m_config ? m_config->value(ConfigKeys::ReplaySegmentSeconds, 5).toInt() : 5;
    const int len  = m_config ? m_config->value(ConfigKeys::ReplayLengthSeconds, 300).toInt() : 300;
    const QSize res = parseResolution(
        m_config ? m_config->value(ConfigKeys::ReplayResolution,
                                   QStringLiteral("1920x1080")).toString()
                 : QStringLiteral("1920x1080"),
        QSize(1920, 1080));
    const bool audioRequested = m_config
        ? m_config->value(ConfigKeys::AudioEnabled, false).toBool() : false;
    const bool audioOn = audioRequested;
    // t24: hidden, default-on — see ConfigKeys::InternalCaptureExperimentalHdr.
    const bool hdrExperimentalEnabled = m_config
        ? m_config->value(ConfigKeys::InternalCaptureExperimentalHdr,
                          ConfigKeys::InternalCaptureExperimentalHdrDefault).toBool()
        : ConfigKeys::InternalCaptureExperimentalHdrDefault;
    const QString executablePath = g.executablePath;
    // Copy at dispatch, never read mutable GUI-thread config on the worker.
    const CaptureBorder::SessionPolicy borderPolicy(m_config
        ? m_config->value(ConfigKeys::CaptureHideBorder, true).toBool() : true);
    qInfo() << "FramePump: start requested on" << gameName << "(audio:" << (audioOn ? "on" : "off")
            << ")";
    const bool queued = QMetaObject::invokeMethod(m_worker, "startPump", Qt::QueuedConnection,
                              Q_ARG(quint64, generation),
                              Q_ARG(qulonglong, qulonglong(reinterpret_cast<quintptr>(g.hwnd))),
                              Q_ARG(unsigned long, g.pid),
                              Q_ARG(int, res.width()), Q_ARG(int, res.height()),
                              Q_ARG(int, fps), Q_ARG(int, mbps), Q_ARG(int, seg),
                              Q_ARG(int, len), Q_ARG(QString, gameName),
                              Q_ARG(QString, executablePath),
                              Q_ARG(bool, audioOn),
                              Q_ARG(bool, hdrExperimentalEnabled),
                              Q_ARG(CaptureBorder::SessionPolicy, borderPolicy));
    if (!queued)
        m_buffer.confirm(generation, ReplayBufferState::Failed,
                         QStringLiteral("Could not dispatch replay startup"));
}

void FramePumpService::stopBuffer()
{
    if (m_owners.needsSession() || m_buffer.state() == ReplayBufferState::Stopped)
        return;
    m_targetGame = {};
    const quint64 generation = m_buffer.requestStop();
    m_targetHwnd = 0;
    m_noGameTicks = 0;
    QMetaObject::invokeMethod(m_worker, "stopPumpForGeneration", Qt::QueuedConnection,
                              Q_ARG(quint64, generation));
}
