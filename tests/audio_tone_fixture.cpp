// Test helper for tst_segmentrecorder's game-audio test: a separate process
// that plays a quiet sine tone, standing in for a game (or a chat/music app).
//
//   audio_tone_fixture <hz> <milliseconds> <default|nondefault> [amplitude]
//
// "nondefault" renders to an active output that is NOT the Windows default,
// falling back to the default when the machine has only one. Prints one line
// "ready endpoint=<id> default=<0|1>" once the tone is playing, so the test
// knows the stream exists before it starts capturing.

#include <windows.h>
#include <initguid.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <ksmedia.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>

namespace {

template <typename T>
void release(T*& p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

IMMDevice* pickEndpoint(IMMDeviceEnumerator* enumerator, bool wantNonDefault, bool* isDefault)
{
    IMMDevice* fallback = nullptr;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &fallback)))
        return nullptr;
    *isDefault = true;
    if (!wantNonDefault)
        return fallback;

    LPWSTR defaultId = nullptr;
    fallback->GetId(&defaultId);
    IMMDeviceCollection* all = nullptr;
    IMMDevice* chosen = nullptr;
    UINT count = 0;
    if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &all))
        && SUCCEEDED(all->GetCount(&count))) {
        for (UINT i = 0; i < count && !chosen; ++i) {
            IMMDevice* device = nullptr;
            LPWSTR id = nullptr;
            if (SUCCEEDED(all->Item(i, &device)) && SUCCEEDED(device->GetId(&id))
                && (!defaultId || wcscmp(id, defaultId) != 0)) {
                chosen = device;
                device = nullptr;
            }
            if (id)
                CoTaskMemFree(id);
            release(device);
        }
    }
    release(all);
    if (defaultId)
        CoTaskMemFree(defaultId);
    if (!chosen)
        return fallback;
    release(fallback);
    *isDefault = false;
    return chosen;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 4)
        return 2;
    const double hz = _wtof(argv[1]);
    const DWORD durationMs = DWORD(_wtoi(argv[2]));
    const bool nonDefault = wcscmp(argv[3], L"nondefault") == 0;
    const float amplitude = argc > 4 ? float(_wtof(argv[4])) : 0.00316f;   // -50 dBFS

    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
        return 3;
    int rc = 4;
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    LPWSTR endpointId = nullptr;
    bool isDefault = true;
    do {
        if (FAILED(CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                    IID_IMMDeviceEnumerator,
                                    reinterpret_cast<void**>(&enumerator))))
            break;
        device = pickEndpoint(enumerator, nonDefault, &isDefault);
        if (!device || FAILED(device->GetId(&endpointId)))
            break;
        if (FAILED(device->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(&client))))
            break;
        WAVEFORMATEXTENSIBLE format{};
        format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        format.Format.nChannels = 2;
        format.Format.nSamplesPerSec = 48000;
        format.Format.wBitsPerSample = 32;
        format.Format.nBlockAlign = 8;
        format.Format.nAvgBytesPerSec = 48000 * 8;
        format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        format.Samples.wValidBitsPerSample = 32;
        format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        format.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                      AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                                          | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                      2000000, 0, &format.Format, nullptr))) {
            rc = 5;
            break;
        }
        UINT32 frames = 0;
        if (FAILED(client->GetBufferSize(&frames))
            || FAILED(client->GetService(IID_IAudioRenderClient,
                                         reinterpret_cast<void**>(&render))))
            break;

        unsigned long long rendered = 0;
        auto feed = [&] {
            UINT32 padding = 0;
            if (FAILED(client->GetCurrentPadding(&padding)))
                return;
            const UINT32 space = frames - padding;
            BYTE* data = nullptr;
            if (space == 0 || FAILED(render->GetBuffer(space, &data)))
                return;
            float* out = reinterpret_cast<float*>(data);
            for (UINT32 i = 0; i < space; ++i, ++rendered) {
                const float v = amplitude * float(std::sin(6.283185307 * hz * double(rendered) / 48000.0));
                out[i * 2] = out[i * 2 + 1] = v;
            }
            render->ReleaseBuffer(space, 0);
        };
        feed();
        if (FAILED(client->Start()))
            break;
        std::printf("ready endpoint=%ls default=%d\n", endpointId, isDefault ? 1 : 0);
        std::fflush(stdout);
        const ULONGLONG until = GetTickCount64() + durationMs;
        while (GetTickCount64() < until) {
            feed();
            Sleep(10);
        }
        client->Stop();
        rc = 0;
    } while (false);

    if (endpointId)
        CoTaskMemFree(endpointId);
    release(render);
    release(client);
    release(device);
    release(enumerator);
    CoUninitialize();
    return rc;
}
