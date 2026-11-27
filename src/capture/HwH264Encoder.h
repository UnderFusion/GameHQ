#pragma once

#include <QString>

#include <deque>
#include <vector>

struct IMFActivate;
struct IMFMediaEventGenerator;
struct IMFMediaType;
struct IMFSample;
struct IMFTransform;

// One hardware H.264 encoder MFT kept for a whole recording session (#39).
//
// Building a fresh hardware encoder inside an IMFSinkWriter for every replay
// segment leaks Windows handles (measured +9.85 per segment on the NVIDIA
// MFT). Driving the same MFT directly and keeping it across segments stays
// flat; the segment files then receive the already encoded H.264 samples
// through a pass-through sink writer.
//
// The MFT is asynchronous: input is accepted only after METransformNeedInput
// and output arrives as METransformHaveOutput. encode() and takeOutput()
// never block - they pump whatever events are queued. Frames that arrive
// while the encoder has not asked for input wait in a small bounded queue;
// beyond it the oldest is dropped and counted. drain() is the only blocking
// call and is bounded by a timeout.
//
// Single-thread affinity: create, feed and close on the capture worker.
// Input is NV12 in system memory at the encode size.
class HwH264Encoder
{
public:
    struct Settings {
        unsigned width = 0, height = 0;   // even
        unsigned fps = 30;
        unsigned bitrate = 12000000;      // bits per second
    };

    HwH264Encoder() = default;
    ~HwH264Encoder();
    HwH264Encoder(const HwH264Encoder&) = delete;
    HwH264Encoder& operator=(const HwH264Encoder&) = delete;

    // Picks the first hardware H.264 encoder (sorted like the sink writer's
    // own choice) that accepts the types. On failure, why explains it.
    bool open(const Settings& s, QString* why);
    void close();

    bool isOpen() const { return m_mft != nullptr; }
    bool failed() const { return m_failed; }
    const QString& failure() const { return m_failure; }
    const QString& encoderName() const { return m_name; }

    // Encoded output type with MF_MT_MPEG_SEQUENCE_HEADER, for pass-through
    // writers. Borrowed; valid while open.
    IMFMediaType* outputType() const { return m_outType; }

    // Queue one NV12 frame. forceKeyframe makes it an IDR frame (segment start).
    // Returns false once the encoder has failed.
    bool encode(IMFSample* nv12, bool forceKeyframe);
    // Move finished output samples (in decode order) into out; caller releases.
    void takeOutput(std::vector<IMFSample*>& out);
    // Flush everything queued and wait (bounded) until the encoder returned
    // all of it; the encoder accepts new input afterwards.
    bool drain(std::vector<IMFSample*>& out, int timeoutMs);

    unsigned droppedFrames() const { return m_dropped; }
    size_t queueLimit() const { return m_maxPending; }
    // Frames handed over but not yet returned: waiting in our queue plus
    // inside the MFT. Times the frame interval, this is the buffering delay.
    unsigned framesInFlight() const { return unsigned(m_pending.size()) + m_inMft; }

    // Shift presentation AND decode timestamp of an encoded sample by delta.
    static void shiftSampleTime(IMFSample* sample, long long delta);

    // Tests: make the next encode() fail as if the MFT had rejected the
    // input, or the next drain() fail as if the encoder had stalled.
    void injectFailure() { m_injectFailure = true; }
    void injectDrainFailure() { m_injectDrainFailure = true; }

private:
    struct Pending { IMFSample* sample; bool key; };
    bool tryActivate(IMFActivate* act, const Settings& s, QString* why);
    bool pumpEvents(bool* drainComplete);
    bool submitPending();
    bool collectOutput();
    bool refreshOutputType();
    void fail(const QString& why);
    void releaseMft();

    IMFActivate*            m_act = nullptr;
    IMFTransform*           m_mft = nullptr;
    IMFMediaEventGenerator* m_events = nullptr;
    IMFMediaType*           m_outType = nullptr;
    Settings                m_settings;
    QString                 m_name;
    bool                    m_providesSamples = false;
    unsigned long           m_outBufferSize = 0;
    int                     m_needInput = 0;
    bool                    m_draining = false;
    std::deque<Pending>     m_pending;
    std::vector<IMFSample*> m_output;
    unsigned                m_dropped = 0;
    unsigned                m_inMft = 0;      // accepted by ProcessInput, no output yet
    bool                    m_failed = false;
    bool                    m_injectFailure = false;
    bool                    m_injectDrainFailure = false;
    QString                 m_failure;

    // Frames waiting for the encoder: one second at the target rate, capped
    // at 128 MB of NV12 and never below 8. Rides out a full encoder stall
    // (measured up to ~1.2 s under heavy GPU load) while staying bounded;
    // the per-segment path's sink writer queues RGB32 without any limit.
    static constexpr size_t kMinPending = 8;
    static constexpr size_t kPendingBytesCap = 128u * 1024u * 1024u;
    size_t                  m_maxPending = kMinPending;
};
