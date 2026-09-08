#pragma once

#include "capture/SegmentLease.h"

#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <atomic>
#include <string>
#include <thread>

// Forward declarations (avoid pulling d3d11.h / mfreadwrite.h into the header).
struct ID3D11Texture2D;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11VideoDevice;
struct ID3D11VideoContext;
struct ID3D11VideoProcessor;
struct ID3D11VideoProcessorEnumerator;
struct ID3D11VideoProcessorOutputView;
struct IMFSinkWriter;

// 0.5 Step 4/7 — rolling H.264+AAC / fragmented-MP4 replay segment writer.
//
// One IMFSinkWriter per ~N-second segment (Finalize + new file each roll), with
// TWO streams: video (H.264) and audio (AAC). Both are fed from the MTA worker.
// Video: BGRA (DXGI_FORMAT_B8G8R8A8_UNORM) → RGB32 input → auto Color Converter
// MFT → H.264 encoder. Audio: interleaved float32 PCM → auto AAC encoder.
// Top-down orientation via positive MF_MT_DEFAULT_STRIDE.
//
// Single-thread affinity: construct, feed, and end ALL on the MTA frame-pump
// worker thread (the one that owns the D3D immediate context). MFStartup/MFShutdown
// are ref-counted internally, so multiple recorders on that thread are safe.
class SegmentRecorder
{
public:
    SegmentRecorder() = default;
    ~SegmentRecorder();

    SegmentRecorder(const SegmentRecorder&) = delete;
    SegmentRecorder& operator=(const SegmentRecorder&) = delete;

    // Encodes captured frames at (encodeWidth,encodeHeight) rounded down to
    // even. fps is the TARGET rate (source frames beyond it are dropped). Opens
    // segment 0.
    // lengthSeconds sizes the on-disk ring: only ~lengthSeconds of segments are
    // kept, oldest deleted as it rolls (Step 5).
    // dev/ctx: the D3D device the source textures live on. When the encode size
    // is smaller than the source, a D3D11 VideoProcessor downscales ON THE GPU
    // before readback — a 4K game encoded at 1080p then reads back ~8 MB/frame
    // instead of ~33 MB and Media Foundation color-converts the small frame,
    // cutting the CPU cost of recording roughly 4x. Falls back to full-size
    // readback if the video processor is unavailable.
    // If audioRateHz > 0 and audioChannels > 0, an AAC audio stream is also
    // added (Step 7 — WASAPI loopback).
    bool begin(int sourceWidth, int sourceHeight, int encodeWidth, int encodeHeight,
               int fps, int bitrateMbps, int segmentSeconds, int lengthSeconds,
               const QString& cacheDir, ID3D11Device* dev, ID3D11DeviceContext* ctx,
               unsigned audioRateHz = 0, unsigned audioChannels = 0);

    // Freeze the buffer for a save (Step 6): finalize the in-flight segment,
    // lease the newest chronological window covering ~lengthSeconds, and
    // immediately reopen a new segment so recording continues uninterrupted.
    // Empty if inactive; the lease outlives this recorder if necessary.
    SegmentLease snapshotForSave();

    // Feed one captured video frame. sampleTime100ns is a monotonic capture
    // timestamp in 100-ns units (e.g. QElapsedTimer::nsecsElapsed()/100).
    void writeFrame(ID3D11Texture2D* tex, ID3D11Device* dev,
                    ID3D11DeviceContext* ctx, qint64 sampleTime100ns);

    // Feed one audio buffer. samples is interleaved float32 PCM, numFrames is
    // the count of audio frames (not samples). audioTime100ns MUST be on the
    // SAME clock/epoch as writeFrame's sampleTime100ns — audio shares the
    // video origin so the streams stay in sync. Samples that predate the
    // first video frame are dropped.
    void writeAudio(const float* samples, unsigned numFrames, qint64 audioTime100ns);

    // Opportunistic cleanup after export; never releases another task's lease.
    void trimRing();

    // Roll to a fresh segment if the current one has reached its duration.
    void rollIfDue();

    // Finalize the in-flight segment and release everything.
    void end();

    // Mark the in-flight file as unusable after a device/capture discontinuity.
    // end() will release it without Finalize and remove it from disk.
    void discardCurrentSegment() { m_discardCurrentSegment = true; }

    bool isActive() const { return m_active; }
    unsigned segmentCount() const { return m_segIndex + (m_writer ? 1 : 0); }
    bool hasAudio() const { return m_audioStream > 0; }

private:
    friend class TestSegmentRecorder;
    void restoreRing();
    // A fully-built sink writer for one segment (streams added, BeginWriting
    // done). Built either synchronously (first segment, fallback) or by the
    // prep thread below so a segment roll doesn't stall the capture pump for
    // the ~300 ms the H.264/AAC encoder init costs — that stall was dropping
    // both frames and audio at every 5 s boundary of saved clips.
    struct PendingWriter {
        IMFSinkWriter* writer = nullptr;
        unsigned long  videoStream = 0;
        unsigned long  audioStream = 0;   // 0 = no audio stream
        QString        path;
    };

    bool buildWriter(const QString& path, PendingWriter& out, bool wantAudio) const;
    // buildWriter stages, in call order. All const — they run on the worker OR
    // the prep thread and must not touch mutable recorder state.
    IMFSinkWriter* createSegmentSink(const std::wstring& wpath) const;
    bool addVideoStream(IMFSinkWriter* writer, unsigned long& videoStream) const;
    bool addAudioStream(IMFSinkWriter* writer, unsigned long& audioStream) const;
    void startPrep();                 // kick off building the NEXT segment's writer
    void joinPrep();                  // wait for the prep thread (if running)
    void discardPrep();               // join + release/delete an unused prepared writer
    bool openSegment();
    void closeSegment();
    bool ensureStaging(ID3D11Device* dev, unsigned w, unsigned h, int dxgiFmt);
    bool initGpuScaler(ID3D11Device* dev, ID3D11DeviceContext* ctx);
    bool scaleFrame(ID3D11Texture2D* tex);   // Blt source -> m_scaled (GPU)
    void releaseGpuScaler();

    // encoder params
    unsigned  m_srcW = 0, m_srcH = 0;        // even input dims
    unsigned  m_w = 0, m_h = 0;              // even encode dims
    unsigned  m_inW = 0, m_inH = 0;          // MF input dims (= m_w/m_h when GPU-scaled)
    int       m_fps = 30;
    unsigned  m_bitrate = 12000000;
    long long m_segTicks = 50000000;         // segmentSeconds * 1e7
    long long m_frameInterval = 333333;      // 1e7 / fps
    QString   m_cacheDir;

    // audio params (Step 7)
    unsigned  m_audioRate     = 0;
    unsigned  m_audioChannels = 0;
    unsigned  m_audioBitrate  = 192000;       // 192 kbps AAC

    // on-disk ring (Step 5): finalized segment file paths, oldest first
    QStringList m_segments;
    QString     m_curPath;                   // current in-flight segment file
    int         m_keepSegments = 12;         // ceil(lengthSeconds / segmentSeconds)
    bool        m_discardCurrentSegment = false;

    // current segment
    IMFSinkWriter* m_writer = nullptr;
    unsigned long  m_videoStream = 0;        // SinkWriter stream indices
    unsigned long  m_audioStream = 0;
    unsigned       m_segIndex = 0;

    // timing (100-ns ticks; global = since first frame)
    long long m_originTime    = -1;
    long long m_segStartPts   = 0;
    long long m_lastSegPts    = -1;
    long long m_nextDuePts    = 0;           // schedule-anchored fps throttle
    long long m_lastGlobalPts = 0;
    bool      m_rebaseOnNextFrame = false;   // first frame after reopen = exact t=0

    // audio timing: continuous SAMPLE-COUNT clock (100-ns ticks on the video
    // timeline). Anchored on the first packet, advanced by numFrames/rate per
    // packet, re-anchored only on large wall-clock drift. Never derived from
    // poll timestamps — those bunch up after a stall and used to get dropped.
    long long m_audioClockPts = -1;

    // per-segment stats for the diagnostics log
    int m_segVideoFrames = 0;
    int m_segAudioPackets = 0;

    // next-segment writer prepared off-thread (see PendingWriter)
    // 0=idle 1=building 2=ready 3=failed
    std::atomic<int> m_prepState{0};
    std::thread      m_prepThread;
    PendingWriter    m_prep;
    bool m_lastOpenUsedPrep = false;

    // reusable readback staging texture
    ID3D11Texture2D* m_staging = nullptr;
    unsigned m_stgW = 0, m_stgH = 0;
    int      m_stgFmt = 0;

    // GPU downscaler (D3D11 VideoProcessor); all null when m_scaleOnGpu false
    bool m_scaleOnGpu = false;
    ID3D11DeviceContext*              m_ctx      = nullptr;   // borrowed (pipeline owns)
    ID3D11VideoDevice*                m_vdev     = nullptr;
    ID3D11VideoContext*               m_vctx     = nullptr;
    ID3D11VideoProcessorEnumerator*   m_vpEnum   = nullptr;
    ID3D11VideoProcessor*             m_vp       = nullptr;
    ID3D11Texture2D*                  m_scaled   = nullptr;   // encode-size BGRA RT
    ID3D11VideoProcessorOutputView*   m_vpOut    = nullptr;
    bool m_scaleFailLogged = false;

    bool m_active = false;
    bool m_mfOwned = false;                  // this instance holds an MFStartup ref
};
