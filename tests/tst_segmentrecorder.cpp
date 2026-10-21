#include "capture/SegmentRecorder.h"
#include "capture/AudioCapture.h"
#include "capture/CaptureUtil.h"
#include "capture/ReplayExporter.h"
#include "capture/ReplayExportTask.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QScopeGuard>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <stdexcept>
#include <thread>
#include <vector>
#include <d3d11.h>
#include <mmdeviceapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <audioclient.h>
#include <wrl/client.h>

class TestSegmentRecorder : public QObject
{
    Q_OBJECT

    static QString segment(const QString& dir, int index, bool stale = false)
    {
        const QString path = dir + QStringLiteral("/%1_clip.mp4").arg(index, 4, 10, QLatin1Char('0'));
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(QByteArray::number(index)) <= 0)
            qFatal("Cannot create segment fixture");
        if (stale && !file.setFileTime(QDateTime::currentDateTime().addSecs(-1200),
                                      QFileDevice::FileModificationTime))
            qFatal("Cannot age segment fixture");
        return path;
    }

    static void restore(SegmentRecorder& recorder, const QString& dir, int keep)
    {
        recorder.m_cacheDir = dir;
        recorder.m_keepSegments = keep;
        // The fixtures are placeholders, not media: these tests are about
        // leases and trimming, so every file counts as a readable segment.
        recorder.m_segmentProbe = [](const QString&) { return true; };
        recorder.restoreRing(); // the same disk restore/prune used by begin()
    }

    struct VideoFixture {
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    };

    static bool makeVideo(VideoFixture& fixture)
    {
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                D3D11_SDK_VERSION, fixture.device.GetAddressOf(), nullptr,
                fixture.context.GetAddressOf())))
            return false;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = 320;
        desc.Height = 180;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.Usage = D3D11_USAGE_DEFAULT;
        const QByteArray pixels(320 * 180 * 4, char(0x80));
        D3D11_SUBRESOURCE_DATA data{};
        data.pSysMem = pixels.constData();
        data.SysMemPitch = 320 * 4;
        return SUCCEEDED(fixture.device->CreateTexture2D(&desc, &data,
                                                          fixture.texture.GetAddressOf()));
    }

    // Decode a clip's AAC track to mono float PCM placed by its timestamp
    // (index = seconds * 48000). covered marks frames the track really holds.
    static bool decodeAudio(const QString& clip, std::vector<float>& mono,
                            std::vector<char>& covered)
    {
        using Microsoft::WRL::ComPtr;
        ComPtr<IMFSourceReader> reader;
        if (FAILED(MFCreateSourceReaderFromURL(reinterpret_cast<LPCWSTR>(clip.utf16()),
                                               nullptr, reader.GetAddressOf())))
            return false;
        reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
        if (FAILED(reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE)))
            return false;
        ComPtr<IMFMediaType> pcm;
        MFCreateMediaType(pcm.GetAddressOf());
        pcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        pcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        pcm->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        pcm->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
        pcm->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        pcm->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
        pcm->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 48000 * 4);
        if (FAILED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr,
                                               pcm.Get())))
            return false;
        for (int packet = 0; packet < 8192; ++packet) {
            ComPtr<IMFSample> sample;
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            if (FAILED(reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr,
                                          &flags, &timestamp, sample.GetAddressOf())))
                return false;
            if (sample) {
                ComPtr<IMFMediaBuffer> buffer;
                if (FAILED(sample->ConvertToContiguousBuffer(buffer.GetAddressOf())))
                    return false;
                BYTE* bytes = nullptr;
                DWORD length = 0;
                if (FAILED(buffer->Lock(&bytes, nullptr, &length)))
                    return false;
                const short* values = reinterpret_cast<const short*>(bytes);
                const long long first = timestamp * 48000 / 10000000LL;
                for (unsigned i = 0; i < length / 4; ++i) {
                    const long long index = first + i;
                    if (index < 0)
                        continue;
                    if (size_t(index) >= mono.size()) {
                        mono.resize(size_t(index) + 48000, 0.0f);
                        covered.resize(mono.size(), 0);
                    }
                    mono[size_t(index)] = (values[i * 2] + values[i * 2 + 1]) / 65536.0f;
                    covered[size_t(index)] = 1;
                }
                buffer->Unlock();
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
                return true;
        }
        return false;
    }

    static double rms(const std::vector<float>& x, size_t from, size_t to)
    {
        double sum = 0.0;
        to = std::min(to, x.size());
        for (size_t i = from; i < to; ++i)
            sum += double(x[i]) * x[i];
        return to > from ? std::sqrt(sum / double(to - from)) : 0.0;
    }

    // Amplitude of one frequency (Goertzel), so a controlled tone can be told
    // apart from whatever else happens to be playing.
    static double toneAmplitude(const std::vector<float>& x, size_t from, size_t to, double hz)
    {
        to = std::min(to, x.size());
        if (to <= from)
            return 0.0;
        const double w = 6.28318530717958647692 * hz / 48000.0;
        const double coeff = 2.0 * std::cos(w);
        double s1 = 0.0, s2 = 0.0;
        for (size_t i = from; i < to; ++i) {
            const double s0 = x[i] + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
        return 2.0 * std::sqrt(std::max(power, 0.0)) / double(to - from);
    }

private slots:
    void desktopAudioUsesEncodableFormat()
    {
        using Microsoft::WRL::ComPtr;
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        QVERIFY(SUCCEEDED(com));
        const auto apartment = qScopeGuard([] { CoUninitialize(); });
        ComPtr<IMMDeviceEnumerator> enumerator;
        QVERIFY(SUCCEEDED(CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
            IID_IMMDeviceEnumerator, reinterpret_cast<void**>(enumerator.GetAddressOf()))));
        ComPtr<IMMDevice> endpoint;
        const HRESULT endpointResult = enumerator->GetDefaultAudioEndpoint(
            eRender, eConsole, endpoint.GetAddressOf());
        if (endpointResult == HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
            QSKIP("No default render endpoint is available on this machine");
        QVERIFY(SUCCEEDED(endpointResult));

        AudioCapture audio;
        QVERIFY(audio.start());
        QVERIFY(audio.isActive());
        QCOMPARE(audio.sampleRate(), 48000u);
        QCOMPARE(audio.channels(), 2u);
        QVERIFY(audio.bufferFrames() > 0);
        // Poll without playing a tone or opening a window. A silent desktop
        // can legitimately produce no packets; startup/format must still work.
        std::vector<float> samples(size_t(audio.bufferFrames()) * audio.channels());
        for (int i = 0; i < 5; ++i) {
            QTest::qSleep(20);
            long long timestamp = 0;
            const unsigned frames = audio.poll(samples.data(), audio.bufferFrames(), &timestamp);
            QVERIFY(frames <= audio.bufferFrames());
            QVERIFY(!audio.deviceInvalidated());
            QVERIFY(audio.isActive());
            for (unsigned sample = 0; sample < frames * 2; ++sample)
                QVERIFY(std::isfinite(samples[sample]));
        }
        audio.stop();
        QVERIFY(!audio.isActive());
    }

    void savedReplayKeepsAudibleAudioAcrossRollsAndSnapshots()
    {
        using Microsoft::WRL::ComPtr;
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        QVERIFY(SUCCEEDED(com));
        const auto apartment = qScopeGuard([] { CoUninitialize(); });
        QTemporaryDir dir(QDir::currentPath() + "/segment-audio-XXXXXX");
        QVERIFY(dir.isValid());
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        QVERIFY(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, device.GetAddressOf(), nullptr,
            context.GetAddressOf())));
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = 320;
        desc.Height = 180;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.Usage = D3D11_USAGE_DEFAULT;
        const QByteArray pixels(320 * 180 * 4, char(0x80));
        D3D11_SUBRESOURCE_DATA data{};
        data.pSysMem = pixels.constData();
        data.SysMemPitch = 320 * 4;
        ComPtr<ID3D11Texture2D> texture;
        QVERIFY(SUCCEEDED(device->CreateTexture2D(&desc, &data, texture.GetAddressOf())));
        SegmentRecorder recorder;
        QVERIFY(recorder.begin(320, 180, 320, 180, 30, 2, 1, 10,
                               dir.path(), device.Get(), context.Get(), 48000, 2));
        const auto finish = qScopeGuard([&] {
            recorder.discardCurrentSegment();
            recorder.end();
        });
        QVERIFY(recorder.hasAudio()); // a video-only fallback must fail this test

        constexpr unsigned audioFrames = 48000 / 30;
        constexpr double tau = 6.28318530717958647692;
        std::array<float, audioFrames * 2> audio{};
        SegmentLease firstSave;
        for (int frame = 0; frame < 105; ++frame) {
            const qint64 timestamp = 10000000LL + frame * 10000000LL / 30;
            recorder.writeFrame(texture.Get(), device.Get(), context.Get(), timestamp);
            for (unsigned i = 0; i < audioFrames; ++i) {
                const double seconds = (frame * audioFrames + i) / 48000.0;
                audio[i * 2] = float(0.4 * std::sin(tau * 440.0 * seconds));
                audio[i * 2 + 1] = float(0.25 * std::sin(tau * 660.0 * seconds));
            }
            recorder.writeAudio(audio.data(), audioFrames, timestamp);
            if (frame == 44) {
                firstSave = recorder.snapshotForSave();
                QCOMPARE(firstSave.paths().size(), 2);
                QVERIFY(recorder.isActive());
                QVERIFY(recorder.hasAudio());
            }
        }
        const SegmentLease saved = recorder.snapshotForSave();
        QVERIFY(saved.paths().size() >= 4); // timed rolls and the explicit snapshot
        const QString clip = dir.path() + "/saved-audio.mp4";
        QVERIFY(ReplayExporter::concat(saved.paths(), clip, QStringLiteral("audio-regression"),
                                       ReplayExporter::Audio::Required));

        ComPtr<IMFSourceReader> reader;
        QVERIFY(SUCCEEDED(MFCreateSourceReaderFromURL(
            reinterpret_cast<LPCWSTR>(clip.utf16()), nullptr, reader.GetAddressOf())));
        QVERIFY(SUCCEEDED(reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE)));
        QVERIFY(SUCCEEDED(reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE)));
        ComPtr<IMFMediaType> native;
        QVERIFY(SUCCEEDED(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM,
            0, native.GetAddressOf())));
        GUID subtype{};
        QVERIFY(SUCCEEDED(native->GetGUID(MF_MT_SUBTYPE, &subtype)));
        QVERIFY(IsEqualGUID(subtype, MFAudioFormat_AAC));
        ComPtr<IMFMediaType> pcm;
        QVERIFY(SUCCEEDED(MFCreateMediaType(pcm.GetAddressOf())));
        pcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        pcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        pcm->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        pcm->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
        pcm->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        pcm->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
        pcm->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 48000 * 4);
        QVERIFY(SUCCEEDED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM,
            nullptr, pcm.Get())));

        // Decode the exported file: an AAC header or packet count alone could
        // pass with silence. Each half-second must contain energy in both
        // channels, including the audio after every roll and snapshot.
        std::array<std::array<double, 2>, 7> energy{};
        std::array<unsigned, 7> counts{};
        unsigned decodedFrames = 0;
        bool ended = false;
        for (int packet = 0; packet < 2048; ++packet) {
            ComPtr<IMFSample> sample;
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            QVERIFY(SUCCEEDED(reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                0, nullptr, &flags, &timestamp, sample.GetAddressOf())));
            if (sample) {
                ComPtr<IMFMediaBuffer> buffer;
                QVERIFY(SUCCEEDED(sample->ConvertToContiguousBuffer(buffer.GetAddressOf())));
                BYTE* bytes = nullptr;
                DWORD length = 0;
                QVERIFY(SUCCEEDED(buffer->Lock(&bytes, nullptr, &length)));
                const auto unlock = qScopeGuard([&] { buffer->Unlock(); });
                QCOMPARE(length % 4, 0u);
                const short* values = reinterpret_cast<const short*>(bytes);
                const unsigned frames = length / 4;
                decodedFrames += frames;
                for (unsigned i = 0; i < frames; ++i) {
                    const LONGLONG time = timestamp + i * 10000000LL / 48000;
                    if (time < 0 || time >= 35000000LL)
                        continue;
                    const size_t bin = size_t(time / 5000000LL);
                    ++counts[bin];
                    for (unsigned channel = 0; channel < 2; ++channel) {
                        const double value = values[i * 2 + channel] / 32768.0;
                        energy[bin][channel] += value * value;
                    }
                }
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                ended = true;
                break;
            }
        }
        QVERIFY(ended);
        QVERIFY(decodedFrames >= 3 * 48000u);
        for (size_t bin = 0; bin < counts.size(); ++bin) {
            QVERIFY2(counts[bin] >= 12000u, "A half-second window lost most of its audio");
            for (unsigned channel = 0; channel < 2; ++channel)
                QVERIFY2(std::sqrt(energy[bin][channel] / counts[bin]) > 0.05,
                         "Saved replay contains a silent channel or segment");
        }

        // A stale video-only segment must not make the exporter silently omit
        // the real AAC stream present in subsequent segments.
        SegmentRecorder videoOnly;
        QVERIFY(videoOnly.begin(320, 180, 320, 180, 30, 2, 5, 10,
                                dir.path() + "/video-only", device.Get(), context.Get()));
        const auto finishVideoOnly = qScopeGuard([&] {
            videoOnly.discardCurrentSegment();
            videoOnly.end();
        });
        for (int frame = 0; frame < 30; ++frame)
            videoOnly.writeFrame(texture.Get(), device.Get(), context.Get(),
                                 10000000LL + frame * 10000000LL / 30);
        const SegmentLease silent = videoOnly.snapshotForSave();
        QCOMPARE(silent.paths().size(), 1);
        const QStringList incompatible{silent.paths().first(), saved.paths().first()};
        QVERIFY(!ReplayExporter::concat(incompatible, dir.path() + "/mixed-audio.mp4",
                                        QStringLiteral("mixed-audio-regression")));
        // The reverse order would otherwise export audio that stops part-way.
        const QStringList audioThenVideoOnly{saved.paths().first(), silent.paths().first()};
        QVERIFY(!ReplayExporter::concat(audioThenVideoOnly, dir.path() + "/mixed-audio-2.mp4",
                                        QStringLiteral("mixed-audio-reverse-regression")));
        // Video-only stays valid when audio is off, but never when it is required.
        QVERIFY(ReplayExporter::concat(silent.paths(), dir.path() + "/video-only.mp4",
                                       QStringLiteral("video-only-allowed")));
        QVERIFY(!ReplayExporter::concat(silent.paths(), dir.path() + "/video-only-required.mp4",
                                        QStringLiteral("video-only-required"),
                                        ReplayExporter::Audio::Required));
    }

    // Loopback sends no packets while nothing plays. The recorder must fill
    // that stretch with silence: the AAC track stays continuous across every
    // segment and audio after the quiet part stays in sync with the video.
    void silentStretchKeepsAudioContinuousAndInSync()
    {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        QVERIFY(SUCCEEDED(com));
        const auto apartment = qScopeGuard([] { CoUninitialize(); });
        QTemporaryDir dir(QDir::currentPath() + "/segment-silence-XXXXXX");
        QVERIFY(dir.isValid());
        VideoFixture video;
        QVERIFY(makeVideo(video));
        SegmentRecorder recorder;
        QVERIFY(recorder.begin(320, 180, 320, 180, 30, 2, 1, 10, dir.path(),
                               video.device.Get(), video.context.Get(), 48000, 2));
        const auto finish = qScopeGuard([&] {
            recorder.discardCurrentSegment();
            recorder.end();
        });

        constexpr unsigned audioFrames = 48000 / 30;
        std::array<float, audioFrames * 2> tone{};
        for (int frame = 0; frame < 120; ++frame) {
            const qint64 timestamp = 10000000LL + frame * 10000000LL / 30;
            recorder.writeFrame(video.texture.Get(), video.device.Get(), video.context.Get(),
                                timestamp);
            // Tone for 0.0-0.5 s and 3.0-4.0 s; nothing at all in between,
            // exactly like an idle loopback endpoint.
            if (frame < 15 || frame >= 90) {
                for (unsigned i = 0; i < audioFrames; ++i) {
                    const double t = (frame * audioFrames + i) / 48000.0;
                    tone[i * 2] = tone[i * 2 + 1] = float(0.4 * std::sin(6.283185307 * 1000.0 * t));
                }
                recorder.writeAudio(tone.data(), audioFrames, timestamp);
            } else {
                recorder.padAudioSilence(timestamp - 2000000);   // FramePump's headroom
            }
        }
        const SegmentLease saved = recorder.snapshotForSave();
        QVERIFY(saved.paths().size() >= 4);
        const QString clip = dir.path() + "/silence.mp4";
        QVERIFY(ReplayExporter::concat(saved.paths(), clip, QStringLiteral("silence-regression"),
                                       ReplayExporter::Audio::Required));

        std::vector<float> mono;
        std::vector<char> covered;
        QVERIFY(decodeAudio(clip, mono, covered));
        // Continuous: no hole longer than one AAC frame anywhere up to 3.9 s.
        size_t hole = 0, longestHole = 0;
        for (size_t i = 2048; i < std::min<size_t>(covered.size(), 187200); ++i) {
            hole = covered[i] ? 0 : hole + 1;
            longestHole = std::max(longestHole, hole);
        }
        QVERIFY2(covered.size() >= 187200, "Audio track ends before the video");
        QVERIFY2(longestHole <= 2048, "Audio track has a hole across the silent stretch");
        // In sync: the tone is where the video says it is, silence in between.
        QVERIFY(toneAmplitude(mono, 2400, 21600, 1000.0) > 0.2);
        QVERIFY(rms(mono, 48000, 132000) < 0.01);
        QVERIFY(toneAmplitude(mono, 148800, 187200, 1000.0) > 0.2);
        QVERIFY(rms(mono, 132000, 141600) < 0.05);   // no early tone before 3.0 s
    }

    // End-to-end with the real Windows audio engine: a quiet controlled tone
    // rendered to the default output -> WASAPI loopback (AudioCapture) ->
    // SegmentRecorder -> ReplayExporter -> decoded MP4. Opt-in because it
    // plays a faint 2.5 s tone: set GAMEHQ_AUDIO_E2E=1.
    void realLoopbackToneReachesSavedClip()
    {
        if (qEnvironmentVariableIntValue("GAMEHQ_AUDIO_E2E") != 1)
            QSKIP("Plays a quiet tone on the default output; set GAMEHQ_AUDIO_E2E=1");
        using Microsoft::WRL::ComPtr;
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        QVERIFY(SUCCEEDED(com));
        const auto apartment = qScopeGuard([] { CoUninitialize(); });

        ComPtr<IMMDeviceEnumerator> enumerator;
        QVERIFY(SUCCEEDED(CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
            IID_IMMDeviceEnumerator, reinterpret_cast<void**>(enumerator.GetAddressOf()))));
        ComPtr<IMMDevice> endpoint;
        if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, endpoint.GetAddressOf())))
            QSKIP("No default render endpoint is available on this machine");
        ComPtr<IAudioClient> renderClient;
        QVERIFY(SUCCEEDED(endpoint->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(renderClient.GetAddressOf()))));
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
        format.SubFormat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}}; // IEEE float
        const HRESULT renderInit = renderClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
            2000000, 0, &format.Format, nullptr);
        if (renderInit == AUDCLNT_E_DEVICE_IN_USE)
            QSKIP("The default output is held in exclusive mode");
        QVERIFY(SUCCEEDED(renderInit));
        UINT32 renderFrames = 0;
        QVERIFY(SUCCEEDED(renderClient->GetBufferSize(&renderFrames)));
        ComPtr<IAudioRenderClient> render;
        QVERIFY(SUCCEEDED(renderClient->GetService(IID_IAudioRenderClient,
            reinterpret_cast<void**>(render.GetAddressOf()))));

        AudioCapture audio;
        QVERIFY(audio.start());
        QTemporaryDir dir(QDir::currentPath() + "/segment-loopback-XXXXXX");
        QVERIFY(dir.isValid());
        VideoFixture video;
        QVERIFY(makeVideo(video));
        SegmentRecorder recorder;
        QVERIFY(recorder.begin(320, 180, 320, 180, 30, 2, 1, 10, dir.path(),
                               video.device.Get(), video.context.Get(),
                               audio.sampleRate(), audio.channels()));
        const auto finish = qScopeGuard([&] {
            recorder.discardCurrentSegment();
            recorder.end();
            audio.stop();
        });
        QVERIFY(recorder.hasAudio());

        // -50 dBFS, 1 kHz: faint, yet far above the AAC noise floor.
        constexpr float amplitude = 0.00316f;
        constexpr double toneHz = 1000.0;
        unsigned long long rendered = 0;
        const auto feedTone = [&] {
            UINT32 padding = 0;
            if (FAILED(renderClient->GetCurrentPadding(&padding)))
                return;
            const UINT32 space = renderFrames - padding;
            BYTE* data = nullptr;
            if (space == 0 || FAILED(render->GetBuffer(space, &data)))
                return;
            float* out = reinterpret_cast<float*>(data);
            for (UINT32 i = 0; i < space; ++i, ++rendered) {
                const float v = amplitude * float(std::sin(6.283185307 * toneHz * rendered / 48000.0));
                out[i * 2] = out[i * 2 + 1] = v;
            }
            render->ReleaseBuffer(space, 0);
        };
        feedTone();
        QVERIFY(SUCCEEDED(renderClient->Start()));
        const auto stopRender = qScopeGuard([&] { renderClient->Stop(); });

        std::vector<float> buffer(size_t(audio.bufferFrames()) * audio.channels());
        const long long epoch = CaptureUtil::qpcNow100ns();
        QElapsedTimer clock;
        clock.start();
        int frame = 0;
        while (clock.elapsed() < 2500) {
            feedTone();
            const long long now = CaptureUtil::qpcNow100ns() - epoch;
            recorder.writeFrame(video.texture.Get(), video.device.Get(), video.context.Get(), now);
            for (;;) {
                long long t100 = 0;
                const unsigned got = audio.poll(buffer.data(), audio.bufferFrames(), &t100);
                if (got == 0)
                    break;
                recorder.writeAudio(buffer.data(), got, audio.startQpc100ns() + t100 - epoch);
            }
            QVERIFY(!audio.failed());
            QVERIFY(!audio.deviceInvalidated());
            recorder.padAudioSilence(now - 2000000);
            ++frame;
            QTest::qSleep(16);
        }
        QVERIFY(frame > 60);
        const SegmentLease saved = recorder.snapshotForSave();
        QVERIFY(!saved.paths().isEmpty());
        const QString clip = dir.path() + "/loopback.mp4";
        QVERIFY(ReplayExporter::concat(saved.paths(), clip, QStringLiteral("loopback-e2e"),
                                       ReplayExporter::Audio::Required));

        std::vector<float> mono;
        std::vector<char> covered;
        QVERIFY(decodeAudio(clip, mono, covered));
        // Skip the start-up half second; measure the steady tone.
        const size_t from = 24000, to = std::min<size_t>(mono.size(), 96000);
        QVERIFY2(to > from + 24000, "Saved clip holds too little audio");
        const double level = rms(mono, from, to);
        // Median over 100 ms windows (whole tone cycles): segment joins may
        // shift the phase, which a single long window would average away.
        const auto medianTone = [&](double hz) {
            std::vector<double> levels;
            for (size_t at = from; at + 4800 <= to; at += 4800)
                levels.push_back(toneAmplitude(mono, at, at + 4800, hz));
            std::sort(levels.begin(), levels.end());
            return levels.empty() ? 0.0 : levels[levels.size() / 2];
        };
        const double toneLevel = medianTone(toneHz);
        const double offTone = medianTone(1500.0);
        qInfo().noquote() << QStringLiteral("loopback e2e: rms=%1 tone=%2 offTone=%3 (rendered %4)")
            .arg(level, 0, 'g', 3).arg(toneLevel, 0, 'g', 3).arg(offTone, 0, 'g', 3)
            .arg(amplitude, 0, 'g', 3);
        QVERIFY2(level > 1e-4, "Saved clip audio is silent although a tone was playing");
        QVERIFY2(toneLevel > amplitude * 0.5, "Controlled tone is missing from the saved clip");
        QVERIFY2(toneLevel > offTone * 20.0, "Saved clip audio is not the controlled tone");
    }

    // Game audio (application loopback) activation, the path that crashed the
    // capture worker in dev.74. Targets this silent test process, so nothing
    // plays; repeated start/stop covers activation, event and COM lifetime.
    void gameAudioStartsAndStopsCleanly()
    {
        if (!AudioCapture::processLoopbackSupported())
            QSKIP("Per-process audio capture needs Windows build 20348 or later");
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        QVERIFY(SUCCEEDED(com));
        const auto apartment = qScopeGuard([] { CoUninitialize(); });
        {
            AudioCapture noTarget;
            QVERIFY(!noTarget.start(AudioCapture::Mode::Game, 0));
            QVERIFY(!noTarget.isActive());
        }
        for (int round = 0; round < 3; ++round) {
            AudioCapture audio;
            QVERIFY(audio.start(AudioCapture::Mode::Game, GetCurrentProcessId()));
            QVERIFY(audio.isActive());
            QCOMPARE(audio.mode(), AudioCapture::Mode::Game);
            QCOMPARE(audio.sampleRate(), 48000u);
            QCOMPARE(audio.channels(), 2u);
            QVERIFY(audio.bufferFrames() > 0);
            std::vector<float> samples(size_t(audio.bufferFrames()) * audio.channels());
            for (int i = 0; i < 5; ++i) {
                QTest::qSleep(20);
                long long timestamp = 0;
                const unsigned frames = audio.poll(samples.data(), audio.bufferFrames(), &timestamp);
                QVERIFY(frames <= audio.bufferFrames());
                QVERIFY(!audio.failed());
                QVERIFY(!audio.deviceInvalidated());
                for (unsigned sample = 0; sample < frames * 2; ++sample) {
                    QVERIFY(std::isfinite(samples[sample]));
                    QCOMPARE(samples[sample], 0.0f);   // this process renders nothing
                }
            }
            if (round == 2)
                continue;   // the last round is released by the destructor alone
            audio.stop();
            QVERIFY(!audio.isActive());
        }
    }

    // End-to-end game audio with the real Windows audio engine: a "game"
    // process plays 1 kHz on an output that is NOT the Windows default (when
    // the machine has one), an unrelated "chat" process plays 1.5 kHz on the
    // default output. Capturing the game's PID must record the game's tone
    // into the saved, decoded MP4 and nothing of the other app. Opt-in
    // because it plays two faint tones: set GAMEHQ_AUDIO_E2E=1.
    void realGameAudioRecordsOnlyTheGame()
    {
        if (qEnvironmentVariableIntValue("GAMEHQ_AUDIO_E2E") != 1)
            QSKIP("Plays two quiet tones; set GAMEHQ_AUDIO_E2E=1");
        if (!AudioCapture::processLoopbackSupported())
            QSKIP("Per-process audio capture needs Windows build 20348 or later");
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        QVERIFY(SUCCEEDED(com));
        const auto apartment = qScopeGuard([] { CoUninitialize(); });

        constexpr double gameHz = 1000.0;
        constexpr double otherHz = 1500.0;
        constexpr float amplitude = 0.00316f;   // -50 dBFS
        const auto launch = [](QProcess& process, double hz, const char* endpoint) {
            process.setProgram(QStringLiteral(AUDIO_TONE_FIXTURE));
            process.setArguments({ QString::number(hz), QStringLiteral("8000"),
                                   QString::fromLatin1(endpoint),
                                   QString::number(double(amplitude)) });
            process.start();
            if (!process.waitForStarted(5000) || !process.waitForReadyRead(5000))
                return QString();
            return QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();
        };
        QProcess game, other;
        const auto stopFixtures = qScopeGuard([&] {
            for (QProcess* p : { &game, &other }) {
                p->kill();
                p->waitForFinished(3000);
            }
        });
        const QString gameReady = launch(game, gameHz, "nondefault");
        if (gameReady.isEmpty() && game.waitForFinished(2000) && game.exitCode() == 5)
            QSKIP("The chosen output is held in exclusive mode");
        QVERIFY2(gameReady.startsWith(QStringLiteral("ready")), qPrintable(gameReady));
        const QString otherReady = launch(other, otherHz, "default");
        QVERIFY2(otherReady.startsWith(QStringLiteral("ready")), qPrintable(otherReady));
        const bool gameOnOtherDevice = gameReady.endsWith(QStringLiteral("default=0"));
        qInfo().noquote() << "game audio e2e: game" << gameReady << "| other" << otherReady;

        AudioCapture audio;
        QVERIFY(audio.start(AudioCapture::Mode::Game, DWORD(game.processId())));
        QTemporaryDir dir(QDir::currentPath() + "/segment-gameaudio-XXXXXX");
        QVERIFY(dir.isValid());
        VideoFixture video;
        QVERIFY(makeVideo(video));
        SegmentRecorder recorder;
        QVERIFY(recorder.begin(320, 180, 320, 180, 30, 2, 1, 10, dir.path(),
                               video.device.Get(), video.context.Get(),
                               audio.sampleRate(), audio.channels()));
        const auto finish = qScopeGuard([&] {
            recorder.discardCurrentSegment();
            recorder.end();
            audio.stop();
        });
        QVERIFY(recorder.hasAudio());

        std::vector<float> buffer(size_t(audio.bufferFrames()) * audio.channels());
        const long long epoch = CaptureUtil::qpcNow100ns();
        QElapsedTimer clock;
        clock.start();
        int frame = 0;
        bool workerStalled = false;
        while (clock.elapsed() < 3000) {
            // One 400 ms busy-worker gap mid-clip: game audio must neither lose
            // samples nor fall out of sync with the held video frame.
            if (!workerStalled && clock.elapsed() > 1500) {
                workerStalled = true;
                QTest::qSleep(400);
            }
            // FramePumpWorker::poll order: drain audio, then the next frame,
            // so a stall's audio lands before that frame can roll the segment.
            for (;;) {
                long long t100 = 0;
                const unsigned got = audio.poll(buffer.data(), audio.bufferFrames(), &t100);
                if (got == 0)
                    break;
                recorder.writeAudio(buffer.data(), got, audio.startQpc100ns() + t100 - epoch);
            }
            const long long now = CaptureUtil::qpcNow100ns() - epoch;
            recorder.writeFrame(video.texture.Get(), video.device.Get(), video.context.Get(), now);
            QVERIFY(!audio.failed());
            QVERIFY(!audio.deviceInvalidated());
            recorder.padAudioSilence(now - 2000000);
            ++frame;
            QTest::qSleep(16);
        }
        QVERIFY(frame > 60);
        const SegmentLease saved = recorder.snapshotForSave();
        QVERIFY(!saved.paths().isEmpty());
        const QString clip = dir.path() + "/gameaudio.mp4";
        QVERIFY(ReplayExporter::concat(saved.paths(), clip, QStringLiteral("gameaudio-e2e"),
                                       ReplayExporter::Audio::Required));

        std::vector<float> mono;
        std::vector<char> covered;
        QVERIFY(decodeAudio(clip, mono, covered));
        const size_t from = 24000, to = std::min<size_t>(mono.size(), 120000);
        QVERIFY2(to > from + 24000, "Saved clip holds too little audio");
        size_t hole = 0, longestHole = 0, holeEnd = 0;
        for (size_t i = from; i < to && i < covered.size(); ++i) {
            hole = covered[i] ? 0 : hole + 1;
            if (hole > longestHole) {
                longestHole = hole;
                holeEnd = i;
            }
        }
        double weakest = 1.0;   // quietest 100 ms window: a dropout shows here
        for (size_t at = from; at + 4800 <= to; at += 4800)
            weakest = std::min(weakest, toneAmplitude(mono, at, at + 4800, gameHz));
        const auto medianTone = [&](double hz) {
            std::vector<double> levels;
            for (size_t at = from; at + 4800 <= to; at += 4800)
                levels.push_back(toneAmplitude(mono, at, at + 4800, hz));
            std::sort(levels.begin(), levels.end());
            return levels.empty() ? 0.0 : levels[levels.size() / 2];
        };
        const double gameLevel = medianTone(gameHz);
        const double otherLevel = medianTone(otherHz);
        const double offTone = medianTone(2300.0);
        qInfo().noquote() << QStringLiteral(
            "game audio e2e: game=%1 weakest=%2 other=%3 offTone=%4 longestHole=%5 "
            "(ends at sample %8 of %9) gameOnOtherDevice=%6 (rendered %7)")
            .arg(gameLevel, 0, 'g', 3).arg(weakest, 0, 'g', 3).arg(otherLevel, 0, 'g', 3)
            .arg(offTone, 0, 'g', 3).arg(longestHole)
            .arg(gameOnOtherDevice ? "yes" : "no").arg(double(amplitude), 0, 'g', 3)
            .arg(holeEnd).arg(mono.size());
        QVERIFY2(longestHole <= 2048, "Game audio track has a hole");
        QVERIFY2(weakest > amplitude * 0.2, "Game audio drops out in the saved clip");
        QVERIFY2(gameLevel > amplitude * 0.5, "The game's tone is missing from the saved clip");
        QVERIFY2(gameLevel > otherLevel * 20.0, "Another app's audio leaked into game audio");
        QVERIFY2(gameLevel > offTone * 20.0, "Saved clip audio is not the game's tone");
        if (!gameOnOtherDevice)
            qWarning("Only one active output: the non-default-device case was not exercised");

        // A busy capture worker: one 400 ms gap between polls, far beyond the
        // ~10 ms Windows buffers for an application-loopback stream. The
        // drain thread must keep every packet, back to back.
        AudioCapture stalled;
        QVERIFY(stalled.start(AudioCapture::Mode::Game, DWORD(game.processId())));
        long long first = -1, expectedNext = -1, worstJump = 0;
        unsigned long long frames = 0;
        QElapsedTimer span;
        span.start();
        const auto drain = [&] {
            for (;;) {
                long long t100 = 0;
                const unsigned got = stalled.poll(buffer.data(), stalled.bufferFrames(), &t100);
                if (got == 0)
                    return;
                if (first < 0)
                    first = t100;
                if (expectedNext >= 0)
                    worstJump = std::max(worstJump, std::llabs(t100 - expectedNext));
                expectedNext = t100 + (long long)got * 10000000LL / 48000;
                frames += got;
            }
        };
        while (span.elapsed() < 300) { drain(); QTest::qSleep(16); }
        QTest::qSleep(400);
        while (span.elapsed() < 1100) { drain(); QTest::qSleep(16); }
        drain();
        const double capturedMs = double(frames) / 48.0;
        const double spannedMs = double(expectedNext - first) / 10000.0;
        qInfo().noquote() << QStringLiteral("game audio stall: captured=%1ms spanned=%2ms "
                                            "worstJump=%3ms")
            .arg(capturedMs, 0, 'f', 1).arg(spannedMs, 0, 'f', 1).arg(worstJump / 10000.0, 0, 'f', 1);
        QVERIFY(!stalled.failed());
        QVERIFY2(worstJump <= 200000, "Game audio skipped packets across a worker stall");
        QVERIFY2(capturedMs >= spannedMs - 25.0, "Game audio lost samples across a worker stall");
        QVERIFY2(capturedMs >= 900.0, "Game audio stopped delivering during a worker stall");
        stalled.stop();
    }

    // A capture gap (worker stall, loading screen, WGC sending no frames)
    // must not lengthen the saved video: the encoder holds the last frame
    // across the gap, and the clip must still match real time so it stays in
    // sync with the audio. Synthetic timestamps keep this deterministic.
    void captureGapKeepsTheVideoTimeline()
    {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        QVERIFY(SUCCEEDED(com));
        const auto apartment = qScopeGuard([] { CoUninitialize(); });
        QTemporaryDir dir(QDir::currentPath() + "/segment-gap-XXXXXX");
        QVERIFY(dir.isValid());
        VideoFixture video;
        QVERIFY(makeVideo(video));
        SegmentRecorder recorder;
        QVERIFY(recorder.begin(320, 180, 320, 180, 30, 2, 1, 10, dir.path(),
                               video.device.Get(), video.context.Get()));
        const auto finish = qScopeGuard([&] {
            recorder.discardCurrentSegment();
            recorder.end();
        });
        constexpr long long kFrame = 10000000LL / 30;
        // 0-1.5 s, a 400 ms gap inside the second segment, then up to 3 s.
        for (long long t = 0; t <= 30000000LL; t += kFrame) {
            if (t > 15000000LL && t < 19000000LL)
                continue;
            recorder.writeFrame(video.texture.Get(), video.device.Get(), video.context.Get(), t);
        }
        const SegmentLease saved = recorder.snapshotForSave();
        QCOMPARE(saved.paths().size(), qsizetype(3));
        const QString clip = dir.path() + "/gap.mp4";
        QVERIFY(ReplayExporter::concat(saved.paths(), clip, QStringLiteral("gap"),
                                       ReplayExporter::Audio::Any));

        using Microsoft::WRL::ComPtr;
        QVERIFY(SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)));
        const auto mf = qScopeGuard([] { MFShutdown(); });
        ComPtr<IMFSourceReader> reader;
        QVERIFY(SUCCEEDED(MFCreateSourceReaderFromURL(
            reinterpret_cast<LPCWSTR>(clip.utf16()), nullptr, reader.GetAddressOf())));
        PROPVARIANT duration;
        PropVariantInit(&duration);
        QVERIFY(SUCCEEDED(reader->GetPresentationAttribute(
            DWORD(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &duration)));
        const double seconds = double(duration.uhVal.QuadPart) / 1e7;
        PropVariantClear(&duration);
        qInfo().noquote() << QStringLiteral("capture gap: clip lasts %1 s for 3.0 s of capture")
                                 .arg(seconds, 0, 'f', 3);
        // Before the fix the 400 ms gap was counted twice: ~3.37 s.
        QVERIFY2(seconds > 2.9 && seconds < 3.1, "Saved video length does not match real time");
    }

    void earlySnapshot_data()
    {
        QTest::addColumn<bool>("hasFrames");
        QTest::newRow("empty-is-not-saveable") << false;
        QTest::newRow("one-second-of-thirty-is-saveable") << true;
    }

    void earlySnapshot()
    {
        QFETCH(bool, hasFrames);
        using Microsoft::WRL::ComPtr;
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        QVERIFY(SUCCEEDED(com));
        const auto apartment = qScopeGuard([] { CoUninitialize(); });
        QTemporaryDir dir(QDir::currentPath() + "/segment-partial-XXXXXX");
        QVERIFY(dir.isValid());
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        QVERIFY(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, device.GetAddressOf(), nullptr,
            context.GetAddressOf())));
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = 320;
        desc.Height = 180;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.Usage = D3D11_USAGE_DEFAULT;
        const QByteArray pixels(320 * 180 * 4, char(0x80));
        D3D11_SUBRESOURCE_DATA data{};
        data.pSysMem = pixels.constData();
        data.SysMemPitch = 320 * 4;
        ComPtr<ID3D11Texture2D> texture;
        QVERIFY(SUCCEEDED(device->CreateTexture2D(&desc, &data, texture.GetAddressOf())));
        SegmentRecorder recorder;
        QVERIFY(recorder.begin(320, 180, 320, 180, 30, 2, 5, 30,
                               dir.path(), device.Get(), context.Get()));
        const auto finish = qScopeGuard([&] {
            recorder.discardCurrentSegment();
            recorder.end();
        });
        if (hasFrames) {
            for (int i = 0; i < 30; ++i)
                recorder.writeFrame(texture.Get(), device.Get(), context.Get(),
                                    10000000LL + i * 10000000LL / 30);
        }
        QVERIFY(!recorder.hasClosedMedia());
        const SegmentLease clip = recorder.snapshotForSave();
        QCOMPARE(recorder.hasClosedMedia(), hasFrames);
        QCOMPARE(clip.isEmpty(), !hasFrames);
        QVERIFY(recorder.isActive()); // saving must leave recording usable
        if (!hasFrames)
            return;
        QCOMPARE(clip.paths().size(), 1);
        const QString path = dir.path() + "/video-only-export.mp4";
        QVERIFY(ReplayExporter::concat(clip.paths(), path, QStringLiteral("video-only-regression")));
        ComPtr<IMFSourceReader> reader;
        QVERIFY(SUCCEEDED(MFCreateSourceReaderFromURL(
            reinterpret_cast<LPCWSTR>(path.utf16()), nullptr, reader.GetAddressOf())));
        PROPVARIANT duration{};
        const auto clearDuration = qScopeGuard([&] { PropVariantClear(&duration); });
        QVERIFY(SUCCEEDED(reader->GetPresentationAttribute(
            MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &duration)));
        QCOMPARE(duration.vt, VARTYPE(VT_UI8));
        QVERIFY(duration.uhVal.QuadPart > 0);
        QVERIFY(duration.uhVal.QuadPart < 50000000ULL); // shorter than first segment
        ComPtr<IMFSample> sample;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        QVERIFY(SUCCEEDED(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
            0, nullptr, &flags, &timestamp, sample.GetAddressOf())));
        QVERIFY(sample.Get() != nullptr); // finalized file really contains video
    }

    // A crash leaves an empty or garbage *_clip.mp4 in the cache. Restoring it
    // used to make every save fail (the exporter is strict) until the ring
    // rolled past it. Only segments with real video may rejoin the ring.
    void restoreRingAdmitsOnlyReadableSegments()
    {
        using Microsoft::WRL::ComPtr;
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        QVERIFY(SUCCEEDED(com));
        const auto apartment = qScopeGuard([] { CoUninitialize(); });
        QTemporaryDir dir(QDir::currentPath() + "/segment-restore-XXXXXX");
        QVERIFY(dir.isValid());
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        QVERIFY(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, device.GetAddressOf(), nullptr,
            context.GetAddressOf())));
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = 320;
        desc.Height = 180;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.Usage = D3D11_USAGE_DEFAULT;
        const QByteArray pixels(320 * 180 * 4, char(0x80));
        D3D11_SUBRESOURCE_DATA data{};
        data.pSysMem = pixels.constData();
        data.SysMemPitch = 320 * 4;
        ComPtr<ID3D11Texture2D> texture;
        QVERIFY(SUCCEEDED(device->CreateTexture2D(&desc, &data, texture.GetAddressOf())));

        QStringList realSegments;
        {
            SegmentRecorder first;
            QVERIFY(first.begin(320, 180, 320, 180, 30, 2, 5, 30,
                                dir.path(), device.Get(), context.Get()));
            for (int i = 0; i < 30; ++i)
                first.writeFrame(texture.Get(), device.Get(), context.Get(),
                                 10000000LL + i * 10000000LL / 30);
            const SegmentLease clip = first.snapshotForSave();
            QVERIFY(!clip.isEmpty());
            realSegments = clip.paths();
            first.discardCurrentSegment();
            first.end();
        }
        for (const QString& path : realSegments)
            QVERIFY(ReplayExporter::hasReadableVideo(path));

        // Sorted after the real segment, so it would be the newest in the ring.
        const QString empty = dir.path() + "/2999-01-01_00-00-00-000_clip.mp4";
        const QString garbage = dir.path() + "/2999-01-01_00-00-01-000_clip.mp4";
        {
            QFile file(empty);
            QVERIFY(file.open(QIODevice::WriteOnly));
        }
        {
            QFile file(garbage);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(QByteArray(4096, 'x'));
        }
        QVERIFY(!ReplayExporter::hasReadableVideo(empty));
        QVERIFY(!ReplayExporter::hasReadableVideo(garbage));

        SegmentRecorder second;
        QVERIFY(second.begin(320, 180, 320, 180, 30, 2, 5, 30,
                             dir.path(), device.Get(), context.Get()));
        const auto finish = qScopeGuard([&] {
            second.discardCurrentSegment();
            second.end();
        });
        QVERIFY(!QFileInfo::exists(empty));
        QVERIFY(!QFileInfo::exists(garbage));
        const SegmentLease restored = second.snapshotForSave();
        QVERIFY(!restored.isEmpty());
        for (const QString& path : realSegments)
            QVERIFY(restored.paths().contains(path));
        const QString out = dir.path() + "/restored-export.mp4";
        QVERIFY(ReplayExporter::concat(restored.paths(), out, QStringLiteral("restore-check")));
    }

    void readinessRequiresConfirmedMediaInTheNormalWindow()
    {
        SegmentRecorder recorder;
        recorder.m_keepSegments = 2;
        recorder.m_segments = {"old-leased", "recent", "newest"};
        QVERIFY(!recorder.hasClosedMedia()); // restored filenames alone prove nothing
        recorder.m_lastClosedMediaPath = "old-leased";
        QVERIFY(!recorder.hasClosedMedia()); // retained lease is outside a new save
        recorder.m_lastClosedMediaPath = "recent";
        QVERIFY(recorder.hasClosedMedia());
        recorder.m_segments << "next";
        QVERIFY(!recorder.hasClosedMedia()); // old usable video rolled out of the window
        recorder.m_lastClosedMediaPath = "next";
        QVERIFY(recorder.hasClosedMedia());
    }

    void replacementRecorderRetainsLeasedStalePaths()
    {
        QTemporaryDir dir(QDir::currentPath() + "/segment-lease-XXXXXX");
        QVERIFY(dir.isValid());
        QStringList paths;
        for (int i = 0; i < 8; ++i)
            paths << segment(dir.path(), i, i < 3);
        SegmentLease lease(paths.first(2));
        auto oldRecorder = std::make_unique<SegmentRecorder>();
        oldRecorder->m_segments = paths.first(2);
        oldRecorder.reset();

        SegmentRecorder replacement;
        restore(replacement, dir.path(), 2);
        QCOMPARE(replacement.m_segments, (QStringList{paths[0], paths[1], paths[6], paths[7]}));
        for (int i = 0; i < paths.size(); ++i)
            QCOMPARE(QFile::exists(paths[i]), i < 2 || i >= 6);
        // Re-arm the same recorder and shrink the window while the export owns it.
        restore(replacement, dir.path(), 1);
        QCOMPARE(replacement.m_segments, (QStringList{paths[0], paths[1], paths[7]}));
        lease = {};
        replacement.trimRing();
        QCOMPARE(replacement.m_segments, QStringList{paths[7]});
    }

    void overlappingLeasesReleaseOnlyTheirOwnPaths()
    {
        QTemporaryDir dir(QDir::currentPath() + "/segment-lease-XXXXXX");
        QVERIFY(dir.isValid());
        QStringList paths;
        for (int i = 0; i < 8; ++i)
            paths << segment(dir.path(), i);
        SegmentLease first(paths.first(2));
        SegmentLease second({paths[1], paths[2]});
        QVERIFY(first.ownerId() != second.ownerId());
        SegmentRecorder recorder;
        restore(recorder, dir.path(), 2);
        QCOMPARE(recorder.m_segments.size(), 5);
        first = {};
        recorder.trimRing();
        QCOMPARE(recorder.m_segments, (QStringList{paths[1], paths[2], paths[6], paths[7]}));
        second = {};
        recorder.trimRing();
        QCOMPARE(recorder.m_segments, paths.last(2));
    }

    void oldLeaseDoesNotBlockUnrelatedTrimming()
    {
        QTemporaryDir dir(QDir::currentPath() + "/segment-lease-XXXXXX");
        QVERIFY(dir.isValid());
        const QString old = segment(dir.path(), 0);
        SegmentLease lease({old});
        SegmentRecorder recorder;
        restore(recorder, dir.path(), 2);
        for (int i = 1; i <= 20; ++i) {
            recorder.m_segments << segment(dir.path(), i);
            recorder.trimRing();
            QCOMPARE(recorder.m_segments.size(), qMin(i + 1, 3));
            QVERIFY(QFile::exists(old));
            QCOMPARE(QDir(dir.path()).entryList({"*_clip.mp4"}, QDir::Files).size(),
                     recorder.m_segments.size());
        }
    }

    void normalizedPathsShareProtection()
    {
        QTemporaryDir dir(QDir::currentPath() + "/segment-lease-XXXXXX");
        QVERIFY(dir.isValid());
        const QString path = segment(dir.path(), 0);
        QString alias = dir.path() + "/./0000_clip.mp4";
#ifdef Q_OS_WIN
        alias = QDir::toNativeSeparators(alias.toUpper());
#endif
        SegmentLease lease({alias, alias});
        QVERIFY(!SegmentLease::removeIfUnleased(path));
        lease = {};
        QVERIFY(SegmentLease::removeIfUnleased(path));
    }

    void exportReadsCompleteSnapshotAfterRecorderReplacement()
    {
        QTemporaryDir dir(QDir::currentPath() + "/segment-lease-XXXXXX");
        QVERIFY(dir.isValid());
        QStringList paths;
        for (int i = 0; i < 3; ++i)
            paths << segment(dir.path(), i);
        auto recorder = std::make_unique<SegmentRecorder>();
        restore(*recorder, dir.path(), 3);
        auto entered = std::make_shared<QSemaphore>();
        auto resume = std::make_shared<QSemaphore>();
        auto bytes = std::make_shared<QByteArray>();
        ReplayExportTask task(SegmentLease(recorder->m_segments),
            [entered, resume, bytes](const QStringList& snapshot) {
                entered->release();
                resume->acquire();
                for (const QString& path : snapshot) {
                    QFile file(path);
                    if (file.open(QIODevice::ReadOnly))
                        *bytes += file.readAll();
                }
            });
        // Always unblock before the task destructor, including assertion exits.
        const auto unblock = qScopeGuard([resume] { resume->release(); });
        task.start();
        QVERIFY(entered->tryAcquire(1, 2000));
        recorder.reset();
        for (int i = 3; i < 8; ++i)
            segment(dir.path(), i);
        recorder = std::make_unique<SegmentRecorder>();
        restore(*recorder, dir.path(), 2);
        QCOMPARE(recorder->m_segments.size(), 5);
        resume->release();
        QVERIFY(task.wait(2000));
        QCOMPARE(*bytes, QByteArray("012"));
        // Task still exists, and no finished callback has run: lease is gone.
        recorder->trimRing();
        QCOMPARE(recorder->m_segments.size(), 2);
        for (const QString& path : paths)
            QVERIFY(!QFile::exists(path));
    }

    void leaseCleanupWithoutCompletionCallback_data()
    {
        QTest::addColumn<bool>("throws");
        QTest::newRow("early-return") << false;
        QTest::newRow("exception") << true;
    }

    void leaseCleanupWithoutCompletionCallback()
    {
        QFETCH(bool, throws);
        QTemporaryDir dir(QDir::currentPath() + "/segment-lease-XXXXXX");
        QVERIFY(dir.isValid());
        const QString path = segment(dir.path(), 0);
        ReplayExportTask task(SegmentLease({path}), [throws](const QStringList&) {
            if (throws)
                throw std::runtime_error("simulated export failure");
            return;
        });
        bool callbackRan = false;
        auto receiver = std::make_unique<QObject>();
        connect(&task, &QThread::finished, receiver.get(), [&] { callbackRan = true; },
                Qt::QueuedConnection);
        receiver.reset();
        if (throws)
            QTest::ignoreMessage(QtWarningMsg, "ReplayExportTask: export failed with an exception");
        task.start();
        QVERIFY(task.wait(2000));
        QCoreApplication::processEvents();
        QVERIFY(!callbackRan);
        QVERIFY(SegmentLease::removeIfUnleased(path));
    }

    void shutdownJoinsBeyondGracePeriod()
    {
        QTemporaryDir dir(QDir::currentPath() + "/segment-lease-XXXXXX");
        QVERIFY(dir.isValid());
        const QString path = segment(dir.path(), 0);
        auto entered = std::make_shared<QSemaphore>();
        auto resume = std::make_shared<QSemaphore>();
        auto completed = std::make_shared<std::atomic_bool>(false);
        ReplayExportTask task(SegmentLease({path}),
            [entered, resume, completed](const QStringList&) {
                entered->release();
                resume->acquire();
                *completed = true;
            });
        const auto unblock = qScopeGuard([resume] { resume->release(); });
        task.start();
        QVERIFY(entered->tryAcquire(1, 2000));
        std::jthread release([resume] {
            QThread::msleep(100);
            resume->release();
        });
        QTest::ignoreMessage(QtWarningMsg,
                            "ReplayExportTask: shutdown grace period elapsed; waiting for export");
        QVERIFY(!task.finish(1));
        QVERIFY(completed->load());
        QVERIFY(task.isFinished());
        QVERIFY(SegmentLease::removeIfUnleased(path));
    }
};

QTEST_GUILESS_MAIN(TestSegmentRecorder)
#include "tst_segmentrecorder.moc"
