#pragma once

// 0.5 Step 7 / 0.8.0 — WASAPI loopback audio capture.
//
// Two sources, both normalized by Windows to 48 kHz stereo float32 for AAC:
//   Game    — application loopback of one process tree (Windows build 20348+),
//             independent of the output device that process renders to;
//   Desktop — loopback of the default render endpoint (everything audible).
// Runs on the same MTA worker thread as
// the WGC frame pump; all COM/WASAPI calls are on that thread.
//
// Flow:
//   start(Game) → ActivateAudioInterfaceAsync(VAD\Process_Loopback)
//          → Initialize(LOOPBACK | EVENTCALLBACK | AUTOCONVERTPCM) → SetEventHandle
//          → GetService(IAudioCaptureClient) → Start
//   start(Desktop) → IMMDeviceEnumerator → GetDefaultAudioEndpoint → Activate(IAudioClient)
//          → GetMixFormat → Initialize(LOOPBACK | AUTOCONVERTPCM)
//          → GetService(IAudioCaptureClient) → Start
//   poll() → IAudioCaptureClient::GetBuffer → AAC encoder → sample queue
//   stop()  → Stop + release
//
// start() never switches sources on its own; a failed game-audio start is
// reported to the caller, which must not silently record the desktop instead.

#include <QObject>
#include <QString>

#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

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

    enum class Mode { Desktop, Game };

    // Start capturing. Game mode records targetPid and its child processes;
    // Desktop mode records the default render endpoint (targetPid is only
    // logged). Returns true once the stream is running.
    bool start(Mode mode = Mode::Desktop, unsigned long targetPid = 0);

    // Whether this Windows build offers per-process (application) loopback.
    static bool processLoopbackSupported();

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
    Mode mode() const { return m_mode; }
    // Friendly name of the captured render endpoint, or "game process <pid>"
    // in game mode (diagnostics only).
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
    bool startStream(unsigned sampleRate, unsigned channels, const QString& label);
    unsigned readPacket(float* outBuf, unsigned maxFrames, long long* outTimestamp100ns);
    void startDrainThread();
    void stopDrainThread();
    void pushFifo(const float* samples, unsigned frames, long long timestamp100ns);
    unsigned popFifo(float* outBuf, unsigned maxFrames, long long* outTimestamp100ns);
    void release();
    void noteReadError(const char* call, long hr);
    void logSummary() const;

    IAudioClient*          m_client   = nullptr;
    IAudioCaptureClient*   m_capture  = nullptr;
    IMMDevice*             m_device   = nullptr;
    IMMDeviceEnumerator*   m_enumerator = nullptr;
    void*                  m_event = nullptr;   // HANDLE; game mode event callback
    Mode                   m_mode = Mode::Desktop;
    unsigned long          m_targetPid = 0;

    unsigned m_sampleRate = 0;
    unsigned m_channels   = 0;
    unsigned m_frameSize  = 0;       // bytes per frame (sampleRate * channels * sizeof(float))
    unsigned m_bufFrames  = 0;       // buffer size in frames

    // First poll timestamp — used to compute relative offsets
    long long m_startQpc100ns = 0;
    // Atomic: game audio's drain thread sets these while the worker reads them.
    std::atomic<bool> m_active{false};
    std::atomic<bool> m_deviceInvalidated{false};
    std::atomic<bool> m_failed{false};
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

    // Game audio drain thread and the FIFO it fills (see startDrainThread).
    struct Chunk {
        long long timestamp100ns = 0;
        std::vector<float> samples;
    };
    std::thread       m_drainThread;
    std::atomic<bool> m_stopDrain{false};
    std::mutex        m_fifoMutex;
    std::deque<Chunk> m_fifo;
    size_t            m_fifoFrames = 0;
    quint64           m_fifoDroppedFrames = 0;
};
