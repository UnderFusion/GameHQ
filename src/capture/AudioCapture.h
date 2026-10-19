#pragma once

// 0.5 Step 7 — WASAPI loopback audio capture.
//
// Captures system audio from the default render device in shared loopback mode,
// normalized by Windows to 48 kHz stereo float32 for AAC encoding.
// Runs on the same MTA worker thread as
// the WGC frame pump; all COM/WASAPI calls are on that thread.
//
// Flow:
//   start() → IMMDeviceEnumerator → GetDefaultAudioEndpoint → Activate(IAudioClient)
//          → GetMixFormat → Initialize(LOOPBACK | AUTOCONVERTPCM)
//          → GetService(IAudioCaptureClient) → Start
//   poll() → IAudioCaptureClient::GetBuffer → AAC encoder → sample queue
//   stop()  → Stop + release
//
// Process-loopback remains disabled pending validation of its activation path.

#include <QObject>
#include <QString>

#ifndef WAVE_FORMAT_PCM
#define WAVE_FORMAT_PCM 1
#endif

// fwd
struct IAudioClient;
struct IAudioCaptureClient;
struct IMMDevice;
struct IMMDeviceEnumerator;

class AudioCapture : public QObject
{
    Q_OBJECT
public:
    explicit AudioCapture(QObject* parent = nullptr);
    ~AudioCapture() override;

    // Start capturing from the default render endpoint.
    // targetPid is retained for diagnostics; process-scoped capture is disabled.
    // Returns true if desktop audio capture is active.
    bool start(unsigned long targetPid = 0);

    // Stop capture and release all WASAPI/COM resources.
    void stop();

    // Poll available audio frames. Copies up to maxFrames worth of interleaved
    // float32 samples into outBuf. Returns the number of frames actually copied.
    // outTimestamp100ns is the device QPC position of the first sample in this
    // batch, relative to startQpc100ns(). Returns 0 both for legitimate silence
    // (loopback delivers no packets while nothing renders) and for errors; the
    // two are told apart by deviceInvalidated()/failed(). Safe when inactive.
    unsigned poll(float* outBuf, unsigned maxFrames, long long* outTimestamp100ns);

    bool isActive() const { return m_active; }
    bool deviceInvalidated() const { return m_deviceInvalidated; }
    // Unexpected WASAPI read errors persisted long enough that the stream can
    // no longer be trusted; the caller must not keep recording "silence".
    bool failed() const { return m_failed; }
    // Friendly name of the captured render endpoint (diagnostics only).
    QString endpointName() const { return m_endpointName; }
    unsigned sampleRate() const { return m_sampleRate; }
    unsigned channels() const { return m_channels; }
    // Maximum buffered packet capacity reported by WASAPI, in output frames.
    unsigned bufferFrames() const { return m_bufFrames; }
    // QPC epoch (100 ns) of this capture's t=0 — lets the caller re-express
    // poll() timestamps on a shared clock with the video frames.
    long long startQpc100ns() const { return m_startQpc100ns; }

signals:
    void errorOccurred(const QString& message);

private:
    bool initDesktopLoopback();
    bool initDesktopLoopbackInner();
    bool initProcessLoopback(unsigned long pid);
    void release();
    void noteReadError(const char* call, long hr);
    void logSummary() const;

    IAudioClient*          m_client   = nullptr;
    IAudioCaptureClient*   m_capture  = nullptr;
    IMMDevice*             m_device   = nullptr;
    IMMDeviceEnumerator*   m_enumerator = nullptr;

    unsigned m_sampleRate = 0;
    unsigned m_channels   = 0;
    unsigned m_frameSize  = 0;       // bytes per frame (sampleRate * channels * sizeof(float))
    unsigned m_bufFrames  = 0;       // buffer size in frames

    // First poll timestamp — used to compute relative offsets
    long long m_startQpc100ns = 0;
    bool      m_active = false;
    bool      m_deviceInvalidated = false;
    bool      m_failed = false;
    QString   m_endpointName;

    // Bounded diagnostics: counters only, never sample content beyond a peak.
    int       m_consecutiveErrors = 0;
    quint64   m_packets = 0;
    quint64   m_frames = 0;
    quint64   m_silentPackets = 0;
    quint64   m_discontinuities = 0;
    quint64   m_readErrors = 0;
    float     m_peak = 0.0f;
    long long m_firstPacketQpc100ns = 0;
};
