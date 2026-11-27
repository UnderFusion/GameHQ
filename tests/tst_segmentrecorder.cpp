#include "capture/SegmentRecorder.h"
#include "capture/Fmp4Index.h"
#include "capture/AudioCapture.h"
#include "capture/CaptureUtil.h"
#include "capture/ReplayExporter.h"
#include "capture/ReplayExportTask.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QScopeGuard>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <memory>
#include <thread>
#include <vector>
#include <d3d11.h>
#include <mmdeviceapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <audioclient.h>
#include <wrl/client.h>

// m_segmentProbe is a plain function pointer, so a file-scope counter is the
// only way for the restore tests below to observe how often it was called.
static int g_restoreProbes = 0;

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
    void shutdownReleasesEncoderBeforeJoiningPreparation()
    {
        struct ActiveWriter final : IMFSinkWriter {
            QSemaphore slotReleased;
            bool finalized = false;
            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
            ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
            ULONG STDMETHODCALLTYPE Release() override { slotReleased.release(); return 0; }
            HRESULT STDMETHODCALLTYPE Finalize() override { finalized = true; return S_OK; }
            HRESULT STDMETHODCALLTYPE AddStream(IMFMediaType*, DWORD*) override { return E_NOTIMPL; }
            HRESULT STDMETHODCALLTYPE SetInputMediaType(DWORD, IMFMediaType*, IMFAttributes*) override { return E_NOTIMPL; }
            HRESULT STDMETHODCALLTYPE BeginWriting() override { return E_NOTIMPL; }
            HRESULT STDMETHODCALLTYPE WriteSample(DWORD, IMFSample*) override { return E_NOTIMPL; }
            HRESULT STDMETHODCALLTYPE SendStreamTick(DWORD, LONGLONG) override { return E_NOTIMPL; }
            HRESULT STDMETHODCALLTYPE PlaceMarker(DWORD, void*) override { return E_NOTIMPL; }
            HRESULT STDMETHODCALLTYPE NotifyEndOfSegment(DWORD) override { return E_NOTIMPL; }
            HRESULT STDMETHODCALLTYPE Flush(DWORD) override { return E_NOTIMPL; }
            HRESULT STDMETHODCALLTYPE GetServiceForStream(DWORD, REFGUID, REFIID, void**) override { return E_NOTIMPL; }
            HRESULT STDMETHODCALLTYPE GetStatistics(DWORD, MF_SINK_WRITER_STATISTICS*) override { return E_NOTIMPL; }
        } writer;
        SegmentRecorder recorder;
        recorder.m_writer = &writer;
        bool acquired = false;
        recorder.m_prepThread = std::thread([&] {
            acquired = writer.slotReleased.tryAcquire(1, 2000);
        });
        recorder.end();
        QVERIFY(writer.finalized);
        QVERIFY2(acquired, "The active encoder held its only slot through the preparation join");
        QVERIFY(!recorder.m_prepThread.joinable());
    }

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
    // Waking from sleep (or any multi-hour stall) used to fill the whole gap
    // with encoded silence in one call: hours of AAC on the capture worker.
    // A gap past the fill bound re-anchors the clock; the clip stays short.
    void aLongStallReanchorsAudioInsteadOfFilling()
    {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        QVERIFY(SUCCEEDED(com));
        const auto apartment = qScopeGuard([] { CoUninitialize(); });
        QTemporaryDir dir(QDir::currentPath() + "/segment-stall-XXXXXX");
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
        for (unsigned i = 0; i < audioFrames; ++i)
            tone[i * 2] = tone[i * 2 + 1] = float(0.3 * std::sin(6.283185307 * 440.0 * i / 48000.0));
        const auto run = [&](qint64 start, int frames) {
            for (int frame = 0; frame < frames; ++frame) {
                const qint64 timestamp = start + frame * 10000000LL / 30;
                recorder.writeFrame(video.texture.Get(), video.device.Get(), video.context.Get(),
                                    timestamp);
                recorder.writeAudio(tone.data(), audioFrames, timestamp);
            }
        };
        run(10000000LL, 45);
        const qint64 threeHours = 3LL * 3600 * 10000000LL;
        QElapsedTimer timer;
        timer.start();
        recorder.padAudioSilence(10000000LL + threeHours);   // the first tick after waking
        run(10000000LL + threeHours, 45);
        QVERIFY2(timer.elapsed() < 5000, qPrintable(QString::number(timer.elapsed())));

        const SegmentLease saved = recorder.snapshotForSave();
        QVERIFY(!saved.isEmpty());
        const QString clip = dir.path() + "/after-stall.mp4";
        QVERIFY(ReplayExporter::concat(saved.paths(), clip, QStringLiteral("stall-regression"),
                                       ReplayExporter::Audio::Required));
        using Microsoft::WRL::ComPtr;
        ComPtr<IMFSourceReader> reader;
        QVERIFY(SUCCEEDED(MFCreateSourceReaderFromURL(
            reinterpret_cast<LPCWSTR>(clip.utf16()), nullptr, reader.GetAddressOf())));
        PROPVARIANT duration{};
        const auto clearDuration = qScopeGuard([&] { PropVariantClear(&duration); });
        QVERIFY(SUCCEEDED(reader->GetPresentationAttribute(
            MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &duration)));
        QVERIFY2(duration.uhVal.QuadPart < 200000000ULL,   // well under 20 s, not 3 h
                 qPrintable(QString::number(duration.uhVal.QuadPart / 10000)));
    }

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
            // Setup, GPU initialization and encoding can outlast the measured
            // three-second clip. The scope guard owns the renderers' lifetimes.
            // A longer watchdog only limits orphan playback if the test crashes.
            process.setArguments({ QString::number(hz), QStringLiteral("60000"),
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
        QVERIFY2(game.state() == QProcess::Running && !game.waitForFinished(0),
                 "The game tone stopped before the recording ended");
        QVERIFY2(other.state() == QProcess::Running && !other.waitForFinished(0),
                 "The unrelated tone stopped before the recording ended");
        // No more PCM is consumed from this instance during snapshot or decode.
        audio.stop();
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

private:
    // --- #39 encoder reuse: hardware device, real encoder, no window ---------

    struct HwVideo {
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        unsigned width = 0, height = 0;
        std::vector<unsigned char> pixels;
    };

    // Same device flags as FramePumpPipeline (default adapter, BGRA support).
    static bool makeHwVideo(HwVideo& v, unsigned w, unsigned h)
    {
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                v.device.GetAddressOf(), nullptr, v.context.GetAddressOf())))
            return false;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.Usage = D3D11_USAGE_DEFAULT;
        // WGC frame-pool and tone-mapper textures are render targets too; the
        // video processor input view needs that.
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        v.width = w;
        v.height = h;
        v.pixels.assign(size_t(w) * h * 4, 0);
        return SUCCEEDED(v.device->CreateTexture2D(&desc, nullptr, v.texture.GetAddressOf()));
    }

    // Moving content so every frame encodes differently: a scrolling gradient
    // with a bright bar whose x position encodes the frame number.
    static void drawFrame(HwVideo& v, int frame)
    {
        for (unsigned y = 0; y < v.height; ++y) {
            unsigned char* row = v.pixels.data() + size_t(y) * v.width * 4;
            for (unsigned x = 0; x < v.width; ++x) {
                const unsigned char g = (unsigned char)((x + y + unsigned(frame) * 5) & 0xff);
                row[x * 4 + 0] = g;
                row[x * 4 + 1] = (unsigned char)(255 - g);
                row[x * 4 + 2] = (unsigned char)((x * 3) & 0xff);
                row[x * 4 + 3] = 255;
            }
        }
        const unsigned bar = (unsigned(frame) * 7) % (v.width - 16);
        for (unsigned y = v.height / 4; y < v.height * 3 / 4; ++y)
            for (unsigned x = bar; x < bar + 16; ++x)
                std::fill_n(v.pixels.data() + (size_t(y) * v.width + x) * 4, 3, (unsigned char)250);
        v.context->UpdateSubresource(v.texture.Get(), 0, nullptr, v.pixels.data(), v.width * 4, 0);
    }

    struct VideoTrack {
        int frames = 0;
        bool firstIsKey = false;
        LONGLONG firstTime = -1;
        bool monotonic = true;
    };

    // Reads the compressed video samples of one file: count, IDR at the start,
    // start time and strictly increasing timestamps.
    static bool readVideoTrack(const QString& file, VideoTrack& out)
    {
        using Microsoft::WRL::ComPtr;
        ComPtr<IMFSourceReader> reader;
        if (FAILED(MFCreateSourceReaderFromURL(reinterpret_cast<LPCWSTR>(file.utf16()),
                                               nullptr, reader.GetAddressOf())))
            return false;
        reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
        if (FAILED(reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE)))
            return false;
        LONGLONG last = -1;
        for (int i = 0; i < 100000; ++i) {
            ComPtr<IMFSample> sample;
            DWORD flags = 0;
            LONGLONG t = 0;
            if (FAILED(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr,
                                          &flags, &t, sample.GetAddressOf())))
                return false;
            if (sample) {
                if (out.frames == 0) {
                    out.firstTime = t;
                    out.firstIsKey = MFGetAttributeUINT32(sample.Get(), MFSampleExtension_CleanPoint, 0) != 0;
                }
                if (t <= last) {
                    if (out.monotonic)
                        qWarning().noquote() << QStringLiteral("timestamp %1 after %2 at sample %3 in %4")
                            .arg(t).arg(last).arg(out.frames).arg(QFileInfo(file).fileName());
                    out.monotonic = false;
                }
                last = t;
                ++out.frames;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
                return true;
        }
        return false;
    }

    // Decodes the first video frame of a file to NV12 (Y plane, then UV).
    static bool decodeFirstFrameNv12(const QString& file, unsigned w, unsigned h,
                                     std::vector<unsigned char>& nv12)
    {
        using Microsoft::WRL::ComPtr;
        ComPtr<IMFAttributes> attrs;
        MFCreateAttributes(attrs.GetAddressOf(), 1);
        attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        ComPtr<IMFSourceReader> reader;
        if (FAILED(MFCreateSourceReaderFromURL(reinterpret_cast<LPCWSTR>(file.utf16()),
                                               attrs.Get(), reader.GetAddressOf())))
            return false;
        ComPtr<IMFMediaType> type;
        MFCreateMediaType(type.GetAddressOf());
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        if (FAILED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, type.Get())))
            return false;
        ComPtr<IMFMediaType> current;
        reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, current.GetAddressOf());
        UINT32 stride = MFGetAttributeUINT32(current.Get(), MF_MT_DEFAULT_STRIDE, w);
        UINT32 fw = 0, fh = 0;
        MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &fw, &fh);   // may be padded (e.g. 1088)
        for (int i = 0; i < 64; ++i) {
            ComPtr<IMFSample> sample;
            DWORD flags = 0;
            LONGLONG t = 0;
            if (FAILED(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr,
                                          &flags, &t, sample.GetAddressOf())))
                return false;
            if (sample) {
                ComPtr<IMFMediaBuffer> buffer;
                sample->ConvertToContiguousBuffer(buffer.GetAddressOf());
                BYTE* p = nullptr;
                DWORD len = 0;
                buffer->Lock(&p, nullptr, &len);
                nv12.assign(size_t(w) * h * 3 / 2, 0);
                const UINT32 planeRows = fh ? fh : h;
                for (unsigned y = 0; y < h; ++y)
                    memcpy(nv12.data() + size_t(y) * w, p + size_t(y) * stride, w);
                for (unsigned y = 0; y < h / 2; ++y)
                    memcpy(nv12.data() + size_t(w) * h + size_t(y) * w,
                           p + size_t(stride) * planeRows + size_t(y) * stride, w);
                buffer->Unlock();
                return true;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
                return false;
        }
        return false;
    }

private slots:
    void reusedEncoderWritesIndependentSegments()
    {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        QVERIFY(SUCCEEDED(com));
        const auto apartment = qScopeGuard([] { CoUninitialize(); });
        QTemporaryDir dir(QDir::currentPath() + "/segment-reuse-XXXXXX");
        QVERIFY(dir.isValid());
        HwVideo video;
        if (!makeHwVideo(video, 1280, 720))
            QSKIP("No hardware D3D11 device");
        SegmentRecorder::setEncoderReuseEnabled(true);
        SegmentRecorder recorder;
        // 1280x720 source encoded at 640x360: exercises the GPU scale + NV12 path.
        QVERIFY(recorder.begin(1280, 720, 640, 360, 30, 4, 1, 10, dir.path(),
                               video.device.Get(), video.context.Get(), 48000, 2));
        const auto finish = qScopeGuard([&] {
            recorder.discardCurrentSegment();
            recorder.end();
        });
        if (!recorder.reusesEncoder())
            QSKIP("No hardware encoder accepted NV12 here; the per-segment path is used");
        QVERIFY(recorder.hasAudio());

        constexpr unsigned audioFrames = 48000 / 30;
        std::array<float, audioFrames * 2> audio{};
        SegmentLease firstSave;
        for (int frame = 0; frame < 225; ++frame) {   // 7.5 s
            const qint64 t = 10000000LL + frame * 10000000LL / 30;
            drawFrame(video, frame);
            recorder.writeFrame(video.texture.Get(), video.device.Get(), video.context.Get(), t);
            for (unsigned i = 0; i < audioFrames; ++i) {
                const double s = (frame * audioFrames + i) / 48000.0;
                audio[i * 2] = audio[i * 2 + 1] = float(0.3 * std::sin(6.283185307 * 440.0 * s));
            }
            recorder.writeAudio(audio.data(), audioFrames, t);
            if (frame == 100)
                firstSave = recorder.snapshotForSave();
        }
        QVERIFY(recorder.reusesEncoder());   // no fallback happened
        QVERIFY(firstSave.paths().size() >= 3);
        const SegmentLease saved = recorder.snapshotForSave();
        QVERIFY(saved.paths().size() >= 6);

        int total = 0;
        for (const QString& path : saved.paths()) {
            VideoTrack track;
            QVERIFY2(readVideoTrack(path, track), qPrintable(path));
            qInfo().noquote() << QStringLiteral("reuse segment %1: %2 frames, first t=%3, key=%4, %5 bytes")
                .arg(QFileInfo(path).fileName()).arg(track.frames).arg(track.firstTime)
                .arg(track.firstIsKey).arg(QFileInfo(path).size());
            QVERIFY2(track.frames > 0, "segment without video");
            QVERIFY2(track.firstIsKey, "segment does not start with an IDR frame");
            QVERIFY2(track.firstTime >= 0 && track.firstTime < 333334, "segment does not start at t=0");
            QVERIFY2(track.monotonic, "timestamps not strictly increasing");
            QVERIFY2(track.frames <= 31, "segment holds frames of another segment");
            total += track.frames;
        }
        // The 10 s ring still holds all 7.5 s: every frame, none lost or doubled
        // across 1 s rolls and the two snapshots.
        QCOMPARE(total, 225);

        const QString clip = dir.path() + "/reuse.mp4";
        QVERIFY(ReplayExporter::concat(saved.paths(), clip, QStringLiteral("reuse"),
                                       ReplayExporter::Audio::Required));
        VideoTrack exported;
        QVERIFY(readVideoTrack(clip, exported));
        QCOMPARE(exported.frames, total);
        QVERIFY(exported.firstIsKey && exported.monotonic);
    }

    void reusedEncoderFailureFallsBackToPerSegmentEncoder()
    {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        QVERIFY(SUCCEEDED(com));
        const auto apartment = qScopeGuard([] { CoUninitialize(); });
        QTemporaryDir dir(QDir::currentPath() + "/segment-reuse-fail-XXXXXX");
        QVERIFY(dir.isValid());
        HwVideo video;
        if (!makeHwVideo(video, 640, 360))
            QSKIP("No hardware D3D11 device");
        SegmentRecorder::setEncoderReuseEnabled(true);
        SegmentRecorder recorder;
        QVERIFY(recorder.begin(640, 360, 640, 360, 30, 4, 1, 10, dir.path(),
                               video.device.Get(), video.context.Get()));
        const auto finish = qScopeGuard([&] {
            recorder.discardCurrentSegment();
            recorder.end();
        });
        if (!recorder.reusesEncoder())
            QSKIP("No hardware encoder accepted NV12 here");
        int frame = 0;
        const auto write = [&](int count) {
            for (int i = 0; i < count; ++i, ++frame) {
                drawFrame(video, frame);
                recorder.writeFrame(video.texture.Get(), video.device.Get(), video.context.Get(),
                                    frame * 10000000LL / 30);
            }
        };
        write(45);                                  // segment 0 closed, segment 1 half full
        QVERIFY(recorder.reusesEncoder() && recorder.m_encoder);
        recorder.m_encoder->injectFailure();        // the encoder rejects the next frame
        write(1);
        QVERIFY(!recorder.reusesEncoder());
        QVERIFY(recorder.isActive());
        write(74);                                  // per-segment encoder from here on
        QVERIFY(recorder.isActive());
        const SegmentLease saved = recorder.snapshotForSave();
        QVERIFY(saved.paths().size() >= 3);
        for (const QString& path : saved.paths()) {
            VideoTrack track;
            QVERIFY2(readVideoTrack(path, track), qPrintable(path));
            QVERIFY2(track.frames > 0 && track.firstIsKey && track.monotonic, qPrintable(path));
        }
        const QString clip = dir.path() + "/fallback.mp4";
        QVERIFY(ReplayExporter::concat(saved.paths(), clip, QStringLiteral("fallback"),
                                       ReplayExporter::Audio::Any));
    }

    // The GPU BGRA->NV12 conversion must give the same colours as the Color
    // Converter MFT of the per-segment path (BT.709, limited range).
    void reusedEncoderMatchesLegacyColours_data()
    {
        QTest::addColumn<unsigned>("w");
        QTest::addColumn<unsigned>("h");
        QTest::newRow("sd-bt601") << 640u << 360u;    // <= 576 lines
        QTest::newRow("hd-bt709") << 1280u << 720u;
    }

    void reusedEncoderMatchesLegacyColours()
    {
        QFETCH(unsigned, w);
        QFETCH(unsigned, h);
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        QVERIFY(SUCCEEDED(com));
        const auto apartment = qScopeGuard([] { CoUninitialize(); });
        const auto restoreDefault = qScopeGuard([] { SegmentRecorder::setEncoderReuseEnabled(true); });
        QVERIFY(SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_FULL)));
        const auto mf = qScopeGuard([] { MFShutdown(); });
        static const unsigned char bands[4][3] = { {200, 50, 50}, {40, 180, 60}, {30, 60, 200}, {128, 128, 128} };
        std::array<std::vector<unsigned char>, 2> decoded;
        for (int mode = 0; mode < 2; ++mode) {
            QTemporaryDir dir(QDir::currentPath() + "/segment-colour-XXXXXX");
            QVERIFY(dir.isValid());
            HwVideo video;
            if (!makeHwVideo(video, w, h))
                QSKIP("No hardware D3D11 device");
            for (unsigned y = 0; y < h; ++y)
                for (unsigned x = 0; x < w; ++x) {
                    const unsigned char* c = bands[x * 4 / w];
                    unsigned char* px = video.pixels.data() + (size_t(y) * w + x) * 4;
                    px[0] = c[2]; px[1] = c[1]; px[2] = c[0]; px[3] = 255;
                }
            video.context->UpdateSubresource(video.texture.Get(), 0, nullptr, video.pixels.data(), w * 4, 0);
            SegmentRecorder::setEncoderReuseEnabled(mode == 1);
            SegmentRecorder recorder;
            QVERIFY(recorder.begin(int(w), int(h), int(w), int(h), 30, 8, 5, 10, dir.path(),
                                   video.device.Get(), video.context.Get()));
            if (mode == 1 && !recorder.reusesEncoder()) {
                recorder.discardCurrentSegment();
                recorder.end();
                QSKIP("No hardware encoder accepted NV12 here");
            }
            QCOMPARE(recorder.reusesEncoder(), mode == 1);
            for (int f = 0; f < 15; ++f)
                recorder.writeFrame(video.texture.Get(), video.device.Get(), video.context.Get(),
                                    f * 10000000LL / 30);
            const SegmentLease saved = recorder.snapshotForSave();
            recorder.discardCurrentSegment();
            recorder.end();
            QCOMPARE(saved.paths().size(), qsizetype(1));
            QVERIFY(decodeFirstFrameNv12(saved.paths().first(), w, h, decoded[mode]));
        }
        for (int b = 0; b < 4; ++b) {
            const unsigned x = unsigned(b) * (w / 4) + w / 8, y = h / 2;
            const auto at = [&](const std::vector<unsigned char>& f, size_t i) { return int(f[i]); };
            const size_t yi = size_t(y) * w + x;
            const size_t ci = size_t(w) * h + size_t(y / 2) * w + (x & ~1u);
            const int dy = std::abs(at(decoded[0], yi) - at(decoded[1], yi));
            const int du = std::abs(at(decoded[0], ci) - at(decoded[1], ci));
            const int dv = std::abs(at(decoded[0], ci + 1) - at(decoded[1], ci + 1));
            qInfo().noquote() << QStringLiteral("colour band %1: legacy YUV %2/%3/%4, reuse %5/%6/%7")
                .arg(b).arg(at(decoded[0], yi)).arg(at(decoded[0], ci)).arg(at(decoded[0], ci + 1))
                .arg(at(decoded[1], yi)).arg(at(decoded[1], ci)).arg(at(decoded[1], ci + 1));
            QVERIFY2(dy <= 3 && du <= 3 && dv <= 3, "reused-encoder colours differ from the per-segment path");
        }
    }

    // --- #39 reused encoder: failure recovery and lifecycle (hardware) -------

private:
    // A started hardware recorder that reuses the encoder, or a skip reason.
    struct ReuseSession {
        HwVideo video;
        std::unique_ptr<SegmentRecorder> recorder;
        int frame = 0;
        void write(int count, ID3D11Texture2D* tex = nullptr)
        {
            for (int i = 0; i < count; ++i, ++frame) {
                drawFrame(video, frame);
                recorder->writeFrame(tex ? tex : video.texture.Get(), video.device.Get(),
                                     video.context.Get(), frame * 10000000LL / 30);
            }
        }
    };
    static QString startReuse(ReuseSession& s, const QString& dir, int segmentSeconds = 1)
    {
        if (!makeHwVideo(s.video, 640, 360))
            return QStringLiteral("No hardware D3D11 device");
        SegmentRecorder::setEncoderReuseEnabled(true);
        s.recorder = std::make_unique<SegmentRecorder>();
        if (!s.recorder->begin(640, 360, 640, 360, 30, 4, segmentSeconds, 30, dir,
                               s.video.device.Get(), s.video.context.Get()))
            return QStringLiteral("begin failed");
        if (!s.recorder->reusesEncoder())
            return QStringLiteral("No hardware encoder accepted NV12 here");
        return {};
    }
    static void stopSession(ReuseSession& s)
    {
        if (s.recorder) {
            s.recorder->discardCurrentSegment();
            s.recorder->end();
        }
    }
    // Every saved file decodes, starts on an IDR at t=0 and exports.
    static bool savedClipIsSound(const SegmentLease& lease, const QString& out)
    {
        for (const QString& path : lease.paths()) {
            VideoTrack track;
            if (!readVideoTrack(path, track) || track.frames == 0 || !track.firstIsKey
                || !track.monotonic || track.firstTime != 0)
                return false;
        }
        return ReplayExporter::concat(lease.paths(), out, QStringLiteral("reuse-recovery"),
                                      ReplayExporter::Audio::Any);
    }

private slots:
    void reusedEncoderConversionFailureFallsBack()
    {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        QVERIFY(SUCCEEDED(com));
        const auto apartment = qScopeGuard([] { CoUninitialize(); });
        QTemporaryDir dir(QDir::currentPath() + "/segment-reuse-vp-XXXXXX");
        QVERIFY(dir.isValid());
        ReuseSession s;
        if (const QString why = startReuse(s, dir.path()); !why.isEmpty())
            QSKIP(qPrintable(why));
        const auto finish = qScopeGuard([&] { stopSession(s); });
        s.write(45);                                  // segment 0 closed, segment 1 in flight
        const QString earlier = s.recorder->m_segments.value(0);
        QVERIFY(!earlier.isEmpty());
        // A frame the video processor cannot read (no render-target binding),
        // like a lost or foreign device surface: conversion fails mid-recording.
        D3D11_TEXTURE2D_DESC desc{};
        s.video.texture->GetDesc(&desc);
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> unreadable;
        QVERIFY(SUCCEEDED(s.video.device->CreateTexture2D(&desc, nullptr, unreadable.GetAddressOf())));
        s.write(1, unreadable.Get());
        QVERIFY(!s.recorder->reusesEncoder());
        QVERIFY(s.recorder->isActive());
        s.write(60);                                  // per-segment encoder from here on
        QVERIFY(s.recorder->isActive());
        const SegmentLease saved = s.recorder->snapshotForSave();
        QVERIFY(saved.paths().contains(earlier));     // earlier footage survived the fallback
        QVERIFY(savedClipIsSound(saved, dir.path() + "/vp-fallback.mp4"));
    }

    void reusedEncoderDrainFailureKeepsEarlierSegments()
    {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        QVERIFY(SUCCEEDED(com));
        const auto apartment = qScopeGuard([] { CoUninitialize(); });
        QTemporaryDir dir(QDir::currentPath() + "/segment-reuse-drain-XXXXXX");
        QVERIFY(dir.isValid());
        ReuseSession s;
        if (const QString why = startReuse(s, dir.path()); !why.isEmpty())
            QSKIP(qPrintable(why));
        const auto finish = qScopeGuard([&] { stopSession(s); });
        s.write(45);
        const QString earlier = s.recorder->m_segments.value(0);
        const QString inFlight = s.recorder->m_curPath;
        QVERIFY(!earlier.isEmpty() && !inFlight.isEmpty());
        s.recorder->m_encoder->injectDrainFailure();   // the encoder stalls at the next boundary
        s.write(30);
        QVERIFY(!s.recorder->reusesEncoder());
        QVERIFY(s.recorder->isActive());
        QVERIFY(!QFileInfo::exists(inFlight));         // incomplete segment discarded, not saved
        s.write(60);
        const SegmentLease saved = s.recorder->snapshotForSave();
        QVERIFY(saved.paths().contains(earlier));
        QVERIFY(!saved.paths().contains(inFlight));
        QVERIFY(savedClipIsSound(saved, dir.path() + "/drain-fallback.mp4"));
    }

    void reusedEncoderWriteFailureNeverReachesASavedClip()
    {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        QVERIFY(SUCCEEDED(com));
        const auto apartment = qScopeGuard([] { CoUninitialize(); });
        QTemporaryDir dir(QDir::currentPath() + "/segment-reuse-write-XXXXXX");
        QVERIFY(dir.isValid());
        ReuseSession s;
        if (const QString why = startReuse(s, dir.path()); !why.isEmpty())
            QSKIP(qPrintable(why));
        const auto finish = qScopeGuard([&] { stopSession(s); });
        s.write(45);
        const QString poisoned = s.recorder->m_curPath;
        s.recorder->m_injectWriteFailures = 1;         // the segment file rejects one sample
        s.write(40);
        QVERIFY(s.recorder->isActive());
        QVERIFY(s.recorder->reusesEncoder());          // a file failure is not an encoder failure
        QVERIFY(!QFileInfo::exists(poisoned));
        const SegmentLease saved = s.recorder->snapshotForSave();
        QVERIFY(!saved.paths().contains(poisoned));
        QVERIFY(savedClipIsSound(saved, dir.path() + "/write-failure.mp4"));
    }

    void reusedEncoderShutdownKeepsFramesInsideTheEncoder()
    {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        QVERIFY(SUCCEEDED(com));
        const auto apartment = qScopeGuard([] { CoUninitialize(); });
        QVERIFY(SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_FULL)));   // the worker holds MF past end()
        const auto mf = qScopeGuard([] { MFShutdown(); });
        QTemporaryDir dir(QDir::currentPath() + "/segment-reuse-stop-XXXXXX");
        QVERIFY(dir.isValid());
        ReuseSession s;
        if (const QString why = startReuse(s, dir.path(), 5); !why.isEmpty())
            QSKIP(qPrintable(why));
        s.write(20);                                   // stop while frames are still in the encoder
        const QString last = s.recorder->m_curPath;
        s.recorder->end();                             // capture stop: drain, finalize, release
        QVERIFY(!s.recorder->isActive());
        VideoTrack track;
        QVERIFY(readVideoTrack(last, track));
        QCOMPARE(track.frames, 20);
        QVERIFY(track.firstIsKey && track.monotonic);
        s.recorder.reset();                            // destructor after end() is a no-op
    }

    void reusedEncoderRestartsWithoutGrowth()
    {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        QVERIFY(SUCCEEDED(com));
        const auto apartment = qScopeGuard([] { CoUninitialize(); });
        QVERIFY(SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_FULL)));   // the worker holds MF
        const auto mf = qScopeGuard([] { MFShutdown(); });
        QTemporaryDir dir(QDir::currentPath() + "/segment-reuse-restart-XXXXXX");
        QVERIFY(dir.isValid());
        DWORD warm = 0, last = 0;
        for (int cycle = 0; cycle < 8; ++cycle) {
            ReuseSession s;                            // new device per session, like the pipeline
            if (const QString why = startReuse(s, dir.path()); !why.isEmpty())
                QSKIP(qPrintable(why));
            s.write(40);
            s.recorder->end();
            s.recorder.reset();
            GetProcessHandleCount(GetCurrentProcess(), cycle == 1 ? &warm : &last);
        }
        qInfo().noquote() << QStringLiteral("restart handles: %1 after warm-up, %2 after 6 more sessions")
                                 .arg(warm).arg(last);
        QVERIFY2(long(last) - long(warm) < 30, "handles grow across encoder restarts");
    }

    // --- #39 Media Foundation truncation of valid segments -------------------

private:
    // Samples Media Foundation's MP4 source returns for one stream.
    static int mfSampleCount(const QString& file, DWORD stream)
    {
        Microsoft::WRL::ComPtr<IMFSourceReader> reader;
        if (FAILED(MFCreateSourceReaderFromURL(reinterpret_cast<LPCWSTR>(file.utf16()), nullptr,
                                               reader.GetAddressOf())))
            return -1;
        reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
        if (FAILED(reader->SetStreamSelection(stream, TRUE)))
            return -1;
        int count = 0;
        for (int i = 0; i < 100000; ++i) {
            Microsoft::WRL::ComPtr<IMFSample> sample;
            DWORD flags = 0;
            LONGLONG t = 0;
            if (FAILED(reader->ReadSample(stream, 0, nullptr, &flags, &t, sample.GetAddressOf())))
                return -1;
            if (sample) ++count;
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        }
        return count;
    }

    // Copy of a fragmented segment with a 'free' box before the first moof,
    // sized so the LAST moof crosses a 256 KiB file offset. Windows' MPEG-4
    // source stops reading at such a fragment although the file is valid
    // (measured: only the last moof, only 256 KiB multiples). tfhd base
    // offsets are moved with the data; the optional mfra index is dropped.
    static bool writeStraddlingCopy(const QString& in, const QString& out)
    {
        QFile f(in);
        if (!f.open(QIODevice::ReadOnly)) return false;
        QByteArray d = f.readAll();
        const auto be32 = [&](qint64 p) {
            const auto* u = reinterpret_cast<const unsigned char*>(d.constData() + p);
            return (quint32(u[0]) << 24) | (quint32(u[1]) << 16) | (quint32(u[2]) << 8) | quint32(u[3]);
        };
        struct Top { QByteArray type; qint64 off, size; };
        std::vector<Top> tops;
        for (qint64 off = 0; off + 8 <= d.size();) {
            const qint64 size = be32(off);
            if (size < 8 || off + size > d.size()) return false;
            tops.push_back({ d.mid(off + 4, 4), off, size });
            off += size;
        }
        qint64 firstMoof = -1, lastMoof = -1, lastMoofSize = 0;
        for (const Top& t : tops)
            if (t.type == "moof") { if (firstMoof < 0) firstMoof = t.off; lastMoof = t.off; lastMoofSize = t.size; }
        if (firstMoof < 0) return false;
        constexpr qint64 kGranule = 256 * 1024;
        qint64 boundary = (lastMoof / kGranule + 1) * kGranule;
        qint64 pad = boundary - lastMoof - lastMoofSize / 2;   // boundary lands mid-moof
        while (pad < 8) { boundary += kGranule; pad += kGranule; }
        QByteArray padded = d.left(firstMoof);
        QByteArray freeBox(int(pad), '\0');
        freeBox[0] = char(pad >> 24); freeBox[1] = char(pad >> 16); freeBox[2] = char(pad >> 8); freeBox[3] = char(pad);
        freeBox.replace(4, 4, "free");
        padded += freeBox;
        for (const Top& t : tops) {
            if (t.off < firstMoof || t.type == "mfra") continue;
            QByteArray box = d.mid(t.off, t.size);
            if (t.type == "moof") {   // shift absolute base_data_offset in every tfhd
                for (int i = 8; i + 8 <= box.size(); ++i) {
                    if (box.mid(i + 4, 4) != "tfhd") continue;
                    const auto* u = reinterpret_cast<const unsigned char*>(box.constData() + i + 8);
                    const quint32 flags = (quint32(u[1]) << 16) | (quint32(u[2]) << 8) | quint32(u[3]);
                    if (!(flags & 1)) continue;
                    quint64 base = 0;
                    for (int k = 0; k < 8; ++k) base = (base << 8) | quint8(box[i + 16 + k]);
                    base += quint64(pad);
                    for (int k = 7; k >= 0; --k) { box[i + 16 + k] = char(base & 0xff); base >>= 8; }
                }
            }
            padded += box;
        }
        QFile o(out);
        return o.open(QIODevice::WriteOnly) && o.write(padded) == padded.size();
    }

    // One finalized 5 s segment with video and audio from the given recorder setup.
    static QString recordSegment(const QString& dir)
    {
        HwVideo video;
        if (!makeHwVideo(video, 640, 360)) return {};
        SegmentRecorder recorder;
        if (!recorder.begin(640, 360, 640, 360, 30, 6, 5, 30, dir, video.device.Get(),
                            video.context.Get(), 48000, 2))
            return {};
        std::array<float, 1600 * 2> audio{};
        for (int f = 0; f < 150; ++f) {
            drawFrame(video, f);
            const qint64 t = f * 10000000LL / 30;
            recorder.writeFrame(video.texture.Get(), video.device.Get(), video.context.Get(), t);
            for (unsigned i = 0; i < 1600; ++i)
                audio[i * 2] = audio[i * 2 + 1] = float(0.2 * std::sin(6.283185307 * 440.0 * (f * 1600 + i) / 48000.0));
            recorder.writeAudio(audio.data(), 1600, t);
        }
        const SegmentLease lease = recorder.snapshotForSave();
        recorder.discardCurrentSegment();
        recorder.end();
        return lease.paths().isEmpty() ? QString() : lease.paths().first();
    }

private slots:
    void exportRecoversSamplesMediaFoundationSkips_data()
    {
        QTest::addColumn<bool>("reuse");
        QTest::newRow("per-segment-encoder") << false;
        QTest::newRow("reused-encoder") << true;
    }

    void exportRecoversSamplesMediaFoundationSkips()
    {
        QFETCH(bool, reuse);
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        QVERIFY(SUCCEEDED(com));
        const auto apartment = qScopeGuard([] { CoUninitialize(); });
        const auto restoreDefault = qScopeGuard([] { SegmentRecorder::setEncoderReuseEnabled(true); });
        QVERIFY(SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_FULL)));
        const auto mf = qScopeGuard([] { MFShutdown(); });
        QTemporaryDir dir(QDir::currentPath() + "/segment-mftail-XXXXXX");
        QVERIFY(dir.isValid());
        SegmentRecorder::setEncoderReuseEnabled(reuse);
        const QString segment = recordSegment(dir.path());
        if (segment.isEmpty())
            QSKIP("No hardware D3D11 device for a recording");

        std::vector<Fmp4Index::Track> index;
        QString why;
        QVERIFY2(Fmp4Index::read(segment, index, &why), qPrintable(why));
        const auto track = [&](bool video) {
            for (const auto& t : index) if (video ? t.video : t.audio) return int(t.samples.size());
            return -1;
        };
        const int videoTotal = track(true), audioTotal = track(false);
        QCOMPARE(videoTotal, 150);
        QVERIFY(audioTotal > 200);
        // Positive control: on an unshifted segment the index and MF agree.
        QCOMPARE(mfSampleCount(segment, DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM)), videoTotal);
        QCOMPARE(mfSampleCount(segment, DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM)), audioTotal);

        const QString shifted = dir.path() + "/straddling.mp4";
        QVERIFY(writeStraddlingCopy(segment, shifted));
        std::vector<Fmp4Index::Track> shiftedIndex;
        QVERIFY(Fmp4Index::read(shifted, shiftedIndex, &why));
        const int mfVideo = mfSampleCount(shifted, DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM));
        qInfo().noquote() << QStringLiteral("Media Foundation reads %1 of %2 video samples from the shifted copy")
                                 .arg(mfVideo).arg(videoTotal);
        if (mfVideo == videoTotal)
            qWarning("This Windows build no longer truncates the shifted copy; checking the export only");
        else
            QVERIFY(mfVideo > 0 && mfVideo < videoTotal);

        // The export must contain every sample anyway, in order, with sync preserved.
        const QString clip = dir.path() + "/recovered.mp4";
        QVERIFY(ReplayExporter::concat({ shifted }, clip, QStringLiteral("mf-tail"),
                                       ReplayExporter::Audio::Required));
        VideoTrack exported;
        QVERIFY(readVideoTrack(clip, exported));
        QCOMPARE(exported.frames, videoTotal);
        QVERIFY(exported.firstIsKey && exported.monotonic && exported.firstTime == 0);
        QCOMPARE(mfSampleCount(clip, DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM)), audioTotal);

        // Same result as exporting the original, unshifted segment.
        const QString reference = dir.path() + "/reference.mp4";
        QVERIFY(ReplayExporter::concat({ segment }, reference, QStringLiteral("mf-tail-ref"),
                                       ReplayExporter::Audio::Required));
        VideoTrack expected;
        QVERIFY(readVideoTrack(reference, expected));
        QCOMPARE(exported.frames, expected.frames);
        QCOMPARE(mfSampleCount(reference, DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM)), audioTotal);
        QVERIFY(std::abs(QFileInfo(clip).size() - QFileInfo(reference).size()) < 4096);
    }

    void exportFailsWhenASegmentIsCutShort()
    {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        QVERIFY(SUCCEEDED(com));
        const auto apartment = qScopeGuard([] { CoUninitialize(); });
        QVERIFY(SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_FULL)));
        const auto mf = qScopeGuard([] { MFShutdown(); });
        QTemporaryDir dir(QDir::currentPath() + "/segment-mfcut-XXXXXX");
        QVERIFY(dir.isValid());
        const QString segment = recordSegment(dir.path());
        if (segment.isEmpty())
            QSKIP("No hardware D3D11 device for a recording");
        // Damaged copies whose samples are gone. Nothing can recover them, so
        // the export must report failure instead of succeeding short.
        QFile in(segment);
        QVERIFY(in.open(QIODevice::ReadOnly));
        QByteArray bytes = in.readAll();
        in.close();
        // Walk the top-level boxes: a byte search can match "moof" inside video data.
        qint64 lastMoof = -1;
        for (qint64 off = 0; off + 8 <= bytes.size();) {
            const auto* u = reinterpret_cast<const unsigned char*>(bytes.constData() + off);
            const qint64 size = (qint64(u[0]) << 24) | (qint64(u[1]) << 16) | (qint64(u[2]) << 8) | qint64(u[3]);
            if (size < 8) break;
            if (bytes.mid(off + 4, 4) == "moof") lastMoof = off;
            off += size;
        }
        QVERIFY(lastMoof > 0);
        const qint64 firstMoof = bytes.indexOf("moof") - 4;
        QVERIFY(firstMoof > 0 && firstMoof < lastMoof);
        const auto writeCut = [&](const QString& path, qint64 keep) {
            QFile out(path);
            return out.open(QIODevice::WriteOnly) && out.write(bytes.left(keep)) == keep;
        };
        // 1) The whole last fragment is gone, leaving a consistent index.
        const QString noTail = dir.path() + "/no-tail.mp4";
        QVERIFY(writeCut(noTail, lastMoof));
        std::vector<Fmp4Index::Track> index;
        QVERIFY(Fmp4Index::read(noTail, index));
        // Media Foundation can infer a plausible duration from the remaining
        // fragments. The live save lease carries the original finalized extent.
        const SegmentLease noTailLease({noTail}, {{noTail, bytes.size()}});
        QVERIFY(!noTailLease.filesMatchFinalizedSizes());
        // 2) Cut inside the first fragment's data: no usable index either.
        const QString cutEarly = dir.path() + "/cut-early.mp4";
        QVERIFY(writeCut(cutEarly, firstMoof + 4096));
        QVERIFY(!Fmp4Index::read(cutEarly, index));
        QVERIFY(!ReplayExporter::concat({ cutEarly }, dir.path() + "/cut-early-export.mp4",
                                        QStringLiteral("mf-cut-early"), ReplayExporter::Audio::Any));
    }

    void annexBConversionMatchesTheMp4Source()
    {
        QByteArray sample = QByteArray::fromHex("0000000209300000000361e0aa");
        QVERIFY(Fmp4Index::lengthPrefixedToAnnexB(sample));
        QCOMPARE(sample.toHex(), QByteArray("0000000109300000000161e0aa"));
        QByteArray broken = QByteArray::fromHex("00000009093000");
        QVERIFY(!Fmp4Index::lengthPrefixedToAnnexB(broken));
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

    // A re-arm used to re-probe the whole cache, one MFStartup/MFShutdown per
    // file. Only the newest files can survive trimRing(), so the rest must be
    // pruned through the lease-aware delete without ever being opened.
    void restoreRingProbesOnlyTheNewestWindow()
    {
        QTemporaryDir dir(QDir::currentPath() + "/segment-window-XXXXXX");
        QVERIFY(dir.isValid());

        QStringList all;
        for (int i = 0; i < 10; ++i)
            all << segment(dir.path(), i);

        SegmentRecorder recorder;
        recorder.m_cacheDir = dir.path();
        recorder.m_keepSegments = 2;
        g_restoreProbes = 0;
        recorder.m_segmentProbe = [](const QString&) { ++g_restoreProbes; return true; };

        recorder.restoreRing();

        // keep + 2 candidates are vetted; the six older files never are.
        QCOMPARE(g_restoreProbes, 4);
        // ... and trimRing() then keeps only the configured window.
        QCOMPARE(recorder.m_segments, all.mid(8));
        for (int i = 0; i < 8; ++i)
            QVERIFY2(!QFileInfo::exists(all.at(i)), qPrintable(all.at(i)));
        for (int i = 8; i < 10; ++i)
            QVERIFY2(QFileInfo::exists(all.at(i)), qPrintable(all.at(i)));
    }

    // The prune above must never delete a file an export still leases, even
    // when it sits far outside the window a restore can keep.
    void restoreRingNeverDeletesAnOlderLeasedSegment()
    {
        QTemporaryDir dir(QDir::currentPath() + "/segment-window-lease-XXXXXX");
        QVERIFY(dir.isValid());

        QStringList all;
        for (int i = 0; i < 10; ++i)
            all << segment(dir.path(), i);
        const SegmentLease lease(QStringList() << all.first());

        SegmentRecorder recorder;
        recorder.m_cacheDir = dir.path();
        recorder.m_keepSegments = 2;
        g_restoreProbes = 0;
        recorder.m_segmentProbe = [](const QString&) { ++g_restoreProbes; return true; };

        recorder.restoreRing();

        // The leased file keeps its ring place (an export snapshot still needs
        // it, see replacementRecorderRetainsLeasedStalePaths); only the
        // unleased files outside the window are dropped unprobed.
        QCOMPARE(g_restoreProbes, 5);
        QVERIFY2(QFileInfo::exists(all.first()), qPrintable(all.first()));
        QCOMPARE(recorder.m_segments, (QStringList{all.at(0), all.at(8), all.at(9)}));
        for (int i = 1; i < 6; ++i)
            QVERIFY2(!QFileInfo::exists(all.at(i)), qPrintable(all.at(i)));
    }

    // A sample write that fails mid-recording used to be logged and ignored,
    // so the segment with the hole still went into the next clip. It is now
    // discarded at once and recording continues in a fresh segment.
    void aWriteFailureNeverReachesASavedClip()
    {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        QVERIFY(SUCCEEDED(com));
        const auto apartment = qScopeGuard([] { CoUninitialize(); });
        QTemporaryDir dir(QDir::currentPath() + "/segment-writefail-XXXXXX");
        QVERIFY(dir.isValid());
        VideoFixture video;
        QVERIFY(makeVideo(video));
        SegmentRecorder recorder;
        QVERIFY(recorder.begin(320, 180, 320, 180, 30, 2, 5, 30,
                               dir.path(), video.device.Get(), video.context.Get()));
        const auto finish = qScopeGuard([&] {
            recorder.discardCurrentSegment();
            recorder.end();
        });
        int frame = 0;
        const auto writeFrames = [&](int count) {
            for (int i = 0; i < count; ++i, ++frame)
                recorder.writeFrame(video.texture.Get(), video.device.Get(), video.context.Get(),
                                    10000000LL + frame * 10000000LL / 30);
        };
        writeFrames(180);   // one full 5 s segment closes, the next is in flight
        QVERIFY(recorder.hasClosedMedia());
        const QString poisoned = recorder.m_curPath;
        QVERIFY(!poisoned.isEmpty());
        writeFrames(10);
        recorder.m_injectWriteFailures = 1;
        writeFrames(30);   // the failure, then the forced roll, then recovery
        QVERIFY(recorder.isActive());
        QVERIFY(!QFileInfo::exists(poisoned));

        const SegmentLease clip = recorder.snapshotForSave();
        QVERIFY(!clip.isEmpty());
        QVERIFY(!clip.paths().contains(poisoned));
        for (const QString& path : clip.paths())
            QVERIFY2(ReplayExporter::hasReadableVideo(path), qPrintable(path));
        QVERIFY(ReplayExporter::concat(clip.paths(), dir.path() + "/after-failure.mp4",
                                       QStringLiteral("write-failure")));
    }

    void repeatedWriteFailuresStopTheRecorder()
    {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        QVERIFY(SUCCEEDED(com));
        const auto apartment = qScopeGuard([] { CoUninitialize(); });
        QTemporaryDir dir(QDir::currentPath() + "/segment-writefail-XXXXXX");
        QVERIFY(dir.isValid());
        VideoFixture video;
        QVERIFY(makeVideo(video));
        SegmentRecorder recorder;
        QVERIFY(recorder.begin(320, 180, 320, 180, 30, 2, 5, 30,
                               dir.path(), video.device.Get(), video.context.Get()));
        const auto finish = qScopeGuard([&] { recorder.end(); });
        recorder.m_injectWriteFailures = 1000;
        for (int i = 0; i < 30 && recorder.isActive(); ++i)
            recorder.writeFrame(video.texture.Get(), video.device.Get(), video.context.Get(),
                                10000000LL + i * 10000000LL / 30);
        // Bounded retry: a writer that keeps failing ends as a visible failure
        // (FramePump reports "recorder not active"), never as a silent loop.
        QVERIFY(!recorder.isActive());
        QVERIFY(!recorder.hasClosedMedia());
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
            [entered, resume, bytes](const SegmentLease& snapshot) {
                entered->release();
                resume->acquire();
                for (const QString& path : snapshot.paths()) {
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
        ReplayExportTask task(SegmentLease({path}), [throws](const SegmentLease&) {
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
            [entered, resume, completed](const SegmentLease&) {
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
