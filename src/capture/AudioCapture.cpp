#include "capture/AudioCapture.h"
#include "capture/CaptureUtil.h"
#include "capture/wasapi_shims.h"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <avrt.h>
#include <propidl.h>
#include <propsys.h>

#include <QDebug>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

bool ok(const char* what, HRESULT hr)
{
    return CaptureUtil::ok("AudioCapture", what, hr);
}

long long qpc100nsNow()
{
    return CaptureUtil::qpcNow100ns();
}

// PKEY_Device_FriendlyName, spelled out so no devpkey import library is needed.
const PROPERTYKEY kDeviceFriendlyName = {
    { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

// Unexpected read errors tolerated in a row before the stream counts as failed
// (~0.5 s at the 16 ms poll cadence). Endpoint invalidation fails immediately.
constexpr int kMaxConsecutiveReadErrors = 30;

bool isEndpointInvalidation(HRESULT hr)
{
    return hr == AUDCLNT_E_DEVICE_INVALIDATED
        || hr == AUDCLNT_E_RESOURCES_INVALIDATED
        || hr == AUDCLNT_E_SERVICE_NOT_RUNNING;
}

QString endpointId(IMMDevice* device)
{
    LPWSTR id = nullptr;
    if (!device || FAILED(device->GetId(&id)) || !id)
        return {};
    const QString out = QString::fromWCharArray(id);
    CoTaskMemFree(id);
    return out;
}

QString endpointFriendlyName(IMMDevice* device)
{
    IPropertyStore* props = nullptr;
    if (!device || FAILED(device->OpenPropertyStore(STGM_READ, &props)) || !props)
        return {};
    PROPVARIANT value;
    PropVariantInit(&value);
    QString out;
    if (SUCCEEDED(props->GetValue(kDeviceFriendlyName, &value)) && value.vt == VT_LPWSTR
        && value.pwszVal)
        out = QString::fromWCharArray(value.pwszVal);
    PropVariantClear(&value);
    props->Release();
    return out;
}

class AudioActivateHandler : public IActivateAudioInterfaceCompletionHandler
{
public:
    AudioActivateHandler()
        : m_event(CreateEventW(nullptr, TRUE, FALSE, nullptr))
    {
    }

    ~AudioActivateHandler()
    {
        if (m_client)
            m_client->Release();
        if (m_event)
            CloseHandle(m_event);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv)
            return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown)
            || IsEqualGUID(riid, IID_IActivateAudioInterfaceCompletionHandler)) {
            *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ULONG(InterlockedIncrement(&m_refs));
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const LONG refs = InterlockedDecrement(&m_refs);
        if (refs == 0)
            delete this;
        return ULONG(refs);
    }

    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override
    {
        IUnknown* unk = nullptr;
        HRESULT activateHr = E_FAIL;
        m_result = op ? op->GetActivateResult(&activateHr, &unk) : E_POINTER;
        if (SUCCEEDED(m_result))
            m_result = activateHr;
        if (SUCCEEDED(m_result) && unk) {
            m_result = unk->QueryInterface(IID_IAudioClient, reinterpret_cast<void**>(&m_client));
        }
        if (unk)
            unk->Release();
        if (m_event)
            SetEvent(m_event);
        return S_OK;
    }

    bool wait(DWORD ms)
    {
        return m_event && WaitForSingleObject(m_event, ms) == WAIT_OBJECT_0;
    }

    HRESULT result() const { return m_result; }

    IAudioClient* takeClient()
    {
        IAudioClient* out = m_client;
        m_client = nullptr;
        return out;
    }

private:
    LONG m_refs = 1;
    HANDLE m_event = nullptr;
    HRESULT m_result = E_FAIL;
    IAudioClient* m_client = nullptr;
};

} // namespace

AudioCapture::AudioCapture(QObject* parent)
    : QObject(parent)
{
}

AudioCapture::~AudioCapture()
{
    stop();
}

bool AudioCapture::start(unsigned long targetPid)
{
    if (m_active)
        return true;
    m_deviceInvalidated = false;
    m_failed = false;
    m_consecutiveErrors = 0;
    m_packets = m_frames = m_silentPackets = m_discontinuities = m_readErrors = 0;
    m_peak = 0.0f;
    m_firstPacketQpc100ns = 0;

    // NOTE: AvSetMmThreadCharacteristicsW removed — passing nullptr as the
    // task-index pointer is UB and the suspected trigger of the dev.74/76
    // worker-thread crash on every game-arm. MMCSS boosting is not required
    // for shared-mode loopback capture.

    qInfo() << "AudioCapture: start (targetPid=" << targetPid << ")";

    HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                  IID_IMMDeviceEnumerator,
                                  reinterpret_cast<void**>(&m_enumerator));
    if (!ok("CoCreateInstance(MMDeviceEnumerator)", hr))
        return false;

    qInfo() << "AudioCapture: enumerator ready";
    m_startQpc100ns = qpc100nsNow();

    // Process-loopback (ActivateAudioInterfaceAsync) is disabled: it is exotic,
    // untested under MinGW, and crashed the worker thread on every game-arm in
    // dev.74. Desktop loopback (below) is the standard, well-supported path.
    // To re-enable process capture later, validate the async activation surface
    // and completion-handler vtable against a Windows SDK build first.
    if (initDesktopLoopback()) {
        m_active = true;
        return true;
    }

    release();
    return false;
}

bool AudioCapture::initDesktopLoopback()
{
    // NOTE: MinGW does not support MSVC __try/__except SEH, so we cannot catch a
    // hardware AV here. Granular step logging (each WASAPI call preceded by a
    // qInfo) lets us identify the crashing call from the log after a crash.
    return initDesktopLoopbackInner();
}

bool AudioCapture::initDesktopLoopbackInner()
{
    qInfo() << "AudioCapture: initDesktopLoopback — GetDefaultAudioEndpoint";
    HRESULT hr = m_enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &m_device);
    if (!ok("GetDefaultAudioEndpoint(eRender)", hr))
        return false;

    // Games normally render to the default endpoint. A game routed elsewhere
    // (Windows per-app output, or a different Multimedia-role default) is not
    // heard by this loopback, so name the endpoint in the log.
    m_endpointName = endpointFriendlyName(m_device);
    const QString consoleId = endpointId(m_device);
    QString multimediaNote;
    IMMDevice* multimedia = nullptr;
    if (SUCCEEDED(m_enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &multimedia))
        && multimedia) {
        if (endpointId(multimedia) != consoleId)
            multimediaNote = QStringLiteral(" (multimedia default differs: \"%1\")")
                                 .arg(endpointFriendlyName(multimedia));
        multimedia->Release();
    }
    qInfo().noquote() << QStringLiteral("AudioCapture: capturing default output \"%1\"%2")
        .arg(m_endpointName.isEmpty() ? QStringLiteral("unknown") : m_endpointName,
             multimediaNote);

    qInfo() << "AudioCapture: Activate(IAudioClient)";
    hr = m_device->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr,
                            reinterpret_cast<void**>(&m_client));
    if (!ok("Activate(IAudioClient)", hr))
        return false;

    WAVEFORMATEX* mix = nullptr;
    qInfo() << "AudioCapture: GetMixFormat";
    hr = m_client->GetMixFormat(&mix);
    if (!ok("GetMixFormat", hr))
        return false;
    qInfo().nospace() << "AudioCapture: mix format " << mix->nSamplesPerSec
                      << "Hz " << mix->nChannels << "ch " << mix->wBitsPerSample << "b";

    // AAC accepts 44.1/48 kHz, while render endpoints may use 96/192 kHz or
    // surround layouts. Ask the shared audio engine to convert to one stable
    // recording format instead of passing the endpoint format to the encoder.
    WAVEFORMATEXTENSIBLE want{};
    want.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    want.Format.nChannels = 2;
    want.Format.nSamplesPerSec = 48000;
    want.Format.wBitsPerSample = 32;
    want.Format.nBlockAlign = WORD(want.Format.nChannels * sizeof(float));
    want.Format.nAvgBytesPerSec = want.Format.nSamplesPerSec * want.Format.nBlockAlign;
    want.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    want.Samples.wValidBitsPerSample = 32;
    want.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    want.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    CoTaskMemFree(mix);

    qInfo() << "AudioCapture: Initialize(LOOPBACK)";
    const DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                      | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    hr = m_client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags,
                              10000000LL, 0, &want.Format, nullptr);
    if (!ok("Initialize(LOOPBACK)", hr))
        return false;

    UINT32 bufFrames = 0;
    qInfo() << "AudioCapture: GetBufferSize";
    hr = m_client->GetBufferSize(&bufFrames);
    if (!ok("GetBufferSize", hr))
        return false;

    m_sampleRate = want.Format.nSamplesPerSec;
    m_channels = want.Format.nChannels;
    m_frameSize = want.Format.nBlockAlign;
    m_bufFrames = bufFrames;

    qInfo() << "AudioCapture: GetService(IAudioCaptureClient)";
    hr = m_client->GetService(IID_IAudioCaptureClient,
                              reinterpret_cast<void**>(&m_capture));
    if (!ok("GetService(IAudioCaptureClient)", hr))
        return false;

    qInfo() << "AudioCapture: client->Start()";
    hr = m_client->Start();
    if (!ok("Start", hr))
        return false;

    qInfo().noquote() << QStringLiteral("AudioCapture: desktop loopback %1Hz %2ch buffer=%3 frames")
        .arg(m_sampleRate).arg(m_channels).arg(m_bufFrames);
    return true;
}

bool AudioCapture::initProcessLoopback(unsigned long pid)
{
    qInfo().noquote() << QStringLiteral("AudioCapture: trying process-loopback for PID %1").arg(pid);

    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = pid;
    params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT pv{};
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof(params);
    pv.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    auto* handler = new AudioActivateHandler();
    IActivateAudioInterfaceAsyncOperation* op = nullptr;
    HRESULT hr = ActivateAudioInterfaceAsync(L"VAD\\Process_Loopback", IID_IAudioClient,
                                             &pv, handler, &op);
    if (!ok("ActivateAudioInterfaceAsync(process loopback)", hr)) {
        handler->Release();
        return false;
    }

    if (!handler->wait(3000)) {
        qWarning() << "AudioCapture: process-loopback activation timed out";
        if (op) op->Release();
        handler->Release();
        return false;
    }

    hr = handler->result();
    m_client = handler->takeClient();
    if (op) op->Release();
    handler->Release();
    if (!ok("process-loopback activation result", hr) || !m_client)
        return false;

    WAVEFORMATEX* mix = nullptr;
    hr = m_client->GetMixFormat(&mix);
    if (!ok("GetMixFormat(process)", hr))
        return false;

    WAVEFORMATEXTENSIBLE want{};
    want.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    want.Format.nChannels = mix->nChannels;
    want.Format.nSamplesPerSec = mix->nSamplesPerSec;
    want.Format.wBitsPerSample = 32;
    want.Format.nBlockAlign = WORD(want.Format.nChannels * sizeof(float));
    want.Format.nAvgBytesPerSec = want.Format.nSamplesPerSec * want.Format.nBlockAlign;
    want.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    want.Samples.wValidBitsPerSample = 32;
    want.dwChannelMask = mix->nChannels == 2
        ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)
        : SPEAKER_FRONT_CENTER;
    want.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    CoTaskMemFree(mix);

    hr = m_client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                              10000000LL, 0, &want.Format, nullptr);
    if (!ok("Initialize(process loopback)", hr))
        return false;

    UINT32 bufFrames = 0;
    hr = m_client->GetBufferSize(&bufFrames);
    if (!ok("GetBufferSize(process)", hr))
        return false;

    m_sampleRate = want.Format.nSamplesPerSec;
    m_channels = want.Format.nChannels;
    m_frameSize = want.Format.nBlockAlign;
    m_bufFrames = bufFrames;

    hr = m_client->GetService(IID_IAudioCaptureClient,
                              reinterpret_cast<void**>(&m_capture));
    if (!ok("GetService(IAudioCaptureClient/process)", hr))
        return false;

    hr = m_client->Start();
    if (!ok("Start(process)", hr))
        return false;

    qInfo().noquote() << QStringLiteral("AudioCapture: process loopback PID %1 %2Hz %3ch buffer=%4 frames")
        .arg(pid).arg(m_sampleRate).arg(m_channels).arg(m_bufFrames);
    return true;
}

void AudioCapture::stop()
{
    if (m_capture)
        logSummary();
    m_active = false;
    release();
}

// One bounded line per capture session: counters and a peak level only.
void AudioCapture::logSummary() const
{
    const double seconds = m_sampleRate ? double(m_frames) / m_sampleRate : 0.0;
    const QString peak = m_peak > 0.0f
        ? QStringLiteral("%1 dBFS").arg(20.0 * std::log10(double(m_peak)), 0, 'f', 1)
        : QStringLiteral("silent");
    const QString firstPacket = m_firstPacketQpc100ns > 0
        ? QStringLiteral("%1 ms").arg((m_firstPacketQpc100ns - m_startQpc100ns) / 10000)
        : QStringLiteral("never");
    qInfo().noquote() << QStringLiteral(
        "AudioCapture: summary endpoint=\"%1\" packets=%2 audio=%3s silentPackets=%4 "
        "discontinuities=%5 readErrors=%6 firstPacket=%7 peak=%8 invalidated=%9 failed=%10")
        .arg(m_endpointName).arg(m_packets).arg(seconds, 0, 'f', 1).arg(m_silentPackets)
        .arg(m_discontinuities).arg(m_readErrors).arg(firstPacket, peak)
        .arg(m_deviceInvalidated ? QStringLiteral("yes") : QStringLiteral("no"),
             m_failed ? QStringLiteral("yes") : QStringLiteral("no"));
}

void AudioCapture::noteReadError(const char* call, long hr)
{
    ++m_readErrors;
    if (isEndpointInvalidation(hr)) {
        m_active = false;
        m_deviceInvalidated = true;
        qWarning().nospace()
            << "AudioCapture: render endpoint changed during " << call
            << " (hr=0x" << Qt::hex << quint32(hr) << Qt::dec
            << ") — replay capture must re-arm";
        return;
    }
    // Log only the first error of a run; a stuck stream would otherwise write
    // one line per poll tick.
    if (m_consecutiveErrors == 0)
        qWarning().nospace() << "AudioCapture: " << call << " hr=0x" << Qt::hex << quint32(hr);
    if (++m_consecutiveErrors >= kMaxConsecutiveReadErrors) {
        m_active = false;
        m_failed = true;
        qWarning().nospace() << "AudioCapture: " << m_consecutiveErrors
                             << " consecutive read errors (last hr=0x" << Qt::hex
                             << quint32(hr) << ") — audio stream failed";
    }
}

void AudioCapture::release()
{
    if (m_client && m_capture)
        m_client->Stop();
    if (m_capture) { m_capture->Release(); m_capture = nullptr; }
    if (m_client) { m_client->Release(); m_client = nullptr; }
    if (m_device) { m_device->Release(); m_device = nullptr; }
    if (m_enumerator) { m_enumerator->Release(); m_enumerator = nullptr; }
    m_sampleRate = 0;
    m_channels = 0;
    m_frameSize = 0;
    m_bufFrames = 0;
    m_startQpc100ns = 0;
}

unsigned AudioCapture::poll(float* outBuf, unsigned maxFrames, long long* outTimestamp100ns)
{
    if (outTimestamp100ns)
        *outTimestamp100ns = 0;
    if (!m_active || !m_capture || !outBuf || maxFrames == 0)
        return 0;

    UINT32 packetFrames = 0;
    HRESULT hr = m_capture->GetNextPacketSize(&packetFrames);
    if (FAILED(hr)) {
        noteReadError("GetNextPacketSize", hr);
        return 0;
    }
    // No packet is legitimate silence: loopback delivers nothing while no
    // stream renders to the endpoint. It is not an error.
    m_consecutiveErrors = 0;
    if (packetFrames == 0)
        return 0;

    BYTE* data = nullptr;
    DWORD flags = 0;
    UINT64 qpcPosition = 0;
    hr = m_capture->GetBuffer(&data, &packetFrames, &flags, nullptr, &qpcPosition);
    if (hr == AUDCLNT_S_BUFFER_EMPTY || (SUCCEEDED(hr) && packetFrames == 0))
        return 0;
    if (FAILED(hr)) {
        noteReadError("GetBuffer", hr);
        return 0;
    }

    const unsigned copied = std::min<unsigned>(packetFrames, maxFrames);
    if (packetFrames > maxFrames)
        qWarning() << "AudioCapture: packet larger than caller buffer —"
                   << (packetFrames - maxFrames) << "of" << packetFrames
                   << "frames dropped (buffer stall?)";
    const size_t bytes = size_t(copied) * m_frameSize;
    if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
        memset(outBuf, 0, bytes);
        ++m_silentPackets;
    } else {
        memcpy(outBuf, data, bytes);
        const unsigned samples = copied * m_channels;
        for (unsigned i = 0; i < samples; ++i)
            m_peak = std::max(m_peak, std::fabs(outBuf[i]));
    }
    if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)
        ++m_discontinuities;

    // The device QPC position stays exact when a stalled poll drains several
    // packets at once; poll time is only the fallback if Windows omits it.
    const long long now = qpc100nsNow();
    const long long position = static_cast<long long>(qpcPosition);
    const long long packetQpc = (position > 0 && position <= now) ? position : now;
    if (m_firstPacketQpc100ns == 0)
        m_firstPacketQpc100ns = packetQpc;
    ++m_packets;
    m_frames += copied;
    if (outTimestamp100ns)
        *outTimestamp100ns = packetQpc - m_startQpc100ns;

    hr = m_capture->ReleaseBuffer(packetFrames);
    if (FAILED(hr))
        noteReadError("ReleaseBuffer", hr);

    return copied;
}
