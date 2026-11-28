#include "capture/HwH264Encoder.h"

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <codecapi.h>
#include <strmif.h>

#include <QDebug>
#include <QElapsedTimer>
#include <QHash>
#include <QMutex>
#include <QSet>
#include <QStringList>
#include <QThread>

#include <algorithm>
#include <cstring>

namespace {

// ICodecAPI's IID; spelled out because some MinGW headers lack __uuidof for it.
const GUID kIID_ICodecAPI = {0x901db4c7, 0x31ce, 0x41a2, {0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda}};

// Input timestamps are shifted by this much inside the encoder so the primer
// frame (t=0) always precedes the first real frame, whatever its time.
constexpr LONGLONG kTimeOffset = 10000000;   // 1 s

QString hrText(const char* what, HRESULT hr)
{
    return QStringLiteral("%1 hr=0x%2").arg(QString::fromLatin1(what))
        .arg(quint32(hr), 8, 16, QLatin1Char('0'));
}

IMFMediaType* videoType(const GUID& subtype, const HwH264Encoder::Settings& s, bool encoded)
{
    IMFMediaType* t = nullptr;
    if (FAILED(MFCreateMediaType(&t)))
        return nullptr;
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, subtype);
    t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(t, MF_MT_FRAME_SIZE, s.width, s.height);
    MFSetAttributeRatio(t, MF_MT_FRAME_RATE, s.fps, 1);
    MFSetAttributeRatio(t, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (encoded) {
        // Same output type SegmentRecorder::addVideoStream gives the sink writer.
        t->SetUINT32(MF_MT_AVG_BITRATE, s.bitrate);
        t->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    }
    return t;
}

void setCodecValue(IMFTransform* mft, const GUID& key, ULONG value)
{
    ICodecAPI* codec = nullptr;
    if (FAILED(mft->QueryInterface(kIID_ICodecAPI, reinterpret_cast<void**>(&codec))) || !codec)
        return;
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = value;
    codec->SetValue(&key, &v);
    codec->Release();
}

// MFT_ENUM_ADAPTER_LUID: which GPU a hardware MFT belongs to.
const GUID kAdapterLuid = {0x1d39518c, 0xe220, 0x4da8, {0xa0, 0x7f, 0xba, 0x17, 0x25, 0x52, 0xd6, 0xb1}};

// Encoders that refused a configuration earlier in this process. Some vendor
// MFTs keep an event, a thread-pool wait and a registry key after every
// ActivateObject/ShutdownObject pair (measured: AMDh264Encoder on an
// AMD + NVIDIA PC, +3 handles per attempt). Retrying a known refusal on
// every capture rebuild therefore grew the handle count (b4-handles).
QMutex g_refusedMutex;
QSet<QString> g_refused;

// Identity of one enumerated encoder for one configuration: CLSID, adapter
// and its position among identical entries, plus size and rate.
QString encoderKey(IMFActivate* act, const HwH264Encoder::Settings& s, QHash<QString, int>* seen)
{
    GUID clsid{};
    act->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &clsid);
    unsigned char luid[8] = {};
    UINT32 luidSize = 0;
    act->GetBlob(kAdapterLuid, luid, sizeof(luid), &luidSize);
    const QString identity = QStringLiteral("%1-%2-%3")
        .arg(clsid.Data1, 8, 16, QLatin1Char('0')).arg(clsid.Data2, 4, 16, QLatin1Char('0'))
        .arg(QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(luid), int(luidSize)).toHex()));
    const int occurrence = (*seen)[identity]++;
    return QStringLiteral("%1#%2@%3x%4@%5@%6").arg(identity).arg(occurrence)
        .arg(s.width).arg(s.height).arg(s.fps).arg(s.bitrate);
}

bool wasRefused(const QString& key)
{
    QMutexLocker lock(&g_refusedMutex);
    return g_refused.contains(key);
}

void setRefused(const QString& key, bool refused)
{
    QMutexLocker lock(&g_refusedMutex);
    // A handful of encoders x the sizes a session uses; the bound only guards
    // against pathological resolution churn.
    constexpr qsizetype kMaxRefusals = 64;
    if (refused && g_refused.size() >= kMaxRefusals)
        g_refused.clear();
    if (refused)
        g_refused.insert(key);
    else
        g_refused.remove(key);
}

} // namespace

HwH264Encoder::~HwH264Encoder()
{
    close();
}

bool HwH264Encoder::open(const Settings& s, QString* why)
{
    close();
    m_settings = s;
    {
        const size_t frameBytes = size_t(s.width) * s.height * 3 / 2;
        const size_t byBytes = frameBytes ? kPendingBytesCap / frameBytes : kMinPending;
        m_maxPending = std::max(kMinPending, std::min<size_t>(s.fps ? s.fps : 30, byBytes));
    }
    m_failed = false;
    m_failure.clear();
    m_dropped = 0;

    MFT_REGISTER_TYPE_INFO outInfo{ MFMediaType_Video, MFVideoFormat_H264 };
    IMFActivate** acts = nullptr;
    UINT32 count = 0;
    const HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                                 MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                                 nullptr, &outInfo, &acts, &count);
    if (FAILED(hr) || count == 0) {
        if (why) *why = FAILED(hr) ? hrText("MFTEnumEx", hr) : QStringLiteral("no hardware H.264 encoder");
        if (acts) CoTaskMemFree(acts);
        return false;
    }
    QStringList rejected;
    QHash<QString, int> seen;
    QStringList keys;
    for (UINT32 i = 0; i < count; ++i)
        keys << encoderKey(acts[i], s, &seen);
    // Known refusals are skipped first, and only retried when nothing else
    // accepts, so a transient refusal can never disable the last encoder.
    QList<UINT32> skipped;
    UINT32 chosen = count;
    for (UINT32 i = 0; i < count && !m_mft; ++i) {
        if (wasRefused(keys[i])) {
            skipped << i;
            continue;
        }
        QString reason;
        if (tryActivate(acts[i], s, &reason)) {
            chosen = i;
            break;
        }
        rejected << reason;
        // Only a type refusal is a property of the encoder and settings; an
        // activation, streaming or priming failure may pass next time.
        if (m_refusalDeterministic) {
            setRefused(keys[i], true);
            qInfo().noquote() << QStringLiteral("HwH264Encoder: %1 - not tried again for %2x%3 at %4 fps, %5 bps")
                                     .arg(reason).arg(s.width).arg(s.height).arg(s.fps).arg(s.bitrate);
        }
    }
    for (const UINT32 i : std::as_const(skipped)) {
        if (m_mft)
            break;
        QString reason;
        if (tryActivate(acts[i], s, &reason)) {
            chosen = i;
            setRefused(keys[i], false);
            break;
        }
        rejected << reason;
    }
    for (UINT32 i = 0; i < count; ++i) {
        if (i == chosen)
            m_act = acts[i];   // keep this activation's reference
        else
            acts[i]->Release();
    }
    CoTaskMemFree(acts);
    if (!m_mft) {
        if (why) *why = QStringLiteral("no hardware encoder accepted NV12 %1x%2 (%3)")
                            .arg(s.width).arg(s.height).arg(rejected.join(QStringLiteral("; ")));
        return false;
    }
    return true;
}

bool HwH264Encoder::tryActivate(IMFActivate* act, const Settings& s, QString* why)
{
    WCHAR* name = nullptr;
    UINT32 nameLen = 0;
    QString encName = QStringLiteral("?");
    if (SUCCEEDED(act->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &nameLen)) && name) {
        encName = QString::fromWCharArray(name);
        CoTaskMemFree(name);
    }
    m_refusalDeterministic = false;
    const auto reject = [&](const QString& r, bool deterministic = false) {
        m_refusalDeterministic = deterministic;
        *why = encName + QStringLiteral(": ") + r;
        releaseMft();
        act->ShutdownObject();
        return false;
    };

    HRESULT hr = act->ActivateObject(IID_PPV_ARGS(&m_mft));
    if (FAILED(hr) || !m_mft) { m_mft = nullptr; return reject(hrText("ActivateObject", hr)); }

    IMFAttributes* attrs = nullptr;
    if (FAILED(m_mft->GetAttributes(&attrs)) || !attrs
        || MFGetAttributeUINT32(attrs, MF_TRANSFORM_ASYNC, FALSE) == FALSE) {
        if (attrs) attrs->Release();
        return reject(QStringLiteral("not an asynchronous MFT"), true);
    }
    attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
    // The sink writer sets MF_LOW_LATENCY on the encoder too; keep parity.
    attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
    attrs->Release();

    hr = m_mft->QueryInterface(IID_PPV_ARGS(&m_events));
    if (FAILED(hr)) return reject(hrText("IMFMediaEventGenerator", hr), true);

    // Same encoder hint SegmentRecorder passes to SetInputMediaType.
    setCodecValue(m_mft, CODECAPI_AVEncCommonRealTime, TRUE);

    IMFMediaType* out = videoType(MFVideoFormat_H264, s, true);
    hr = out ? m_mft->SetOutputType(0, out, 0) : E_OUTOFMEMORY;
    if (out) out->Release();
    if (FAILED(hr)) return reject(hrText("SetOutputType", hr), hr != E_OUTOFMEMORY);
    IMFMediaType* in = videoType(MFVideoFormat_NV12, s, false);
    hr = in ? m_mft->SetInputType(0, in, 0) : E_OUTOFMEMORY;
    if (in) in->Release();
    if (FAILED(hr)) return reject(hrText("SetInputType(NV12)", hr), hr != E_OUTOFMEMORY);

    MFT_OUTPUT_STREAM_INFO info{};
    m_mft->GetOutputStreamInfo(0, &info);
    m_providesSamples = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES
                                         | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    m_outBufferSize = info.cbSize ? info.cbSize : s.width * s.height * 2;

    hr = m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (SUCCEEDED(hr)) hr = m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (FAILED(hr)) return reject(hrText("BEGIN_STREAMING", hr));
    m_name = encName;

    // Pass-through writers need the SPS/PPS before the first real frame.
    // Most encoders only publish them with their first output, so encode one
    // black primer frame and discard it.
    if (!refreshOutputType()) {
        const DWORD bytes = s.width * s.height * 3 / 2;
        IMFMediaBuffer* buf = nullptr;
        IMFSample* primer = nullptr;
        if (SUCCEEDED(MFCreateMemoryBuffer(bytes, &buf)) && SUCCEEDED(MFCreateSample(&primer))) {
            BYTE* p = nullptr;
            buf->Lock(&p, nullptr, nullptr);
            memset(p, 16, s.width * s.height);                  // Y black
            memset(p + s.width * s.height, 128, bytes - s.width * s.height);
            buf->Unlock();
            buf->SetCurrentLength(bytes);
            primer->AddBuffer(buf);
            primer->SetSampleTime(0);
            primer->SetSampleDuration(10000000 / LONGLONG(s.fps ? s.fps : 30));
            m_pending.push_back({ primer, true });
            primer = nullptr;
        }
        if (buf) buf->Release();
        if (primer) primer->Release();
        std::vector<IMFSample*> discard;
        const bool drained = drain(discard, 2000);
        for (IMFSample* d : discard) d->Release();
        if (!drained || !refreshOutputType())
            return reject(m_failure.isEmpty() ? QStringLiteral("no sequence header after priming")
                                              : m_failure);
    }
    m_failed = false;
    m_failure.clear();
    return true;
}

bool HwH264Encoder::refreshOutputType()
{
    IMFMediaType* cur = nullptr;
    if (FAILED(m_mft->GetOutputCurrentType(0, &cur)) || !cur)
        return false;
    UINT32 headerLen = 0;
    if (FAILED(cur->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &headerLen)) || headerLen == 0) {
        cur->Release();
        return false;
    }
    if (m_outType) m_outType->Release();
    m_outType = cur;
    return true;
}

void HwH264Encoder::releaseMft()
{
    for (const Pending& p : m_pending) p.sample->Release();
    m_pending.clear();
    for (IMFSample* s : m_output) s->Release();
    m_output.clear();
    if (m_outType) { m_outType->Release(); m_outType = nullptr; }
    if (m_events) { m_events->Release(); m_events = nullptr; }
    if (m_mft) {
        m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        IMFShutdown* shutdown = nullptr;
        if (SUCCEEDED(m_mft->QueryInterface(IID_PPV_ARGS(&shutdown))) && shutdown) {
            shutdown->Shutdown();
            shutdown->Release();
        }
        m_mft->Release();
        m_mft = nullptr;
    }
    m_needInput = 0;
    m_inMft = 0;
    m_draining = false;
}

void HwH264Encoder::close()
{
    releaseMft();
    if (m_act) {
        m_act->ShutdownObject();
        m_act->Release();
        m_act = nullptr;
    }
    m_name.clear();
}

void HwH264Encoder::fail(const QString& why)
{
    if (m_failed)
        return;
    m_failed = true;
    m_failure = why;
}

bool HwH264Encoder::collectOutput()
{
    MFT_OUTPUT_DATA_BUFFER db{};
    IMFSample* own = nullptr;
    if (!m_providesSamples) {
        IMFMediaBuffer* buf = nullptr;
        if (FAILED(MFCreateMemoryBuffer(m_outBufferSize, &buf)) || FAILED(MFCreateSample(&own))) {
            if (buf) buf->Release();
            fail(QStringLiteral("output sample allocation failed"));
            return false;
        }
        own->AddBuffer(buf);
        buf->Release();
        db.pSample = own;
    }
    DWORD status = 0;
    const HRESULT hr = m_mft->ProcessOutput(0, 1, &db, &status);
    if (db.pEvents) db.pEvents->Release();
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        if (own) own->Release();
        IMFMediaType* next = nullptr;
        HRESULT th = m_mft->GetOutputAvailableType(0, 0, &next);
        if (SUCCEEDED(th)) th = m_mft->SetOutputType(0, next, 0);
        if (next) next->Release();
        if (FAILED(th)) { fail(hrText("stream change SetOutputType", th)); return false; }
        // Segment files already carry the first SPS/PPS; a different one
        // later would not match them, so treat that as a failure.
        UINT8 before[256]; UINT32 beforeLen = 0;
        const bool hadHeader = m_outType
            && SUCCEEDED(m_outType->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, before, sizeof(before), &beforeLen));
        if (refreshOutputType() && hadHeader) {
            UINT8 after[256]; UINT32 afterLen = 0;
            if (FAILED(m_outType->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, after, sizeof(after), &afterLen))
                || afterLen != beforeLen || memcmp(before, after, afterLen) != 0) {
                fail(QStringLiteral("encoder changed its sequence header mid-session"));
                return false;
            }
        }
        return true;
    }
    if (FAILED(hr) || !db.pSample) {
        if (own) own->Release();
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
            return true;
        fail(hrText("ProcessOutput", hr));
        return false;
    }
    HwH264Encoder::shiftSampleTime(db.pSample, -kTimeOffset);
    m_output.push_back(db.pSample);   // takes the reference (own or MFT-provided)
    if (m_inMft > 0) --m_inMft;
    return true;
}

// Moves a sample on the timeline. Encoded samples carry a decode timestamp
// next to the presentation time; the MP4 sink orders by it, so both must move
// together or every rebased sample collapses to t=0.
void HwH264Encoder::shiftSampleTime(IMFSample* sample, long long delta)
{
    LONGLONG t = 0;
    if (SUCCEEDED(sample->GetSampleTime(&t)))
        sample->SetSampleTime(t + delta);
    UINT64 dts = 0;
    if (SUCCEEDED(sample->GetUINT64(MFSampleExtension_DecodeTimestamp, &dts)))
        sample->SetUINT64(MFSampleExtension_DecodeTimestamp, UINT64(LONGLONG(dts) + delta));
}

bool HwH264Encoder::submitPending()
{
    while (!m_draining && m_needInput > 0 && !m_pending.empty()) {
        Pending p = m_pending.front();
        if (p.key)
            setCodecValue(m_mft, CODECAPI_AVEncVideoForceKeyFrame, 1);
        const HRESULT hr = m_mft->ProcessInput(0, p.sample, 0);
        if (hr == MF_E_NOTACCEPTING) {   // our credit count was stale; wait for the next request
            m_needInput = 0;
            break;
        }
        m_pending.pop_front();
        p.sample->Release();
        --m_needInput;
        if (FAILED(hr)) { fail(hrText("ProcessInput", hr)); return false; }
        ++m_inMft;
    }
    return true;
}

bool HwH264Encoder::pumpEvents(bool* drainComplete)
{
    if (!submitPending()) return false;
    for (;;) {
        IMFMediaEvent* ev = nullptr;
        const HRESULT hr = m_events->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev);
        if (hr == MF_E_NO_EVENTS_AVAILABLE)
            return true;
        if (FAILED(hr) || !ev) { fail(hrText("GetEvent", hr)); return false; }
        MediaEventType type = MEUnknown;
        HRESULT status = S_OK;
        ev->GetType(&type);
        ev->GetStatus(&status);
        ev->Release();
        if (FAILED(status)) { fail(hrText("encoder event", status)); return false; }
        switch (type) {
        case METransformNeedInput:
            if (!m_draining) ++m_needInput;
            break;
        case METransformHaveOutput:
            if (!collectOutput()) return false;
            break;
        case METransformDrainComplete:
            if (drainComplete) *drainComplete = true;
            break;
        case MEError:
            fail(QStringLiteral("encoder reported MEError"));
            return false;
        default:
            break;
        }
        if (!submitPending()) return false;
    }
}

bool HwH264Encoder::encode(IMFSample* nv12, bool forceKeyframe)
{
    if (!m_mft || m_failed || !nv12)
        return false;
    if (m_injectFailure) {
        m_injectFailure = false;
        fail(QStringLiteral("injected test failure"));
        return false;
    }
    LONGLONG t = 0;
    nv12->GetSampleTime(&t);
    nv12->SetSampleTime(t + kTimeOffset);
    nv12->AddRef();
    if (m_pending.size() >= m_maxPending) {
        // Never block the capture worker: drop the oldest waiting frame. A
        // keyframe request moves to the next frame so the segment still
        // starts with an IDR.
        const bool key = m_pending.front().key;
        m_pending.front().sample->Release();
        m_pending.pop_front();
        ++m_dropped;
        if (key && !m_pending.empty()) m_pending.front().key = true;
        else if (key) forceKeyframe = true;
    }
    m_pending.push_back({ nv12, forceKeyframe });
    return submitPending() && pumpEvents(nullptr);
}

void HwH264Encoder::takeOutput(std::vector<IMFSample*>& out)
{
    if (m_mft && !m_failed)
        pumpEvents(nullptr);
    out.insert(out.end(), m_output.begin(), m_output.end());
    m_output.clear();
}

bool HwH264Encoder::drain(std::vector<IMFSample*>& out, int timeoutMs)
{
    if (!m_mft || m_failed)
        return false;
    if (m_injectDrainFailure) {
        m_injectDrainFailure = false;
        fail(QStringLiteral("injected drain failure"));
        return false;
    }
    QElapsedTimer timer;
    timer.start();
    bool complete = false;
    // Queued frames first: they still need input requests from the encoder.
    while (!m_pending.empty() && !m_failed) {
        if (!pumpEvents(nullptr)) break;
        if (m_pending.empty()) break;
        if (timer.elapsed() > timeoutMs) { fail(QStringLiteral("drain timed out waiting for input requests")); break; }
        QThread::usleep(500);
    }
    if (!m_failed) {
        m_draining = true;
        m_needInput = 0;   // the encoder asks again once the drain is done
        HRESULT hr = m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        if (SUCCEEDED(hr)) hr = m_mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        if (FAILED(hr)) fail(hrText("COMMAND_DRAIN", hr));
        while (!m_failed && !complete) {
            if (!pumpEvents(&complete)) break;
            if (complete) break;
            if (timer.elapsed() > timeoutMs) { fail(QStringLiteral("drain timed out")); break; }
            QThread::usleep(500);
        }
        m_draining = false;
        if (complete) {
            hr = m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
            if (FAILED(hr)) fail(hrText("START_OF_STREAM", hr));
        }
    }
    out.insert(out.end(), m_output.begin(), m_output.end());
    m_output.clear();
    return complete && !m_failed;
}
