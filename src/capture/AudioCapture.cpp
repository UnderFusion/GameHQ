#include "capture/AudioCapture.h"
#include "capture/AudioSourcePolicy.h"
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
#include <vector>

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

// IAgileObject: ActivateAudioInterfaceAsync requires an agile completion
// handler (the Microsoft sample derives from FtmBase). It completes on an MTA
// thread-pool thread; without the marker COM may try to marshal the callback.
// {94EA2B94-E9CC-49E0-C0FF-EE64CA8F5B90}
const GUID kIidAgileObject = {
    0x94ea2b94, 0xe9cc, 0x49e0, { 0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90 } };

// Completion handler for process-loopback activation. Reference counted: the
// async operation holds its own reference, so a late completion after a
// timed-out wait touches a live object, never freed memory.
class AudioActivateHandler : public IActivateAudioInterfaceCompletionHandler
{
public:
    AudioActivateHandler()
        : m_event(CreateEventW(nullptr, TRUE, FALSE, nullptr))
    {
    }

    AudioActivateHandler(const AudioActivateHandler&) = delete;
    AudioActivateHandler& operator=(const AudioActivateHandler&) = delete;

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
            || IsEqualGUID(riid, IID_IActivateAudioInterfaceCompletionHandler)
            || IsEqualGUID(riid, kIidAgileObject)) {
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

    // Only valid after wait() returned true: the completion writes both fields
    // before SetEvent, which orders them for the waiting thread.
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

bool AudioCapture::start(Mode mode, unsigned long targetPid)
{
    if (m_active)
        return true;
    m_deviceInvalidated = false;
    m_failed = false;
    m_consecutiveErrors = 0;
    m_packets = m_frames = m_silentPackets = m_discontinuities = m_readErrors = 0;
    m_peak = 0.0f;
    m_firstPacketQpc100ns = 0;
    m_fifoDroppedFrames = 0;
    m_mode = mode;
    m_targetPid = targetPid;
    m_endpointName.clear();

    // NOTE: AvSetMmThreadCharacteristicsW removed — passing nullptr as the
    // task-index pointer is UB and the suspected trigger of the dev.74/76
    // worker-thread crash on every game-arm. MMCSS boosting is not required
    // for shared-mode loopback capture.

    qInfo().noquote() << QStringLiteral("AudioCapture: start (%1, targetPid=%2)")
        .arg(mode == Mode::Game ? QStringLiteral("game audio") : QStringLiteral("desktop audio"))
        .arg(targetPid);
    m_startQpc100ns = qpc100nsNow();

    // Game audio never falls back to the desktop mix here: the caller decides,
    // and tells the user, if the source has to change.
    const bool started = mode == Mode::Game ? initProcessLoopback(targetPid)
                                            : initDesktopLoopback();
    if (started) {
        m_active = true;
        if (mode == Mode::Game)
            startDrainThread();
        return true;
    }
    release();
    return false;
}

bool AudioCapture::processLoopbackSupported()
{
    return AudioSourcePolicy::isProcessLoopbackBuild(CaptureUtil::windowsBuildNumber());
}

bool AudioCapture::initDesktopLoopback()
{
    // NOTE: MinGW does not support MSVC __try/__except SEH, so we cannot catch a
    // hardware AV here. Granular step logging (each WASAPI call preceded by a
    // qInfo) lets us identify the crashing call from the log after a crash.
    HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                  IID_IMMDeviceEnumerator,
                                  reinterpret_cast<void**>(&m_enumerator));
    if (!ok("CoCreateInstance(MMDeviceEnumerator)", hr))
        return false;
    qInfo() << "AudioCapture: enumerator ready";
    return initDesktopLoopbackInner();
}

namespace {

// One stable recording format for every source: AAC accepts 44.1/48 kHz,
// while render endpoints may use 96/192 kHz or surround layouts, so the
// shared audio engine converts to 48 kHz stereo float32.
WAVEFORMATEXTENSIBLE stereoFloat48k()
{
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
    return want;
}

} // namespace

bool AudioCapture::initDesktopLoopbackInner()
{
    qInfo() << "AudioCapture: initDesktopLoopback — GetDefaultAudioEndpoint";
    HRESULT hr = m_enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &m_device);
    if (!ok("GetDefaultAudioEndpoint(eRender)", hr))
        return false;

    // A game routed elsewhere (Windows per-app output, or a different
    // Multimedia-role default) is not heard by this loopback, so name the
    // endpoint in the log. Game audio mode does not have this limitation.
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
    CoTaskMemFree(mix);

    WAVEFORMATEXTENSIBLE want = stereoFloat48k();
    qInfo() << "AudioCapture: Initialize(LOOPBACK)";
    const DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                      | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    hr = m_client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags,
                              10000000LL, 0, &want.Format, nullptr);
    if (!ok("Initialize(LOOPBACK)", hr))
        return false;
    return startStream(want.Format.nSamplesPerSec, want.Format.nChannels,
                       QStringLiteral("desktop loopback"));
}

// Shared tail of both sources: buffer size, capture service, Start.
bool AudioCapture::startStream(unsigned sampleRate, unsigned channels, const QString& label)
{
    UINT32 bufFrames = 0;
    HRESULT hr = m_client->GetBufferSize(&bufFrames);
    if (FAILED(hr) || bufFrames == 0) {
        // Only sizes the caller's poll buffer; one second is always enough.
        qInfo().nospace() << "AudioCapture: GetBufferSize hr=0x" << Qt::hex << quint32(hr)
                          << Qt::dec << " — using a one-second poll buffer";
        bufFrames = sampleRate;
    }

    m_sampleRate = sampleRate;
    m_channels = channels;
    m_frameSize = unsigned(channels * sizeof(float));
    m_bufFrames = bufFrames;

    hr = m_client->GetService(IID_IAudioCaptureClient,
                              reinterpret_cast<void**>(&m_capture));
    if (!ok("GetService(IAudioCaptureClient)", hr))
        return false;

    hr = m_client->Start();
    if (!ok("Start", hr))
        return false;

    qInfo().noquote() << QStringLiteral("AudioCapture: %1 %2Hz %3ch buffer=%4 frames")
        .arg(label).arg(m_sampleRate).arg(m_channels).arg(m_bufFrames);
    return true;
}

// Application loopback, following Microsoft's ApplicationLoopback sample:
// ActivateAudioInterfaceAsync on VAD\Process_Loopback with an agile handler,
// an explicit format (GetMixFormat is not implemented for this client),
// LOOPBACK | EVENTCALLBACK | AUTOCONVERTPCM and an event handle set before
// Start. The stream follows the process wherever it renders, so the output
// device the game uses does not matter.
bool AudioCapture::initProcessLoopback(unsigned long pid)
{
    if (pid == 0) {
        qWarning() << "AudioCapture: game audio needs the game's process id";
        return false;
    }
    if (!processLoopbackSupported()) {
        qWarning().noquote() << QStringLiteral(
            "AudioCapture: game audio needs Windows build %1 or later (this is %2)")
            .arg(AudioSourcePolicy::kMinimumProcessLoopbackBuild)
            .arg(CaptureUtil::windowsBuildNumber());
        return false;
    }
    m_endpointName = QStringLiteral("game process %1").arg(pid);
    qInfo().noquote() << QStringLiteral("AudioCapture: activating game audio for PID %1").arg(pid);

    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = pid;
    params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

    // The blob is read during the ActivateAudioInterfaceAsync call itself, and
    // this frame outlives the wait below anyway. Never PropVariantClear it:
    // the blob points at the stack.
    PROPVARIANT pv;
    PropVariantInit(&pv);
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof(params);
    pv.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    auto* handler = new AudioActivateHandler();
    IActivateAudioInterfaceAsyncOperation* op = nullptr;
    HRESULT hr = ActivateAudioInterfaceAsync(L"VAD\\Process_Loopback", IID_IAudioClient,
                                             &pv, handler, &op);
    if (!ok("ActivateAudioInterfaceAsync(process loopback)", hr)) {
        if (op) op->Release();
        handler->Release();
        return false;
    }

    // Activation normally completes in a few milliseconds. On a timeout the
    // operation keeps its own handler reference and frees it when it finishes.
    if (!handler->wait(3000)) {
        qWarning() << "AudioCapture: game audio activation timed out";
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

    WAVEFORMATEXTENSIBLE want = stereoFloat48k();
    const DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                      | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                      | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    hr = m_client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags,
                              10000000LL, 0, &want.Format, nullptr);
    if (!ok("Initialize(process loopback)", hr))
        return false;

    // EVENTCALLBACK requires an event before Start. The pump polls on its own
    // cadence, so the auto-reset event is only signalled, never waited on.
    m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_event) {
        qWarning() << "AudioCapture: CreateEvent failed" << GetLastError();
        return false;
    }
    hr = m_client->SetEventHandle(static_cast<HANDLE>(m_event));
    if (!ok("SetEventHandle(process loopback)", hr))
        return false;

    return startStream(want.Format.nSamplesPerSec, want.Format.nChannels,
                       QStringLiteral("game audio PID %1").arg(pid));
}

void AudioCapture::stop()
{
    // Join first: the drain thread owns the capture client while it runs.
    m_active = false;
    stopDrainThread();
    if (m_capture)
        logSummary();
    release();
    std::lock_guard<std::mutex> lock(m_fifoMutex);
    m_fifo.clear();
    m_fifoFrames = 0;
}

// Game audio drains on its own thread: Windows gives an application-loopback
// stream only ~10 ms of buffer, while the capture worker polls every 16 ms and
// can stall far longer behind a busy GPU. The thread wakes on the stream event
// and moves every packet into a FIFO that poll() serves.
void AudioCapture::startDrainThread()
{
    m_stopDrain = false;
    m_drainThread = std::thread([this] {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        // A 100 ms packet ceiling; real packets are ~10 ms.
        std::vector<float> scratch(size_t(std::max(m_bufFrames, m_sampleRate / 10)) * m_channels);
        const unsigned scratchFrames = unsigned(scratch.size() / m_channels);
        while (!m_stopDrain.load() && m_active.load()) {
            WaitForSingleObject(static_cast<HANDLE>(m_event), 100);
            while (!m_stopDrain.load() && m_active.load()) {
                long long timestamp = 0;
                const unsigned got = readPacket(scratch.data(), scratchFrames, &timestamp);
                if (got == 0)
                    break;
                pushFifo(scratch.data(), got, timestamp);
            }
        }
        if (SUCCEEDED(com))
            CoUninitialize();
    });
}

void AudioCapture::stopDrainThread()
{
    if (!m_drainThread.joinable())
        return;
    m_stopDrain = true;
    if (m_event)
        SetEvent(static_cast<HANDLE>(m_event));
    m_drainThread.join();
}

void AudioCapture::pushFifo(const float* samples, unsigned frames, long long timestamp100ns)
{
    // Two seconds covers any worker stall that does not already re-arm the
    // pump; past that, the oldest audio goes first and the summary says so.
    const size_t cap = size_t(m_sampleRate) * 2;
    std::lock_guard<std::mutex> lock(m_fifoMutex);
    m_fifo.push_back({ timestamp100ns, std::vector<float>(samples, samples + size_t(frames) * m_channels) });
    m_fifoFrames += frames;
    while (m_fifoFrames > cap && m_fifo.size() > 1) {
        const size_t dropped = m_fifo.front().samples.size() / m_channels;
        m_fifoFrames -= dropped;
        m_fifoDroppedFrames += dropped;
        m_fifo.pop_front();
    }
}

unsigned AudioCapture::popFifo(float* outBuf, unsigned maxFrames, long long* outTimestamp100ns)
{
    std::lock_guard<std::mutex> lock(m_fifoMutex);
    if (m_fifo.empty())
        return 0;
    Chunk& chunk = m_fifo.front();
    const unsigned frames = unsigned(chunk.samples.size() / m_channels);
    const unsigned copied = std::min(frames, maxFrames);
    const size_t samples = size_t(copied) * m_channels;
    memcpy(outBuf, chunk.samples.data(), samples * sizeof(float));
    if (outTimestamp100ns)
        *outTimestamp100ns = chunk.timestamp100ns;
    if (copied == frames) {
        m_fifo.pop_front();
    } else {
        // A caller buffer smaller than the packet keeps the rest, re-stamped
        // at its own first sample, for the next call.
        chunk.samples.erase(chunk.samples.begin(), chunk.samples.begin() + std::ptrdiff_t(samples));
        chunk.timestamp100ns += (long long)copied * 10000000LL / m_sampleRate;
    }
    m_fifoFrames -= copied;
    return copied;
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
        "discontinuities=%5 readErrors=%6 firstPacket=%7 peak=%8 invalidated=%9 failed=%10 "
        "fifoDropped=%11")
        .arg(m_endpointName).arg(m_packets).arg(seconds, 0, 'f', 1).arg(m_silentPackets)
        .arg(m_discontinuities).arg(m_readErrors).arg(firstPacket, peak)
        .arg(m_deviceInvalidated ? QStringLiteral("yes") : QStringLiteral("no"),
             m_failed ? QStringLiteral("yes") : QStringLiteral("no"))
        .arg(m_fifoDroppedFrames);
}

void AudioCapture::noteReadError(const char* call, long hr)
{
    ++m_readErrors;
    if (isEndpointInvalidation(hr)) {
        m_active = false;
        m_deviceInvalidated = true;
        qWarning().nospace()
            << "AudioCapture: audio stream invalidated during " << call
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
    // After the client: Windows may signal the event until the stream stops.
    if (m_event) { CloseHandle(static_cast<HANDLE>(m_event)); m_event = nullptr; }
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
    if (!outBuf || maxFrames == 0)
        return 0;
    // Game audio: serve what the drain thread already captured, including the
    // tail it read before a failure stopped it.
    if (m_drainThread.joinable())
        return popFifo(outBuf, maxFrames, outTimestamp100ns);
    if (!m_active || !m_capture)
        return 0;
    return readPacket(outBuf, maxFrames, outTimestamp100ns);
}

// One packet straight from WASAPI. Desktop audio calls it from poll() on the
// capture worker; game audio only from the drain thread.
unsigned AudioCapture::readPacket(float* outBuf, unsigned maxFrames, long long* outTimestamp100ns)
{

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
