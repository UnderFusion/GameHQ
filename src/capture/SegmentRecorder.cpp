#include "capture/SegmentRecorder.h"
#include "capture/CaptureUtil.h"

#include <windows.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <codecapi.h>

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>

#include <cstring>
#include <string>

namespace {

// --- MFStartup ref-count (worker-thread-local; single thread, no atomics needed).
int g_mfRefs = 0;
bool mfAddRef()
{
    if (g_mfRefs == 0) {
        const HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
        if (FAILED(hr)) {
            qWarning().nospace() << "SegmentRecorder: MFStartup failed hr=0x"
                                 << Qt::hex << quint32(hr);
            return false;
        }
    }
    ++g_mfRefs;
    return true;
}
void mfRelease()
{
    if (g_mfRefs > 0 && --g_mfRefs == 0)
        MFShutdown();
}

inline bool ok(const char* what, HRESULT hr)
{
    return CaptureUtil::ok("SegmentRecorder", what, hr);
}

QString uniqueSegmentPath(const QString& cacheDir)
{
    // Include milliseconds because snapshotForSave() closes one segment and
    // opens the next immediately; second-resolution names can collide and
    // overwrite the segment the exporter is about to read.
    return CaptureUtil::uniqueTimestampedPath(
        cacheDir, QStringLiteral("yyyy-MM-dd_HH-mm-ss-zzz"), QStringLiteral("_clip.mp4"));
}

} // namespace

SegmentRecorder::~SegmentRecorder()
{
    end();
}

bool SegmentRecorder::begin(int sourceWidth, int sourceHeight, int encodeWidth, int encodeHeight,
                            int fps, int bitrateMbps, int segmentSeconds, int lengthSeconds,
                            const QString& cacheDir, ID3D11Device* dev, ID3D11DeviceContext* ctx,
                            unsigned audioRateHz, unsigned audioChannels)
{
    if (m_active)
        return true;
    if (sourceWidth < 2 || sourceHeight < 2 || encodeWidth < 2 || encodeHeight < 2) {
        qWarning() << "SegmentRecorder: refusing to start, bad size"
                   << sourceWidth << sourceHeight << "->" << encodeWidth << encodeHeight;
        return false;
    }

    m_srcW     = unsigned(sourceWidth)  & ~1u;
    m_srcH     = unsigned(sourceHeight) & ~1u;
    m_w        = unsigned(encodeWidth)  & ~1u;      // H.264 wants even dimensions
    m_h        = unsigned(encodeHeight) & ~1u;

    // GPU downscale (performance): when the encode cap is smaller than the
    // source, scale on the GPU so the CPU only ever touches encode-size
    // frames. Falls back to full-size readback (MF scales on CPU) if the
    // D3D11 VideoProcessor is unavailable.
    m_scaleOnGpu = false;
    m_ctx = ctx;
    if ((m_w != m_srcW || m_h != m_srcH) && dev && ctx)
        m_scaleOnGpu = initGpuScaler(dev, ctx);
    m_inW = m_scaleOnGpu ? m_w : m_srcW;
    m_inH = m_scaleOnGpu ? m_h : m_srcH;
    m_fps      = fps > 0 ? fps : 30;
    m_bitrate  = unsigned((bitrateMbps > 0 ? bitrateMbps : 12)) * 1000000u;
    const int segS = segmentSeconds > 0 ? segmentSeconds : 5;
    const int lenS = lengthSeconds  > 0 ? lengthSeconds  : 300;
    m_segTicks = segS * 10000000LL;
    m_frameInterval = 10000000LL / m_fps;
    m_keepSegments = (lenS + segS - 1) / segS;      // ceil(length / segment)
    if (m_keepSegments < 1) m_keepSegments = 1;
    m_audioRate     = audioRateHz;
    m_audioChannels = audioChannels;
    m_segments.clear();
    m_curPath.clear();
    m_discardCurrentSegment = false;
    m_cacheDir = cacheDir;
    m_segIndex = 0;
    m_originTime = -1;
    m_segStartPts = 0;
    m_lastSegPts  = -1;
    m_nextDuePts  = 0;
    m_lastGlobalPts = 0;
    m_audioClockPts = -1;

    QDir().mkpath(m_cacheDir);

    restoreRing();

    if (!mfAddRef())
        return false;
    m_mfOwned = true;

    if (!openSegment()) {
        mfRelease();
        m_mfOwned = false;
        return false;
    }

    m_active = true;
    qInfo().noquote() << QStringLiteral(
        "SegmentRecorder: started %1x%2 -> %3x%4 (%5) @%6fps %7Mbps seg=%8s audio=%9Hz/%10ch -> %11")
        .arg(m_srcW).arg(m_srcH).arg(m_w).arg(m_h)
        .arg(m_scaleOnGpu ? QStringLiteral("GPU scale")
             : (m_w != m_srcW || m_h != m_srcH) ? QStringLiteral("CPU scale")
                                                : QStringLiteral("no scale"))
        .arg(m_fps).arg(bitrateMbps)
        .arg(m_segTicks / 10000000LL)
        .arg(m_audioRate).arg(m_audioChannels)
        .arg(m_cacheDir);
    return true;
}

void SegmentRecorder::restoreRing()
{
    m_segments.clear();
    // Restore the ring from any segments left on disk by a previous session/arm.
    // Without this, every teardown (alt-tab/overlay flicker disarming the buffer)
    // wipes the in-memory m_segments list, so a save right after re-arm yields
    // only the 1-2 s recorded since the re-arm instead of the configured window.
    // Filenames are timestamped "yyyy-MM-dd_HH-mm-ss-zzz_clip.mp4" => lexical
    // sort is chronological.
    //
    // STALE THRESHOLD: must be MUCH wider than the configured replay length. The
    // buffer disarms whenever the game loses foreground focus (alt-tab, overlay,
    // UAC, a loading screen) and re-arms seconds-to-minutes later. The segment
    // files' mtimes are the moment each was FINALISED, so by the time we re-arm,
    // even a healthy 30 s ring is already 30-60 s old. A threshold of just
    // `lengthSeconds` (e.g. 30 s) therefore deletes the entire ring on almost
    // every re-arm — leaving "restored 1/6" and the user getting a ~5 s clip
    // instead of the configured 30 s. 10 minutes is generous enough to survive
    // any normal focus churn while still clearing genuinely orphaned files from
    // a previous play session; the ring trim below enforces the segment cap.
    {
        const qint64 lengthSecs = qint64(m_keepSegments) * (m_segTicks / 10000000LL);
        const qint64 staleSecs  = CaptureUtil::kStaleSegmentMaxAgeSecs;   // 10 min — see note above
        const qint64 nowSecs    = QDateTime::currentSecsSinceEpoch();
        const QStringList files = QDir(m_cacheDir).entryList(
            QStringList() << QStringLiteral("*_clip.mp4"), QDir::Files, QDir::Name);
        for (const QString& f : files) {
            const QString full = m_cacheDir + QLatin1Char('/') + f;
            const qint64 ageSecs = nowSecs - QFileInfo(full).lastModified().toSecsSinceEpoch();
            if (ageSecs > staleSecs && SegmentLease::removeIfUnleased(full))
                continue;
            m_segments.append(full);
        }
        // entryList already sorted chronologically; trim oldest if over capacity.
        trimRing();
        if (!m_segments.isEmpty())
            qInfo().noquote() << QStringLiteral(
                "SegmentRecorder: restored %1/%2 segments from disk (window=%3s)")
                .arg(m_segments.size()).arg(m_keepSegments).arg(lengthSecs);
    }
}

// Build the D3D11 VideoProcessor that downscales source frames to the encode
// size on the GPU. Any failure falls back to full-size CPU readback.
bool SegmentRecorder::initGpuScaler(ID3D11Device* dev, ID3D11DeviceContext* ctx)
{
    if (FAILED(dev->QueryInterface(__uuidof(ID3D11VideoDevice),
                                   reinterpret_cast<void**>(&m_vdev)))
        || FAILED(ctx->QueryInterface(__uuidof(ID3D11VideoContext),
                                      reinterpret_cast<void**>(&m_vctx)))) {
        qInfo() << "SegmentRecorder: D3D11 video interfaces unavailable — CPU scaling";
        releaseGpuScaler();
        return false;
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputWidth   = m_srcW;
    cd.InputHeight  = m_srcH;
    cd.OutputWidth  = m_w;
    cd.OutputHeight = m_h;
    cd.Usage        = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    if (!ok("CreateVideoProcessorEnumerator",
            m_vdev->CreateVideoProcessorEnumerator(&cd, &m_vpEnum))
        || !ok("CreateVideoProcessor", m_vdev->CreateVideoProcessor(m_vpEnum, 0, &m_vp))) {
        releaseGpuScaler();
        return false;
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width            = m_w;
    td.Height           = m_h;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_RENDER_TARGET;
    if (!ok("CreateTexture2D(scaled)", dev->CreateTexture2D(&td, nullptr, &m_scaled))) {
        releaseGpuScaler();
        return false;
    }

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ovd.Texture2D.MipSlice = 0;
    if (!ok("CreateVideoProcessorOutputView",
            m_vdev->CreateVideoProcessorOutputView(m_scaled, m_vpEnum, &ovd, &m_vpOut))) {
        releaseGpuScaler();
        return false;
    }

    qInfo().noquote() << QStringLiteral(
        "SegmentRecorder: GPU downscale active %1x%2 -> %3x%4")
        .arg(m_srcW).arg(m_srcH).arg(m_w).arg(m_h);
    return true;
}

// One GPU blit: source texture -> m_scaled. The input view is per-texture
// (WGC rotates pool textures), but creating it is cheap.
bool SegmentRecorder::scaleFrame(ID3D11Texture2D* tex)
{
    if (!m_vdev || !m_vctx || !m_vp || !m_vpOut)
        return false;

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
    ivd.FourCC = 0;
    ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ivd.Texture2D.MipSlice = 0;
    ivd.Texture2D.ArraySlice = 0;
    ID3D11VideoProcessorInputView* in = nullptr;
    HRESULT hr = m_vdev->CreateVideoProcessorInputView(tex, m_vpEnum, &ivd, &in);
    if (FAILED(hr) || !in) {
        if (!m_scaleFailLogged) {
            m_scaleFailLogged = true;
            qWarning().nospace() << "SegmentRecorder: CreateVideoProcessorInputView failed hr=0x"
                                 << Qt::hex << quint32(hr);
        }
        return false;
    }

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = in;
    hr = m_vctx->VideoProcessorBlt(m_vp, m_vpOut, 0, 1, &stream);
    in->Release();
    if (FAILED(hr) && !m_scaleFailLogged) {
        m_scaleFailLogged = true;
        qWarning().nospace() << "SegmentRecorder: VideoProcessorBlt failed hr=0x"
                             << Qt::hex << quint32(hr);
    }
    return SUCCEEDED(hr);
}

void SegmentRecorder::releaseGpuScaler()
{
    if (m_vpOut)  { m_vpOut->Release();  m_vpOut = nullptr; }
    if (m_scaled) { m_scaled->Release(); m_scaled = nullptr; }
    if (m_vp)     { m_vp->Release();     m_vp = nullptr; }
    if (m_vpEnum) { m_vpEnum->Release(); m_vpEnum = nullptr; }
    if (m_vctx)   { m_vctx->Release();   m_vctx = nullptr; }
    if (m_vdev)   { m_vdev->Release();   m_vdev = nullptr; }
    m_scaleOnGpu = false;
}

// Writer attributes: fragmented MP4 (crash-safe within a segment), no
// throttling (we push in real time), low latency, allow a HW encoder MFT.
IMFSinkWriter* SegmentRecorder::createSegmentSink(const std::wstring& wpath) const
{
    IMFAttributes* attrs = nullptr;
    if (!ok("MFCreateAttributes", MFCreateAttributes(&attrs, 4)))
        return nullptr;
    attrs->SetGUID  (MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_FMPEG4);
    attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    attrs->SetUINT32(MF_LOW_LATENCY, TRUE);

    IMFSinkWriter* writer = nullptr;
    const HRESULT hr = MFCreateSinkWriterFromURL(wpath.c_str(), nullptr, attrs, &writer);
    attrs->Release();
    if (!ok("MFCreateSinkWriterFromURL", hr))
        return nullptr;
    return writer;
}

// H.264 output stream + RGB32 input type on the video stream.
bool SegmentRecorder::addVideoStream(IMFSinkWriter* writer, unsigned long& videoStream) const
{
    // OUTPUT: H.264.
    IMFMediaType* outType = nullptr;
    if (!ok("MFCreateMediaType(out)", MFCreateMediaType(&outType)))
        return false;
    outType->SetGUID  (MF_MT_MAJOR_TYPE,      MFMediaType_Video);
    outType->SetGUID  (MF_MT_SUBTYPE,         MFVideoFormat_H264);
    outType->SetUINT32(MF_MT_AVG_BITRATE,     m_bitrate);
    outType->SetUINT32(MF_MT_INTERLACE_MODE,  MFVideoInterlace_Progressive);
    outType->SetUINT32(MF_MT_MPEG2_PROFILE,   eAVEncH264VProfile_High);
    MFSetAttributeSize (outType, MF_MT_FRAME_SIZE,         m_w, m_h);
    MFSetAttributeRatio(outType, MF_MT_FRAME_RATE,         UINT32(m_fps), 1);
    MFSetAttributeRatio(outType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    HRESULT hr = writer->AddStream(outType, &videoStream);
    outType->Release();
    if (!ok("AddStream(video)", hr)) return false;

    // INPUT: RGB32 (== BGRA), top-down (positive default stride == the flip fix).
    IMFMediaType* inType = nullptr;
    if (!ok("MFCreateMediaType(in)", MFCreateMediaType(&inType)))
        return false;
    inType->SetGUID  (MF_MT_MAJOR_TYPE,      MFMediaType_Video);
    inType->SetGUID  (MF_MT_SUBTYPE,         MFVideoFormat_RGB32);
    inType->SetUINT32(MF_MT_INTERLACE_MODE,  MFVideoInterlace_Progressive);
    inType->SetUINT32(MF_MT_DEFAULT_STRIDE,  UINT32(m_inW * 4));   // +stride => top-down
    MFSetAttributeSize (inType, MF_MT_FRAME_SIZE,         m_inW, m_inH);
    MFSetAttributeRatio(inType, MF_MT_FRAME_RATE,         UINT32(m_fps), 1);
    MFSetAttributeRatio(inType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    // Real-time encoder hint (best-effort).
    IMFAttributes* encParams = nullptr;
    if (SUCCEEDED(MFCreateAttributes(&encParams, 1)))
        encParams->SetUINT32(CODECAPI_AVEncCommonRealTime, TRUE);
    hr = writer->SetInputMediaType(videoStream, inType, encParams);
    inType->Release();
    if (encParams) encParams->Release();
    if (!ok("SetInputMediaType(video)", hr)) return false;
    return true;
}

// AAC output stream + PCM-16 input type — Step 7. Returns true only when the
// audio stream is fully wired (AddStream AND SetInputMediaType accepted); any
// rejection leaves audioStream == 0 and the caller rebuilds video-only.
bool SegmentRecorder::addAudioStream(IMFSinkWriter* writer, unsigned long& audioStream) const
{
    audioStream = 0;
    IMFMediaType* aOutType = nullptr;
    if (!ok("MFCreateMediaType(audio out)", MFCreateMediaType(&aOutType)))
        return false;
    aOutType->SetGUID  (MF_MT_MAJOR_TYPE,   MFMediaType_Audio);
    aOutType->SetGUID  (MF_MT_SUBTYPE,      MFAudioFormat_AAC);
    aOutType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    aOutType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, m_audioRate);
    aOutType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,       m_audioChannels);
    aOutType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, m_audioBitrate / 8);
    aOutType->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29); // AAC-LC
    HRESULT hr = writer->AddStream(aOutType, &audioStream);
    aOutType->Release();
    if (!ok("AddStream(audio)", hr)) {
        audioStream = 0;
        return false;
    }

    // INPUT: PCM-16 (the AAC encoder MFT accepts it natively; WASAPI's
    // float32 is converted to int16 in writeAudio before reaching here).
    IMFMediaType* aInType = nullptr;
    if (!ok("MFCreateMediaType(audio in)", MFCreateMediaType(&aInType))) {
        audioStream = 0;
        return false;
    }
    aInType->SetGUID  (MF_MT_MAJOR_TYPE,   MFMediaType_Audio);
    aInType->SetGUID  (MF_MT_SUBTYPE,      MFAudioFormat_PCM);  // AAC encoder wants PCM-16
    aInType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    aInType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, m_audioRate);
    aInType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,       m_audioChannels);

    IMFAttributes* aEncParams = nullptr;
    if (SUCCEEDED(MFCreateAttributes(&aEncParams, 1)))
        aEncParams->SetUINT32(CODECAPI_AVEncCommonRealTime, TRUE);
    hr = writer->SetInputMediaType(audioStream, aInType, aEncParams);
    aInType->Release();
    if (aEncParams) aEncParams->Release();
    if (!ok("SetInputMediaType(audio)", hr)) {
        qWarning() << "SegmentRecorder: audio input rejected, video-only";
        audioStream = 0;
        return false;
    }
    return true;
}

// Build one segment's sink writer at `path`: fMP4 container, H.264 stream
// (+ optional AAC), input types set, BeginWriting done. Runs on the worker
// OR the prep thread, so it must not touch mutable recorder state; on AAC
// input rejection it retries video-only and returns out.audioStream == 0.
bool SegmentRecorder::buildWriter(const QString& path, PendingWriter& out, bool wantAudio) const
{
    IMFSinkWriter* writer = createSegmentSink(path.toStdWString());
    if (!writer)
        return false;

    unsigned long videoStream = 0;
    unsigned long audioStream = 0;
    if (!addVideoStream(writer, videoStream)) {
        writer->Release();
        return false;
    }

    // If audio was requested but rejected, the sink writer may already have a
    // dangling audio OUTPUT stream (added via AddStream but never given an
    // input media type). That produces a malformed fragmented-MP4 the export
    // reader cannot open, breaking EVERY clip save. Rebuild as pure video-only
    // so each segment stays well-formed; the caller sees audioStream == 0 and
    // disables audio for the rest of this recorder.
    if (wantAudio && !addAudioStream(writer, audioStream)) {
        qWarning() << "SegmentRecorder: audio rejected — rebuilding segment video-only";
        writer->Release();
        QFile::remove(path);             // discard the empty/poisoned file
        return buildWriter(path, out, false);
    }

    const HRESULT hr = writer->BeginWriting();
    if (!ok("BeginWriting", hr)) { writer->Release(); QFile::remove(path); return false; }

    out.writer      = writer;
    out.videoStream = videoStream;
    out.audioStream = audioStream;
    out.path        = path;
    return true;
}

void SegmentRecorder::startPrep()
{
    if (m_prepState.load(std::memory_order_relaxed) != 0)
        return;

    const QString path = uniqueSegmentPath(m_cacheDir);
    const bool wantAudio = (m_audioRate > 0 && m_audioChannels > 0);
    m_prep = PendingWriter();
    m_prepState.store(1, std::memory_order_relaxed);
    // MF stays started for this recorder's whole lifetime (worker holds the
    // ref), so the prep thread only needs its own COM apartment.
    m_prepThread = std::thread([this, path, wantAudio] {
        const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        PendingWriter pw;
        const bool built = buildWriter(path, pw, wantAudio);
        m_prep = pw;
        m_prepState.store(built ? 2 : 3, std::memory_order_release);
        if (SUCCEEDED(hrCo))
            CoUninitialize();
    });
}

void SegmentRecorder::joinPrep()
{
    if (m_prepThread.joinable())
        m_prepThread.join();
}

void SegmentRecorder::discardPrep()
{
    joinPrep();
    if (m_prepState.load(std::memory_order_acquire) == 2 && m_prep.writer) {
        m_prep.writer->Release();        // never wrote a sample — just drop it
        QFile::remove(m_prep.path);
    }
    m_prep = PendingWriter();
    m_prepState.store(0, std::memory_order_relaxed);
}

// Adopt the pre-built next-segment writer when ready (a segment roll then
// costs ~a millisecond instead of the ~300 ms encoder init that used to
// stall the capture pump and drop frames+audio at every boundary), or build
// one inline as fallback. Always schedules the NEXT prep before returning.
bool SegmentRecorder::openSegment()
{
    PendingWriter pw;
    m_lastOpenUsedPrep = false;

    if (m_prepState.load(std::memory_order_acquire) != 0) {
        joinPrep();                       // rarely blocks: 5 s window vs ~0.3 s build
        if (m_prepState.load(std::memory_order_acquire) == 2 && m_prep.writer) {
            pw = m_prep;
            m_lastOpenUsedPrep = true;
        }
        m_prep = PendingWriter();
        m_prepState.store(0, std::memory_order_relaxed);
    }

    if (!pw.writer) {
        const QString path = uniqueSegmentPath(m_cacheDir);
        if (!buildWriter(path, pw, m_audioRate > 0 && m_audioChannels > 0))
            return false;
    }

    // Audio requested but the writer came back video-only: AAC input was
    // rejected — stay video-only for the rest of this recorder.
    if (m_audioRate > 0 && pw.audioStream == 0) {
        qWarning() << "SegmentRecorder: audio disabled for this session (input rejected)";
        m_audioRate = 0;
        m_audioChannels = 0;
    }

    m_writer      = pw.writer;
    m_videoStream = pw.videoStream;
    m_audioStream = pw.audioStream;
    m_curPath     = pw.path;
    m_segVideoFrames  = 0;
    m_segAudioPackets = 0;

    qInfo().noquote() << QStringLiteral("SegmentRecorder: opened %1%2%3")
        .arg(QDir(m_cacheDir).relativeFilePath(m_curPath))
        .arg(m_audioStream > 0 ? QStringLiteral(" (+AAC audio)") : QString())
        .arg(m_lastOpenUsedPrep ? QStringLiteral(" [pre-built]") : QString());

    startPrep();
    return true;
}

void SegmentRecorder::closeSegment()
{
    if (!m_writer)
        return;

    if (m_discardCurrentSegment) {
        m_writer->Release();
        m_writer = nullptr;
        m_videoStream = 0;
        m_audioStream = 0;
        qWarning().noquote()
            << QStringLiteral("SegmentRecorder: discarded in-flight segment after capture "
                              "discontinuity: %1")
                   .arg(QDir(m_cacheDir).relativeFilePath(m_curPath));
        if (!m_curPath.isEmpty())
            QFile::remove(m_curPath);
        m_curPath.clear();
        m_discardCurrentSegment = false;
        return;
    }

    const HRESULT hrFin = m_writer->Finalize();   // writes final index -> playable
    m_writer->Release();
    m_writer = nullptr;

    // A segment with no encoded samples (e.g. a near-static source starved this
    // 5 s window of frames) fails with MF_E_SINK_NO_SAMPLES_PROCESSED and yields a
    // junk file. Discard it so it can't poison a later concat.
    if (FAILED(hrFin)) {
        qWarning().nospace() << "SegmentRecorder: dropping empty segment "
            << QDir(m_cacheDir).relativeFilePath(m_curPath)
            << " (Finalize hr=0x" << Qt::hex << quint32(hrFin) << ")";
        if (!m_curPath.isEmpty())
            QFile::remove(m_curPath);
        m_curPath.clear();
        return;
    }

    if (!m_curPath.isEmpty())
        m_segments.append(m_curPath);
    // Keep the normal window plus leased snapshots; unrelated old files go.
    trimRing();

    // Per-segment stats: frames vs the ~expected count at the target fps make
    // capture starvation visible; lastSegPts is the segment's video span.
    qInfo().noquote() << QStringLiteral(
        "SegmentRecorder: finalized %1 (ring=%2/%3, %4 frames / %5 audio pkts, span=%6ms)")
        .arg(QDir(m_cacheDir).relativeFilePath(m_curPath))
        .arg(m_segments.size()).arg(m_keepSegments)
        .arg(m_segVideoFrames).arg(m_segAudioPackets)
        .arg(m_lastSegPts >= 0 ? m_lastSegPts / 10000 : 0);
    m_curPath.clear();
}

SegmentLease SegmentRecorder::snapshotForSave()
{
    if (!m_active || !m_writer)
        return {};
    // Finalize the in-flight segment so its frames are on disk + in the ring,
    // snapshot the whole ring, then reopen a new segment to keep recording.
    QElapsedTimer rollTimer;
    rollTimer.start();
    closeSegment();
    // Older leased files can remain in m_segments, but are not part of a new
    // save. Acquire before reopening/returning to the worker event loop.
    SegmentLease clip(m_segments.last(qMin(qsizetype(m_keepSegments), m_segments.size())));
    ++m_segIndex;
    if (!openSegment()) { m_active = false; return clip; }
    m_segStartPts = m_lastGlobalPts;   // provisional base until the next frame
    m_lastSegPts  = -1;
    m_rebaseOnNextFrame = true;        // next frame becomes exactly t=0 (IDR)
    qInfo().noquote() << QStringLiteral("SegmentRecorder: snapshot roll took %1 ms (%2)")
        .arg(rollTimer.elapsed())
        .arg(m_lastOpenUsedPrep ? QStringLiteral("pre-built") : QStringLiteral("built inline"));
    return clip;
}

void SegmentRecorder::rollIfDue()
{
    if (!m_active || !m_writer)
        return;
    if (m_lastGlobalPts - m_segStartPts < m_segTicks)
        return;
    QElapsedTimer rollTimer;
    rollTimer.start();
    closeSegment();
    ++m_segIndex;
    if (!openSegment()) { m_active = false; return; }
    m_segStartPts = m_lastGlobalPts;   // provisional base until the next frame
    m_lastSegPts  = -1;
    m_rebaseOnNextFrame = true;        // next frame becomes exactly t=0 (IDR)
    // The roll used to cost ~300 ms (inline encoder init) and dropped frames
    // + audio at every boundary; with the pre-built writer it should be ~1 ms.
    qInfo().noquote() << QStringLiteral("SegmentRecorder: segment roll took %1 ms (%2)")
        .arg(rollTimer.elapsed())
        .arg(m_lastOpenUsedPrep ? QStringLiteral("pre-built") : QStringLiteral("built inline"));
}

bool SegmentRecorder::ensureStaging(ID3D11Device* dev, unsigned w, unsigned h, int dxgiFmt)
{
    if (m_staging && m_stgW == w && m_stgH == h && m_stgFmt == dxgiFmt)
        return true;
    if (m_staging) { m_staging->Release(); m_staging = nullptr; }

    D3D11_TEXTURE2D_DESC sd{};
    sd.Width            = w;
    sd.Height           = h;
    sd.MipLevels        = 1;
    sd.ArraySize        = 1;
    sd.Format           = DXGI_FORMAT(dxgiFmt);   // DXGI_FORMAT_B8G8R8A8_UNORM (87)
    sd.SampleDesc.Count = 1;
    sd.Usage            = D3D11_USAGE_STAGING;
    sd.BindFlags        = 0;
    sd.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags        = 0;

    const HRESULT hr = dev->CreateTexture2D(&sd, nullptr, &m_staging);
    if (!ok("CreateTexture2D(staging)", hr))
        return false;
    m_stgW = w; m_stgH = h; m_stgFmt = dxgiFmt;
    return true;
}

void SegmentRecorder::writeFrame(ID3D11Texture2D* tex, ID3D11Device* dev,
                                 ID3D11DeviceContext* ctx, qint64 sampleTime100ns)
{
    if (!m_active || !m_writer || !tex || !dev || !ctx)
        return;

    if (m_originTime < 0)
        m_originTime = sampleTime100ns;
    long long gpts = sampleTime100ns - m_originTime;
    if (gpts < 0) gpts = 0;
    m_lastGlobalPts = gpts;

    // --- fps throttle: schedule-anchored. Accept a frame once it is within
    // half an interval of its due time — a 60 Hz source feeding a 30 fps
    // target then locks onto every 2nd frame instead of beating between
    // 33/50 ms gaps (visible judder in saved clips).
    if (m_lastSegPts >= 0 && gpts + m_frameInterval / 2 < m_nextDuePts)
        return;

    // --- roll first so this frame becomes the new segment's t=0 IDR keyframe ---
    rollIfDue();
    if (!m_writer) return;   // roll failed
    if (m_rebaseOnNextFrame) {
        // First frame after a roll/snapshot reopen: make it EXACTLY t=0 of
        // the new segment. Without this every segment started ~one frame
        // late, and the exporter's concat then inserted that gap at every
        // segment boundary — a periodic hitch in saved clips.
        m_segStartPts = gpts;
        m_rebaseOnNextFrame = false;
    }

    // --- GPU downscale (if active), then readback into a staging texture ------
    // The staging copy + row memcpy below run at ENCODE size when the GPU
    // scaler is on — that is the whole performance point: a 4K source encoded
    // at 1080p reads back ~8 MB/frame instead of ~33 MB.
    ID3D11Texture2D* readSrc = tex;
    unsigned rbW, rbH;
    int rbFmt;
    if (m_scaleOnGpu) {
        if (!scaleFrame(tex))
            return;   // MF input type is encode-size; can't feed the raw frame
        readSrc = m_scaled;
        rbW = m_w;
        rbH = m_h;
        rbFmt = int(DXGI_FORMAT_B8G8R8A8_UNORM);
    } else {
        D3D11_TEXTURE2D_DESC srcDesc{};
        tex->GetDesc(&srcDesc);
        rbW = srcDesc.Width;
        rbH = srcDesc.Height;
        rbFmt = int(srcDesc.Format);
    }
    if (!ensureStaging(dev, rbW, rbH, rbFmt))
        return;
    ctx->CopyResource(m_staging, readSrc);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (!ok("Map(staging)", ctx->Map(m_staging, 0, D3D11_MAP_READ, 0, &mapped)))
        return;

    const DWORD dstStride = rbW * 4;                       // tight, top-down
    const DWORD bufLen    = dstStride * rbH;

    IMFSample*      sample = nullptr;
    IMFMediaBuffer* buffer = nullptr;
    bool built = false;
    if (ok("MFCreateSample", MFCreateSample(&sample)) &&
        ok("MFCreateMemoryBuffer", MFCreateMemoryBuffer(bufLen, &buffer))) {
        BYTE* dst = nullptr;
        if (ok("buffer->Lock", buffer->Lock(&dst, nullptr, nullptr))) {
            const BYTE* src = static_cast<const BYTE*>(mapped.pData);
            for (UINT y = 0; y < rbH; ++y)                 // RowPitch may exceed w*4
                memcpy(dst + size_t(y) * dstStride,
                       src + size_t(y) * mapped.RowPitch, dstStride);
            buffer->Unlock();
            buffer->SetCurrentLength(bufLen);
            sample->AddBuffer(buffer);
            built = true;
        }
    }
    ctx->Unmap(m_staging, 0);

    if (built) {
        const long long segPts = gpts - m_segStartPts;
        long long dur = (m_lastSegPts >= 0 && segPts > m_lastSegPts)
                            ? segPts - m_lastSegPts : m_frameInterval;
        sample->SetSampleTime(segPts);
        sample->SetSampleDuration(dur);
        if (ok("WriteSample(video)", m_writer->WriteSample(DWORD(m_videoStream), sample))) {
            m_lastSegPts = segPts;
            ++m_segVideoFrames;
            // Schedule-anchored cadence: advance by exactly one interval so
            // acceptance stays locked to the fps grid. After a source stall
            // (loading screen, static scene — WGC stops delivering frames),
            // resync forward instead of burst-accepting to "catch up" on a
            // timeline the game never rendered.
            m_nextDuePts += m_frameInterval;
            if (m_nextDuePts <= gpts)
                m_nextDuePts = gpts + m_frameInterval;
        }
    }

    if (buffer) buffer->Release();
    if (sample) sample->Release();
}

void SegmentRecorder::writeAudio(const float* samples, unsigned numFrames, qint64 audioTime100ns)
{
    if (!m_active || !m_writer || m_audioStream == 0 || !samples || numFrames == 0)
        return;
    if (m_audioRate == 0 || m_audioChannels == 0)
        return;

    // Audio shares the VIDEO origin (caller passes both streams on one
    // clock), so the AAC track lines up with the frames instead of starting
    // its own timeline at whatever moment the first audio packet arrived.
    if (m_originTime < 0)
        return;   // no video frame yet — nothing to sync against
    const long long wall = audioTime100ns - m_originTime;
    if (wall < 0)
        return;   // predates the first video frame — drop

    // Audio PTS comes from a CONTINUOUS SAMPLE CLOCK, not the poll wall
    // clock. Poll timestamps bunch up after any capture stall (the segment
    // roll, a busy tick): a burst of drained packets used to arrive with
    // near-identical times and the monotonic guard threw them away —
    // measured ~330-520 ms audio holes at every 5 s boundary of saved clips,
    // audible/visible as the clip "pausing" every few seconds. The clock
    // anchors on the first packet, advances by exactly numFrames/rate per
    // packet, and re-anchors only on real drift (device stall > 0.5 s).
    if (m_audioClockPts < 0) {
        m_audioClockPts = wall;
    } else if (qAbs(wall - m_audioClockPts) > 5000000LL) {
        qInfo() << "SegmentRecorder: audio clock re-anchored, drift was"
                << (wall - m_audioClockPts) / 10000 << "ms";
        m_audioClockPts = wall;
    }
    const long long gpts = m_audioClockPts;
    m_audioClockPts += (static_cast<long long>(numFrames) * 10000000LL) / m_audioRate;

    rollIfDue();
    if (!m_writer)
        return;

    long long segPts = gpts - m_segStartPts;
    if (segPts < 0) {
        // Boundary packet straddling the rebase — clamp instead of dropping
        // (at most ~one packet, keeps the sample clock unbroken).
        if (segPts < -m_frameInterval)
            return;
        segPts = 0;
    }

    const DWORD numSamples = DWORD(numFrames) * m_audioChannels;
    const DWORD bytes = numSamples * sizeof(short);   // PCM-16 for the AAC encoder
    IMFSample* sample = nullptr;
    IMFMediaBuffer* buffer = nullptr;
    bool built = false;
    if (ok("MFCreateSample(audio)", MFCreateSample(&sample)) &&
        ok("MFCreateMemoryBuffer(audio)", MFCreateMemoryBuffer(bytes, &buffer))) {
        BYTE* dst = nullptr;
        if (ok("audio buffer->Lock", buffer->Lock(&dst, nullptr, nullptr))) {
            // WASAPI loopback gives interleaved float32 [-1,1]; the AAC encoder
            // takes PCM-16, so clip + scale here.
            short* dst16 = reinterpret_cast<short*>(dst);
            for (DWORD i = 0; i < numSamples; ++i) {
                float v = samples[i] * 32767.0f;
                if (v > 32767.0f) v = 32767.0f;
                if (v < -32768.0f) v = -32768.0f;
                dst16[i] = static_cast<short>(v);
            }
            buffer->Unlock();
            buffer->SetCurrentLength(bytes);
            sample->AddBuffer(buffer);
            built = true;
        }
    }

    if (built) {
        const long long dur = (static_cast<long long>(numFrames) * 10000000LL) / m_audioRate;
        sample->SetSampleTime(segPts);
        sample->SetSampleDuration(dur);
        if (ok("WriteSample(audio)", m_writer->WriteSample(DWORD(m_audioStream), sample)))
            ++m_segAudioPackets;
    }

    if (buffer) buffer->Release();
    if (sample) sample->Release();
}

void SegmentRecorder::trimRing()
{
    SegmentLease::trim(m_segments, m_keepSegments);
}

void SegmentRecorder::end()
{
    discardPrep();
    if (m_active || m_writer)
        closeSegment();
    if (m_staging) { m_staging->Release(); m_staging = nullptr; m_stgW = m_stgH = 0; }
    releaseGpuScaler();
    m_ctx = nullptr;
    if (m_mfOwned) { mfRelease(); m_mfOwned = false; }
    m_active = false;
}
