#include "capture/FramePumpService.h"

#include "capture/wgc_shims.h"
#include "capture/AudioCapture.h"
#include "capture/CapturePublisher.h"
#include "capture/CaptureUtil.h"
#include "capture/HdrCapabilities.h"
#include "capture/hdr/GpuToneMapper.h"
#include "capture/hdr/HdrToneMapMath.h"
#include "config/ConfigKeys.h"
#include "capture/SegmentRecorder.h"
#include "capture/ReplayExporter.h"
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
#include <QThread>
#include <QVector>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <memory>

#include <algorithm>
#include <cstring>

#include <roapi.h>       // RoInitialize / RoUninitialize / RoGetActivationFactory
#include <winstring.h>   // WindowsCreateString / WindowsDeleteString

// ===========================================================================
//  FramePumpWorker — all WGC/D3D work, runs on a dedicated MTA thread
// ===========================================================================

namespace { void sweepStaleReplayCache(); }   // defined below with the helpers

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
    QTimer*                       timer       = nullptr;
    SegmentRecorder*              recorder    = nullptr;   // 0.5 Step 4 H.264 segment writer
    AudioCapture*                 audio       = nullptr;    // 0.5 Step 7 WASAPI loopback
    QVector<float>                audioBuf;
    QString                       gameName;                // for the saved-clip Clips/ path
    QString                       executablePath;          // for sidebar game icon metadata

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
    teardown();
    if (m_apartmentReady) {
        RoUninitialize();
        m_apartmentReady = false;
    }
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
            && QFile::remove(f))
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

// Log an HRESULT failure and return false, for the linear build-pipeline flow.
bool failStep(FramePumpWorker* self, const char* step, HRESULT hr)
{
    const QString reason = QStringLiteral("%1 failed hr=0x%2")
                               .arg(QLatin1String(step))
                               .arg(quint32(hr), 8, 16, QLatin1Char('0'));
    qWarning() << "FramePump:" << reason;
    QMetaObject::invokeMethod(self, "failed", Qt::DirectConnection,
                              Q_ARG(QString, reason));
    return false;
}
} // namespace

// 1-2. D3D11 device (BGRA support is mandatory for WGC) bridged to a WinRT
//      IDirect3DDevice.
bool FramePumpWorker::createDevices(Pipeline* pipe)
{
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                   D3D11_SDK_VERSION, &pipe->d3dDevice, nullptr,
                                   &pipe->d3dCtx);
    if (FAILED(hr)) return failStep(this, "D3D11CreateDevice", hr);

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
        return failStep(this, "resolve CreateDirect3D11DeviceFromDXGIDevice", E_FAIL);

    IDXGIDevice* dxgiDevice = nullptr;
    hr = pipe->d3dDevice->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice));
    if (FAILED(hr)) return failStep(this, "QI IDXGIDevice", hr);

    IInspectable* inspDevice = nullptr;
    hr = pfnBridge(dxgiDevice, &inspDevice);
    dxgiDevice->Release();
    if (FAILED(hr) || !inspDevice)
        return failStep(this, "CreateDirect3D11DeviceFromDXGIDevice", hr);

    hr = inspDevice->QueryInterface(IID_IDirect3DDevice, reinterpret_cast<void**>(&pipe->winrtDevice));
    inspDevice->Release();
    if (FAILED(hr)) return failStep(this, "QI IDirect3DDevice", hr);
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
        return failStep(this, "RoGetActivationFactory(interop)", hr);

    hr = interop->CreateForWindow(static_cast<HWND>(hwndPtr), IID_IGraphicsCaptureItem,
                                  reinterpret_cast<void**>(&pipe->item));
    interop->Release();
    if (FAILED(hr) || !pipe->item) return failStep(this, "CreateForWindow", hr);

    WgcSizeInt32 size{ 0, 0 };
    hr = pipe->item->get_Size(&size);
    if (FAILED(hr)) return failStep(this, "item->get_Size", hr);
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
// Cannot fail the arm: a recorder that will not begin degrades to capture-only,
// exactly as before, so this returns void rather than bool.
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
                                    bool hdrExperimentalEnabled)
{
    HWND hwnd = reinterpret_cast<HWND>(hwndVoid);
    IDirect3D11CaptureFramePoolStatics2* poolStatics2 = nullptr;
    HSTRING poolClass = nullptr;
    WindowsCreateString(kWgcFramePoolClass, UINT32(wcslen(kWgcFramePoolClass)), &poolClass);
    HRESULT hr = RoGetActivationFactory(poolClass, IID_IDirect3D11CaptureFramePoolStatics2,
                                        reinterpret_cast<void**>(&poolStatics2));
    if (poolClass) WindowsDeleteString(poolClass);
    if (FAILED(hr) || !poolStatics2)
        return failStep(this, "RoGetActivationFactory(FramePoolStatics2)", hr);

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
        return failStep(this, "CreateFreeThreaded", hr);
    pipe->hdrToneMapActive = (pipe->hdrToneMapper != nullptr);

    hr = pipe->framePool->CreateCaptureSession(pipe->item, &pipe->session);
    if (FAILED(hr) || !pipe->session) return failStep(this, "CreateCaptureSession", hr);

    hr = pipe->session->StartCapture();
    if (FAILED(hr)) return failStep(this, "StartCapture", hr);

    // Best-effort: hide the yellow WGC capture border (Win11 IGraphicsCaptureSession3).
    // If the interface/capability is unavailable, QI fails and the border just stays.
    IGraphicsCaptureSession3* session3 = nullptr;
    if (SUCCEEDED(pipe->session->QueryInterface(IID_IGraphicsCaptureSession3,
                                                reinterpret_cast<void**>(&session3))) && session3) {
        const HRESULT hrb = session3->put_IsBorderRequired(0);   // 0 = false
        if (SUCCEEDED(hrb))
            qInfo() << "FramePump: capture border hidden";
        else
            qInfo().nospace() << "FramePump: could not hide capture border (hr=0x"
                              << Qt::hex << quint32(hrb) << ")";
        session3->Release();
    } else {
        qInfo() << "FramePump: capture-border interface unavailable — border stays";
    }
    return true;
}

void FramePumpWorker::startPump(qulonglong hwndVal, unsigned long pid, int encodeWidth,
                                int encodeHeight, int fps, int bitrateMbps, int segmentSeconds,
                                int lengthSeconds, const QString& gameName,
                                const QString& executablePath, bool audioEnabled,
                                bool hdrExperimentalEnabled)
{
    if (m_pipe)   // already running
        return;
    if (!m_apartmentReady) {
        qWarning() << "FramePump: cannot start — worker apartment not initialised";
        return;
    }

    HWND hwnd = reinterpret_cast<HWND>(static_cast<quintptr>(hwndVal));
    auto pipe = new Pipeline();
    pipe->gameName = gameName;
    pipe->executablePath = executablePath;

    int srcW = 0, srcH = 0;
    if (!createDevices(pipe) || !createCaptureItem(pipe, hwnd, &srcW, &srcH)) {
        delete pipe;
        return;
    }
    attachRecorder(pipe, pid, srcW, srcH, encodeWidth, encodeHeight, fps, bitrateMbps,
                   segmentSeconds, lengthSeconds, audioEnabled);
    if (!createSession(pipe, hwnd, srcW, srcH, hdrExperimentalEnabled)) {
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

void FramePumpWorker::stopPump()
{
    if (!m_pipe)
        return;
    if (m_hdrScreenshotPending) {
        m_hdrScreenshotPending = false;
        emit hdrScreenshotFailed(
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
            emit restartRequested(QStringLiteral("Windows audio endpoint changed"));
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
            qWarning().nospace() << "FramePump: TryGetNextFrame hr=0x" << Qt::hex << quint32(hr);
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
                                emit hdrScreenshotReady(image, m_pipe->gameName,
                                                        m_pipe->executablePath);
                            } else {
                                emit hdrScreenshotFailed(
                                    QStringLiteral("could not read back the tone-mapped HDR frame"));
                            }
                        } else {
                            emit hdrScreenshotFailed(
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

void FramePumpWorker::captureScreenshotOnWorker()
{
    if (!m_pipe || !m_pipe->hdrToneMapActive) {
        emit hdrScreenshotFailed(
            QStringLiteral("HDR replay frame pump is not active for the foreground game"));
        return;
    }
    if (m_hdrScreenshotPending) {
        emit hdrScreenshotFailed(QStringLiteral("an HDR screenshot is already pending"));
        return;
    }
    m_hdrScreenshotPending = true;
}

// Preflight: refuse when the buffer is not running or an export is in flight.
bool FramePumpWorker::saveGuard(const QString& saveId)
{
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
// Pin it FIRST so the rolling recorder cannot delete any snapshot file
// while the export thread cut in runExport() is still reading it.
// Empty result = ring had nothing; the failure signal is already emitted
// and the ring unpinned.
QStringList FramePumpWorker::freezeRing(const QString& saveId)
{
    m_pipe->recorder->pinRing();
    QElapsedTimer snapshotTimer;
    snapshotTimer.start();
    const QStringList segs = m_pipe->recorder->snapshotForSave();
    qInfo().noquote() << QStringLiteral("ReplaySave[%1]: snapshot segments=%2 elapsedMs=%3")
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
        m_pipe->recorder->unpinRing();
        const QString game = m_pipe->gameName.isEmpty() ? QStringLiteral("Replay")
                                                        : m_pipe->gameName;
        emit clipFailed(game, QStringLiteral("Replay buffer is empty"));
        return QStringList();
    }
    return segs;
}

// The moment is locked in — give instant feedback (before the slower
// remux) so the "saved" sound/notification doesn't wait for encoding.
// Grab a preview from the freshest completed segment (not the
// not-yet-remuxed final file) so the toast can show a thumbnail right away.
// Returns the written thumbnail path, or empty if the grab failed.
QString FramePumpWorker::instantThumbnail(const QString& lastSegment, const QString& thumbPath,
                                          const QString& saveId)
{
    QString instantThumb;
    QImage instantFrame;
    QElapsedTimer instantThumbTimer;
    instantThumbTimer.start();
    if (ReplayExporter::grabThumbnail(lastSegment, instantFrame) && !instantFrame.isNull()) {
        QDir().mkpath(Paths::thumbnailsDir());
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
// pad/frame processing) for the whole export. The ring stays pinned until
// the export finishes so none of the snapshot files can be deleted.
void FramePumpWorker::runExport(const QStringList& segs,
                                const CapturePublisher::Reservation& reservation,
                                const QString& thumbPath, const QString& instantThumb,
                                const QString& game, const QString& exePath,
                                const QString& saveId)
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

    QThread* exportThread = QThread::create(
        [segs, reservation, outPath, partialPath, thumbPath, instantThumb, saveId, result] {
            const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            QElapsedTimer t;
            t.start();
            result->finalThumb = instantThumb;
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
            if (SUCCEEDED(hrCo))
                CoUninitialize();
        });

    // Completion runs back on THIS worker thread (context object = this), so
    // recorder access needs no locking; the connection dissolves safely if
    // the worker is destroyed first.
    connect(exportThread, &QThread::finished, this,
            [this, result, reservation, outPath, game, exePath, saveId] {
                m_exportBusy = false;
                emit exportBusyChanged(false);
                if (m_pipe && m_pipe->recorder)
                    m_pipe->recorder->unpinRing();
                if (result->ok) {
                    qInfo().noquote() << QStringLiteral("ReplaySave[%1]: success (async)").arg(saveId);
                    emit clipSaved(outPath, game, result->finalThumb, exePath);
                } else {
                    // Only the reservation is given back. Deleting outPath here
                    // used to destroy whatever clip already held that name.
                    CapturePublisher::discard(reservation);
                    qWarning() << "FramePump: save-replay — remux failed for" << outPath;
                    emit clipFailed(game, QStringLiteral("Could not export a complete replay clip"));
                }
                if (m_updatePreparing) {
                    stopPump();
                    emit updateReady();
                }
            });
    connect(exportThread, &QThread::finished, exportThread, &QObject::deleteLater);
    exportThread->start();
}

void FramePumpWorker::prepareForUpdate()
{
    m_updatePreparing = true;
    if (m_exportBusy)
        return;
    stopPump();
    emit updateReady();
}

void FramePumpWorker::cancelUpdatePreparation()
{
    m_updatePreparing = false;
}

void FramePumpWorker::saveReplayOnWorker(const QString& clipsBaseRoot)
{
    static quint64 saveCounter = 0;
    const QString saveId = QStringLiteral("%1-%2")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("HHmmsszzz")))
        .arg(++saveCounter);
    QElapsedTimer saveTimer;
    saveTimer.start();

    if (!saveGuard(saveId))
        return;

    const QStringList segs = freezeRing(saveId);
    if (segs.isEmpty())
        return;

    const QString game = m_pipe->gameName.isEmpty() ? QStringLiteral("Unknown Game")
                                                    : m_pipe->gameName;

    const QString clipsDir = clipsBaseRoot + QLatin1Char('/')
                             + GameIdentity::folderName(game) + QStringLiteral("/Clips");
    if (!QDir().mkpath(clipsDir)) {
        m_pipe->recorder->unpinRing();
        emit clipFailed(game, QStringLiteral("Could not create the selected clips folder"));
        return;
    }
    // The name is claimed before anything is written, so a second save inside
    // the same second is handed <stamp>_2.mp4 instead of the first clip's name.
    // Nothing below may delete or overwrite a file it did not reserve.
    const CapturePublisher::Reservation reservation = CapturePublisher::reserve(
        clipsDir, QStringLiteral("yyyy-MM-dd_HH-mm-ss"), QStringLiteral(".mp4"));
    if (!reservation.isValid()) {
        m_pipe->recorder->unpinRing();
        emit clipFailed(game, QStringLiteral("Could not reserve a name for the clip"));
        return;
    }
    // Derived from the reserved clip name, so the preview inherits the same
    // _2 and stays the "<clip base>_clip.png" pair ThumbnailService reattaches.
    const QString thumbPath = CapturePublisher::companionPath(
        reservation.finalPath, Paths::thumbnailsDir(), QStringLiteral("_clip.png"));
    qInfo().noquote() << QStringLiteral("ReplaySave[%1]: output path=%2 game=%3")
                             .arg(saveId).arg(reservation.finalPath).arg(game);

    const QString instantThumb = instantThumbnail(segs.last(), thumbPath, saveId);
    emit clipSaving(game, instantThumb, m_pipe->executablePath);

    runExport(segs, reservation, thumbPath, instantThumb, game, m_pipe->executablePath, saveId);
    qInfo().noquote() << QStringLiteral("ReplaySave[%1]: export started in background totalSoFarMs=%2")
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
    connect(m_worker, &FramePumpWorker::failed, this, &FramePumpService::failed);
    connect(m_worker, &FramePumpWorker::restartRequested, this,
            [this](const QString& reason) {
                qInfo().noquote() << QStringLiteral("FramePump: %1 — rebuilding capture pipeline")
                                         .arg(reason);
                restartBuffer();
            });
    connect(m_worker, &FramePumpWorker::clipSaving, this, &FramePumpService::clipSaving);
    connect(m_worker, &FramePumpWorker::clipSaved, this, &FramePumpService::clipSaved);
    connect(m_worker, &FramePumpWorker::clipFailed, this, &FramePumpService::clipFailed);
    connect(m_worker, &FramePumpWorker::hdrScreenshotReady, this,
            [this](const QImage& image, const QString& game, const QString& exePath) {
                emit hdrScreenshotReady(image, game, exePath);
                if (m_stopAfterHdrScreenshot) {
                    m_stopAfterHdrScreenshot = false;
                    stopBuffer();
                }
            });
    connect(m_worker, &FramePumpWorker::hdrScreenshotFailed, this,
            [this](const QString& reason) {
                emit hdrScreenshotFailed(reason);
                if (m_stopAfterHdrScreenshot) {
                    m_stopAfterHdrScreenshot = false;
                    stopBuffer();
                }
            });
    connect(m_worker, &FramePumpWorker::exportBusyChanged, this, [this](bool busy) {
        if (m_exportBusy == busy)
            return;
        m_exportBusy = busy;
        emit exportBusyChanged(busy);
    });
    connect(m_worker, &FramePumpWorker::updateReady, this, [this] {
        m_running = false;
        m_targetHwnd = 0;
        emit recordingStateChanged(false, QString());
        emit updateReady();
    });
    // If a startPump fails, clear the armed flag so auto-arm can retry next tick.
    connect(m_worker, &FramePumpWorker::failed, this, [this] {
        m_running = false;
        m_targetHwnd = 0;
        emit recordingStateChanged(false, QString());
    });

    // Always-on auto-arm (replay.auto, default true): a light poll of the foreground
    // window arms the buffer whenever a game is focused — no manual keypress needed.
    m_autoEnabled = m_config ? m_config->value(ConfigKeys::ReplayAuto, true).toBool() : true;
    m_autoTimer = new QTimer(this);
    m_autoTimer->setInterval(1500);
    connect(m_autoTimer, &QTimer::timeout, this, &FramePumpService::autoTick);
    m_autoTimer->start();

    m_thread.start();
}

void FramePumpService::saveReplay()
{
    if (m_preparingForUpdate) {
        emit clipFailed(QStringLiteral("Replay"), QStringLiteral("Replay capture is paused for an update"));
        return;
    }
    // A cold buffer holds no past frames, so passing the request through would
    // only bounce back as "buffer is not running" — the Share-hold then looks
    // ignored no matter how long it was held. Arm here instead: an explicit
    // save request outranks replay.auto, so this arms even with always-on off.
    // The tick is restarted because disabling always-on stops it, and it is
    // what disarms the buffer once the game leaves the foreground.
    if (!m_running) {
        if (m_autoTimer && !m_autoTimer->isActive())
            m_autoTimer->start();
        startBuffer();
        qInfo() << "FramePump: save-replay on a cold buffer —"
                << (m_running ? "armed now, next hold has footage to save"
                              : "foreground window is not a game, nothing to arm");
        emit clipFailed(QStringLiteral("Replay"),
                        m_running
                            ? QStringLiteral("Replay buffer was off — recording now, hold again in a few seconds")
                            : QStringLiteral("Replay buffer is not running"));
        return;
    }

    const QString clipsBaseRoot = m_locations ? m_locations->clipsBaseRoot()
                                              : Paths::capturesRoot();
    QMetaObject::invokeMethod(m_worker, "saveReplayOnWorker", Qt::QueuedConnection,
                              Q_ARG(QString, clipsBaseRoot));
}

void FramePumpService::captureHdrScreenshot(qulonglong hwnd)
{
    if (m_preparingForUpdate) {
        emit hdrScreenshotFailed(
            QStringLiteral("screenshot capture is paused for an update"));
        return;
    }

    if (!m_running || m_targetHwnd != hwnd) {
        if (m_running)
            stopBuffer();
        startBuffer();
    }
    if (!m_running || m_targetHwnd != hwnd) {
        emit hdrScreenshotFailed(
            QStringLiteral("could not arm HDR capture on the foreground game"));
        return;
    }

    m_stopAfterHdrScreenshot = !m_autoEnabled;
    QMetaObject::invokeMethod(m_worker, "captureScreenshotOnWorker",
                              Qt::QueuedConnection);
}

void FramePumpService::prepareForUpdate()
{
    if (m_preparingForUpdate)
        return;
    m_preparingForUpdate = true;
    emit preparingForUpdateChanged(true);
    if (m_autoTimer)
        m_autoTimer->stop();
    if (m_exportBusy)
        emit updateWaitingForExport();
    QMetaObject::invokeMethod(m_worker, "prepareForUpdate", Qt::QueuedConnection);
}

void FramePumpService::cancelUpdatePreparation()
{
    if (!m_preparingForUpdate)
        return;
    m_preparingForUpdate = false;
    emit preparingForUpdateChanged(false);
    QMetaObject::invokeMethod(m_worker, "cancelUpdatePreparation", Qt::QueuedConnection);
    if (m_autoEnabled && m_autoTimer && !m_autoTimer->isActive())
        m_autoTimer->start();
}

void FramePumpService::restartBuffer()
{
    const bool configuredAuto = m_config
        ? m_config->value(ConfigKeys::ReplayAuto, true).toBool() : true;
    const bool autoChanged = configuredAuto != m_autoEnabled;
    m_autoEnabled = configuredAuto;

    if (!m_autoEnabled) {
        if (m_autoTimer)
            m_autoTimer->stop();
        stopBuffer();
        qInfo() << "FramePump: always-on recording disabled from Settings";
        return;
    }

    if (m_autoTimer && !m_autoTimer->isActive())
        m_autoTimer->start();
    if (!m_running) {
        if (autoChanged)
            qInfo() << "FramePump: always-on recording enabled from Settings";
        startBuffer();
        return;
    }
    qInfo() << "FramePump: capture conditions changed — re-arming the buffer";
    stopBuffer();
    startBuffer();   // no-op unless a game is still foreground; autoTick covers the rest
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
    const QString mode = m_config
        ? m_config->value(ConfigKeys::CaptureMode, QStringLiteral("only_in_games")).toString()
        : QStringLiteral("only_in_games");
    const ForegroundGame g = GameDetector::current();
    const bool isGame = GameDetector::shouldCapture(g, mode);
    if (g.valid && !g.isExcludedProcess)
        emit foregroundGameDetected(g.gameName, g.executablePath);

    if (isGame) {
        if (!m_autoEnabled)
            return;
        m_noGameTicks = 0;
        const qulonglong foregroundHwnd =
            qulonglong(reinterpret_cast<quintptr>(g.hwnd));
        if (!m_running) {
            startBuffer();
        } else if (m_targetHwnd != foregroundHwnd) {
            qInfo() << "FramePump: foreground capture target changed — re-arming";
            stopBuffer();
            startBuffer();
        }
    } else {
        if (!g.valid || g.isExcludedProcess)
            emit foregroundGameDetected(QString(), QString());
        if (m_running && ++m_noGameTicks >= 2)   // ~3 s grace so a brief focus change doesn't cut it
            stopBuffer();
    }
}

void FramePumpService::startBuffer()
{
    if (m_running)
        return;
    const QString mode = m_config
        ? m_config->value(ConfigKeys::CaptureMode, QStringLiteral("only_in_games")).toString()
        : QStringLiteral("only_in_games");
    const ForegroundGame g = GameDetector::current();
    if (!GameDetector::shouldCapture(g, mode))
        return;

    m_running = true;
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
    const QString gameName = g.gameName.isEmpty() ? QStringLiteral("Unknown Game") : g.gameName;
    const QString executablePath = g.executablePath;
    qInfo() << "FramePump: armed on" << gameName << "(audio:" << (audioOn ? "on" : "off")
            << ")";
    emit recordingStateChanged(true, gameName);
    QMetaObject::invokeMethod(m_worker, "startPump", Qt::QueuedConnection,
                              Q_ARG(qulonglong, qulonglong(reinterpret_cast<quintptr>(g.hwnd))),
                              Q_ARG(unsigned long, g.pid),
                              Q_ARG(int, res.width()), Q_ARG(int, res.height()),
                              Q_ARG(int, fps), Q_ARG(int, mbps), Q_ARG(int, seg),
                              Q_ARG(int, len), Q_ARG(QString, gameName),
                              Q_ARG(QString, executablePath),
                              Q_ARG(bool, audioOn),
                              Q_ARG(bool, hdrExperimentalEnabled));
}

void FramePumpService::stopBuffer()
{
    if (!m_running)
        return;
    m_running = false;
    m_targetHwnd = 0;
    m_noGameTicks = 0;
    QMetaObject::invokeMethod(m_worker, "stopPump", Qt::QueuedConnection);
    emit recordingStateChanged(false, QString());
}
