#include "capture/ReplayExporter.h"
#include "capture/Fmp4Index.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <mfobjects.h>   // IMF2DBuffer

#include <QImage>
#include <QDebug>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QScopeGuard>

#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

// Copy the compressed H.264 samples of N MP4 segments into one MP4, stitching
// timestamps so playback is continuous. No decode, no encode. Every input must
// share identical encode params and start on an IDR keyframe.
QString fromWide(const std::wstring& value)
{
    return QString::fromStdWString(value);
}

// One MP4 segment opened for compressed passthrough.
struct SegmentSource {
    IMFSourceReader* reader    = nullptr;
    IMFMediaType*    videoType = nullptr;   // native (compressed) video type
    IMFMediaType*    audioType = nullptr;   // native audio type, null if none
    bool             hasAudio  = false;

    void release()
    {
        if (videoType) videoType->Release();
        if (audioType) audioType->Release();
        if (reader)    reader->Release();
        videoType = nullptr;
        audioType = nullptr;
        reader    = nullptr;
    }
};

// The single output sink shared by all segments, created from the first usable one.
struct ConcatWriter {
    IMFSinkWriter* writer      = nullptr;
    DWORD          videoStream = 0;
    DWORD          audioStream = 0;
    bool           ready       = false;     // BeginWriting succeeded
    bool           haveAudio   = false;
    LONGLONG       frameDurGuess = 333667;  // ~30fps fallback if a sample lacks duration
};

// Open one segment. Do NOT enable advanced video processing => reader keeps the
// native (compressed) type => hands back undecoded H.264 samples. False =
// segment unusable (already logged); the caller skips it and keeps scanning so
// all bad inputs are logged.
bool openSegment(const std::wstring& path, int segmentIndex, const QString& tag,
                 SegmentSource& src)
{
    const QString qPath = fromWide(path);
    const QFileInfo fi(qPath);
    qInfo().noquote() << QStringLiteral("ReplaySave[%1]: segment %2 open path=%3 exists=%4 bytes=%5")
                             .arg(tag).arg(segmentIndex).arg(qPath)
                             .arg(fi.exists() ? QStringLiteral("yes") : QStringLiteral("no"))
                             .arg(fi.exists() ? fi.size() : -1);

    const auto fail = [&](const char* stage, HRESULT error) {
        qWarning().nospace() << "ReplaySave[" << tag
                             << "]: segment " << segmentIndex
                             << " rejected - " << stage << " hr=0x"
                             << Qt::hex << quint32(error);
        src.release();
        return false;
    };

    IMFAttributes* rattr = nullptr;
    HRESULT hr = MFCreateAttributes(&rattr, 1);
    if (FAILED(hr))
        return fail("reader attributes", hr);
    rattr->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, FALSE);

    hr = MFCreateSourceReaderFromURL(path.c_str(), rattr, &src.reader);
    if (rattr) rattr->Release();
    if (FAILED(hr) || !src.reader) {
        qWarning().nospace() << "ReplaySave[" << tag
                             << "]: segment " << segmentIndex
                             << " skipped - open failed hr=0x"
                             << Qt::hex << quint32(hr);
        src.release();
        return false;
    }

    hr = src.reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (SUCCEEDED(hr))
        hr = src.reader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    if (FAILED(hr))
        return fail("video stream selection", hr);

    // Pin the native compressed type => no decoder inserted (passthrough).
    hr = src.reader->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &src.videoType);
    if (FAILED(hr) || !src.videoType) {
        qWarning() << "ReplayExporter: skipping segment — no native video type";
        src.release();
        return false;
    }
    hr = src.reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, src.videoType);
    if (FAILED(hr))
        return fail("compressed video type", hr);

    hr = src.reader->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &src.audioType);
    // Only an absent stream means video-only. A damaged audio type must not
    // silently turn a clip with audio into a successful silent export.
    if (hr != MF_E_INVALIDSTREAMNUMBER) {
        if (FAILED(hr) || !src.audioType)
            return fail("native audio type", FAILED(hr) ? hr : E_UNEXPECTED);
        src.hasAudio = true;
        hr = src.reader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
        if (SUCCEEDED(hr))
            hr = src.reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, src.audioType);
        if (FAILED(hr))
            return fail("compressed audio setup", hr);
    }

    UINT32 segW = 0, segH = 0, fpsNum = 0, fpsDen = 0;
    MFGetAttributeSize(src.videoType, MF_MT_FRAME_SIZE, &segW, &segH);
    MFGetAttributeRatio(src.videoType, MF_MT_FRAME_RATE, &fpsNum, &fpsDen);
    qInfo().noquote() << QStringLiteral("ReplaySave[%1]: segment %2 media video=%3x%4 fps=%5/%6 audio=%7")
                             .arg(tag).arg(segmentIndex)
                             .arg(segW).arg(segH).arg(fpsNum).arg(fpsDen)
                             .arg(src.hasAudio ? QStringLiteral("yes") : QStringLiteral("no"));
    return true;
}

// Create the sink writer from the first usable segment. Clone the FULL native
// type so MF_MT_MPEG_SEQUENCE_HEADER (SPS/PPS), frame size, PAR, etc. reach the
// output avcC box. A hand-built minimal type would yield an unplayable file.
// On failure w.writer may be non-null but w.ready stays false; the caller
// releases it in its normal teardown.
bool beginConcatWriter(const std::wstring& outFile, const SegmentSource& src, ConcatWriter& w)
{
    IMFAttributes* wattr = nullptr;
    HRESULT hr = MFCreateAttributes(&wattr, 2);
    if (FAILED(hr)) return false;
    wattr->SetGUID  (MF_TRANSCODE_CONTAINERTYPE,        MFTranscodeContainerType_MPEG4);
    wattr->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);

    hr = MFCreateSinkWriterFromURL(outFile.c_str(), nullptr, wattr, &w.writer);
    if (wattr) wattr->Release();
    if (FAILED(hr) || !w.writer) return false;

    IMFMediaType* outType = nullptr;
    hr = MFCreateMediaType(&outType);
    if (FAILED(hr)) return false;
    hr = src.videoType->CopyAllItems(outType);

    if (SUCCEEDED(hr))
        hr = w.writer->AddStream(outType, &w.videoStream);
    if (SUCCEEDED(hr))
        hr = w.writer->SetInputMediaType(w.videoStream, outType, nullptr); // same type => no encoder
    outType->Release();
    if (FAILED(hr)) return false;

    if (src.hasAudio) {
        IMFMediaType* outAudioType = nullptr;
        hr = MFCreateMediaType(&outAudioType);
        if (SUCCEEDED(hr))
            hr = src.audioType->CopyAllItems(outAudioType);
        if (SUCCEEDED(hr))
            hr = w.writer->AddStream(outAudioType, &w.audioStream);
        if (SUCCEEDED(hr))
            hr = w.writer->SetInputMediaType(w.audioStream, outAudioType, nullptr);
        if (outAudioType) outAudioType->Release();
        if (FAILED(hr)) {
            qWarning().nospace() << "ReplayExporter: audio stream setup failed hr=0x"
                                 << Qt::hex << quint32(hr);
            return false;
        }
        w.haveAudio = true;
    }

    UINT32 num = 0, den = 0;
    if (SUCCEEDED(MFGetAttributeRatio(src.videoType, MF_MT_FRAME_RATE, &num, &den)) && num)
        w.frameDurGuess = (LONGLONG)(10000000.0 * den / num);

    hr = w.writer->BeginWriting();
    if (FAILED(hr)) return false;
    w.ready = true;
    return true;
}

enum class CopyStatus { Ok, ReadFailed, WriteFailed };
constexpr LONGLONG kSegmentDurationTolerance100ns = 500000; // 50 ms

// Copy every compressed sample of one stream into the output, shifting sample
// times by `offset` so the timeline stays continuous. The first sample of each
// segment is flagged as a discontinuity (each seg starts with IDR).
CopyStatus copySamples(IMFSourceReader* reader, DWORD srcStream, IMFSinkWriter* writer,
                       DWORD dstStream, LONGLONG offset, LONGLONG fallbackDur,
                       const char* what, LONGLONG& segMaxEnd, int& samples,
                       LONGLONG& lastTs, DWORD& lastLen)
{
    bool firstOfSeg = true;
    for (;;) {
        DWORD      streamFlags = 0;
        LONGLONG   ts          = 0;
        IMFSample* sample      = nullptr;
        HRESULT hr = reader->ReadSample(srcStream, 0, nullptr, &streamFlags, &ts, &sample);
        if (FAILED(hr) || (streamFlags & MF_SOURCE_READERF_ERROR)) {
            qWarning().nospace() << "ReplayExporter: " << what << " read failed hr=0x"
                                 << Qt::hex << quint32(hr);
            if (sample) sample->Release();
            return CopyStatus::ReadFailed;
        }
        if (streamFlags & MF_SOURCE_READERF_ENDOFSTREAM) { if (sample) sample->Release(); return CopyStatus::Ok; }
        if (!sample) continue;   // stream tick / gap, no payload

        LONGLONG dur = 0;
        if (FAILED(sample->GetSampleDuration(&dur)) || dur <= 0)
            dur = fallbackDur;

        sample->SetSampleTime(ts + offset);
        if (firstOfSeg) {
            sample->SetUINT32(MFSampleExtension_Discontinuity, TRUE);
            firstOfSeg = false;
        }
        hr = writer->WriteSample(dstStream, sample);
        if (FAILED(hr)) {
            qWarning().nospace() << "ReplayExporter: " << what << " write failed hr=0x"
                                 << Qt::hex << quint32(hr);
            sample->Release();
            return CopyStatus::WriteFailed;
        }

        const LONGLONG end = ts + dur;
        if (end > segMaxEnd) segMaxEnd = end;
        lastTs = ts;
        lastLen = 0;
        sample->GetTotalLength(&lastLen);
        sample->Release();
        ++samples;
    }
}

// Media Foundation's MPEG-4 source sometimes stops at a valid fragment and
// reports end of stream early (it depends only on the fragment's position in
// the file). Compare what it returned with the segment's own sample index and
// copy any missing samples straight from the file, unchanged, in the form the
// source would have produced (#39). Nothing is ever invented: a sample that
// cannot be read, or an index that does not line up with what Media
// Foundation returned, makes the segment incomplete and the export fails.
CopyStatus recoverTail(const QString& path, const Fmp4Index::Track& track, bool video,
                       IMFMediaType* nativeType, int readCount, LONGLONG lastTs, DWORD lastLen,
                       IMFSinkWriter* writer, DWORD dstStream, LONGLONG offset,
                       LONGLONG& segMaxEnd, int& samples, const QString& tag, int segmentIndex)
{
    const int total = int(track.samples.size());
    if (readCount >= total)
        return CopyStatus::Ok;
    const char* what = video ? "video" : "audio";
    QByteArray sequenceHeader;
    if (video && nativeType) {
        UINT32 len = 0;
        if (SUCCEEDED(nativeType->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &len)) && len) {
            sequenceHeader.resize(int(len));
            nativeType->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER,
                                reinterpret_cast<UINT8*>(sequenceHeader.data()), len, &len);
        }
    }
    const auto expectedLength = [&](const Fmp4Index::Sample& s) {
        return DWORD(s.size) + DWORD(video && s.sync ? sequenceHeader.size() : 0);
    };
    // Anchor on the last sample Media Foundation delivered: it must be the
    // index entry at the same position, or the two views disagree.
    LONGLONG anchorTs = 0, anchorIndexTime = 0;
    if (readCount > 0) {
        const Fmp4Index::Sample& last = track.samples[size_t(readCount - 1)];
        if (lastLen != expectedLength(last)) {
            qWarning().noquote() << QStringLiteral(
                "ReplaySave[%1]: segment %2 %3 ended after %4 of %5 samples and the file index does "
                "not match the last one read (%6 vs %7 bytes) - segment incomplete")
                .arg(tag).arg(segmentIndex).arg(QLatin1String(what)).arg(readCount).arg(total)
                .arg(lastLen).arg(expectedLength(last));
            return CopyStatus::ReadFailed;
        }
        anchorTs = lastTs;
        anchorIndexTime = last.time;
    }
    for (int i = readCount; i < total; ++i) {
        const Fmp4Index::Sample& s = track.samples[size_t(i)];
        QByteArray payload;
        if (!Fmp4Index::readPayload(path, s, payload)
            || (video && !Fmp4Index::lengthPrefixedToAnnexB(payload))) {
            qWarning().noquote() << QStringLiteral(
                "ReplaySave[%1]: segment %2 %3 sample %4 of %5 could not be recovered from the file - "
                "segment incomplete")
                .arg(tag).arg(segmentIndex).arg(QLatin1String(what)).arg(i + 1).arg(total);
            return CopyStatus::ReadFailed;
        }
        if (video && s.sync)
            payload.prepend(sequenceHeader);   // as the MP4 source does for IDR frames
        IMFMediaBuffer* buffer = nullptr;
        IMFSample* sample = nullptr;
        BYTE* dst = nullptr;
        HRESULT hr = MFCreateMemoryBuffer(DWORD(payload.size()), &buffer);
        if (SUCCEEDED(hr)) hr = buffer->Lock(&dst, nullptr, nullptr);
        if (SUCCEEDED(hr)) {
            memcpy(dst, payload.constData(), size_t(payload.size()));
            buffer->Unlock();
            buffer->SetCurrentLength(DWORD(payload.size()));
            hr = MFCreateSample(&sample);
        }
        if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer);
        if (buffer) buffer->Release();
        if (FAILED(hr)) {
            if (sample) sample->Release();
            return CopyStatus::ReadFailed;
        }
        // Keep the file's own spacing, continued from the last delivered sample.
        const LONGLONG ts = anchorTs + (s.time - anchorIndexTime);
        sample->SetSampleTime(ts + offset);
        sample->SetSampleDuration(s.duration);
        if (s.sync)
            sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
        if (readCount == 0 && i == 0)
            sample->SetUINT32(MFSampleExtension_Discontinuity, TRUE);
        hr = writer->WriteSample(dstStream, sample);
        sample->Release();
        if (FAILED(hr)) {
            qWarning().nospace() << "ReplayExporter: recovered " << what << " write failed hr=0x"
                                 << Qt::hex << quint32(hr);
            return CopyStatus::WriteFailed;
        }
        if (ts + s.duration > segMaxEnd) segMaxEnd = ts + s.duration;
        ++samples;
    }
    qWarning().noquote() << QStringLiteral(
        "ReplaySave[%1]: segment %2 %3: Media Foundation stopped after %4 of %5 samples; "
        "recovered the remaining %6 from the file")
        .arg(tag).arg(segmentIndex).arg(QLatin1String(what)).arg(readCount).arg(total).arg(total - readCount);
    return CopyStatus::Ok;
}

const Fmp4Index::Track* findTrack(const std::vector<Fmp4Index::Track>& tracks, bool video)
{
    for (const Fmp4Index::Track& t : tracks)
        if (video ? t.video : t.audio) return &t;
    return nullptr;
}

bool remuxConcatImpl(const std::vector<std::wstring>& inFiles, const std::wstring& outFile,
                     const QString& traceId, bool requireAudio)
{
    QElapsedTimer totalTimer;
    totalTimer.start();
    const QString tag = traceId.isEmpty() ? QStringLiteral("no-id") : traceId;

    if (inFiles.empty()) {
        qWarning().noquote() << QStringLiteral("ReplaySave[%1]: concat refused - no input segments")
                                    .arg(tag);
        return false;
    }

    const bool startedMf = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    qInfo().noquote() << QStringLiteral("ReplaySave[%1]: concat begin inputs=%2 output=%3 mfStarted=%4")
                             .arg(tag).arg(inFiles.size()).arg(fromWide(outFile))
                             .arg(startedMf ? QStringLiteral("yes") : QStringLiteral("no"));

    ConcatWriter w;
    LONGLONG offset         = 0;      // cumulative 100-ns offset across segments
    bool     wroteAnything  = false;
    bool     skippedSegment = false;  // a bad input makes the whole export fail:
                                      // a partial 10-second clip is worse than a
                                      // clear failure when the configured ring
                                      // should contain 30+ seconds.
    bool     writeFailed    = false;
    int      totalAudioSamples = 0;

    int segmentIndex = 0;
    for (const std::wstring& path : inFiles) {
        QElapsedTimer segTimer;
        segTimer.start();

        SegmentSource src;
        if (!openSegment(path, segmentIndex, tag, src)) {
            skippedSegment = true;
            ++segmentIndex;
            continue;
        }
        if (!w.ready && !beginConcatWriter(outFile, src, w)) {
            src.release();
            break;
        }
        // The first segment fixes the output's streams. Any later segment
        // with a different audio presence (either direction) would leave a
        // silent stretch or drop audio, so the whole export is refused.
        if (src.hasAudio != w.haveAudio || (requireAudio && !src.hasAudio)) {
            qWarning().noquote() << QStringLiteral("ReplaySave[%1]: segment %2 audio=%3 but output audio=%4%5 - refusing mixed or silent clip")
                                        .arg(tag).arg(segmentIndex)
                                        .arg(src.hasAudio ? QStringLiteral("yes") : QStringLiteral("no"))
                                        .arg(w.haveAudio ? QStringLiteral("yes") : QStringLiteral("no"))
                                        .arg(requireAudio ? QStringLiteral(" (required)") : QString());
            writeFailed = true;
            src.release();
            break;
        }

        LONGLONG segMaxEnd   = 0;
        int      videoSamples = 0;
        int      audioSamples = 0;
        // The segment's own sample index: the exact count Media Foundation
        // must return, and the source for anything it fails to return.
        const QString qPath = fromWide(path);
        std::vector<Fmp4Index::Track> index;
        QString indexWhy;
        const bool indexed = Fmp4Index::read(qPath, index, &indexWhy);
        if (!indexed)
            qWarning().noquote() << QStringLiteral("ReplaySave[%1]: segment %2 has no usable sample index (%3) - "
                                                   "checking its length against the file duration instead")
                                        .arg(tag).arg(segmentIndex).arg(indexWhy);
        const Fmp4Index::Track* videoIndex = indexed ? findTrack(index, true) : nullptr;
        const Fmp4Index::Track* audioIndex = indexed ? findTrack(index, false) : nullptr;
        LONGLONG lastTs = 0;
        DWORD lastLen = 0;
        CopyStatus vs = copySamples(src.reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                    w.writer, w.videoStream, offset, w.frameDurGuess,
                                    "video", segMaxEnd, videoSamples, lastTs, lastLen);
        if (vs == CopyStatus::Ok && videoIndex)
            vs = recoverTail(qPath, *videoIndex, true, src.videoType, videoSamples, lastTs, lastLen,
                             w.writer, w.videoStream, offset, segMaxEnd, videoSamples, tag, segmentIndex);
        if (vs == CopyStatus::ReadFailed)
            skippedSegment = true;
        else if (vs == CopyStatus::WriteFailed)
            writeFailed = true;
        if (videoSamples > 0)
            wroteAnything = true;

        if (!writeFailed && w.haveAudio && src.hasAudio) {
            lastTs = 0;
            lastLen = 0;
            CopyStatus as = copySamples(src.reader, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                                        w.writer, w.audioStream, offset,
                                        213333 /* ~1024 samples @ 48kHz fallback */,
                                        "audio", segMaxEnd, audioSamples, lastTs, lastLen);
            if (as == CopyStatus::Ok && audioIndex)
                as = recoverTail(qPath, *audioIndex, false, src.audioType, audioSamples, lastTs, lastLen,
                                 w.writer, w.audioStream, offset, segMaxEnd, audioSamples, tag, segmentIndex);
            if (as == CopyStatus::ReadFailed)
                skippedSegment = true;
            else if (as != CopyStatus::Ok)
                writeFailed = true;
        }
        // A finalized segment's duration is where its last sample ends. Ending
        // earlier means samples are missing that neither Media Foundation nor
        // the index could supply (a truncated file loses whole fragments and
        // its index shrinks with it): report it instead of exporting less.
        if (!writeFailed) {
            PROPVARIANT duration;
            PropVariantInit(&duration);
            if (SUCCEEDED(src.reader->GetPresentationAttribute(DWORD(MF_SOURCE_READER_MEDIASOURCE),
                                                               MF_PD_DURATION, &duration))
                && duration.vt == VT_UI8
                && LONGLONG(duration.uhVal.QuadPart) > segMaxEnd + kSegmentDurationTolerance100ns) {
                qWarning().noquote() << QStringLiteral("ReplaySave[%1]: segment %2 read %3 ms of %4 ms - segment incomplete")
                                            .arg(tag).arg(segmentIndex).arg(segMaxEnd / 10000)
                                            .arg(LONGLONG(duration.uhVal.QuadPart) / 10000);
                skippedSegment = true;
            }
            PropVariantClear(&duration);
        }
        totalAudioSamples += audioSamples;

        if (videoSamples == 0) {
            skippedSegment = true;
            qWarning().noquote() << QStringLiteral("ReplaySave[%1]: segment %2 wrote no video samples")
                                        .arg(tag).arg(segmentIndex);
        }
        qInfo().noquote() << QStringLiteral("ReplaySave[%1]: segment %2 done videoSamples=%3 audioSamples=%4 durationMs=%5 elapsedMs=%6")
                                 .arg(tag).arg(segmentIndex)
                                 .arg(videoSamples).arg(audioSamples)
                                 .arg(segMaxEnd / 10000).arg(segTimer.elapsed());
        offset += segMaxEnd;   // next segment picks up where this one ended
        src.release();
        if (writeFailed)
            break;
        ++segmentIndex;
    }

    HRESULT hr = S_OK;
    if (w.writer && w.ready && wroteAnything)
        hr = w.writer->Finalize();             // writes moov -> playable file
    else
        hr = E_FAIL;

    if (w.writer) w.writer->Release();
    if (startedMf) MFShutdown();
    const bool audioMissing = requireAudio && totalAudioSamples == 0;
    if (audioMissing)
        qWarning().noquote() << QStringLiteral("ReplaySave[%1]: audio was required but no audio samples were exported")
                                    .arg(tag);
    const bool ok = SUCCEEDED(hr) && wroteAnything && !skippedSegment && !writeFailed
                 && !audioMissing;
    qInfo().noquote() << QStringLiteral("ReplaySave[%1]: concat end ok=%2 wroteAnything=%3 skipped=%4 writeFailed=%5 finalHr=0x%6 durationMs=%7 totalMs=%8")
                             .arg(tag)
                             .arg(ok ? QStringLiteral("yes") : QStringLiteral("no"))
                             .arg(wroteAnything ? QStringLiteral("yes") : QStringLiteral("no"))
                             .arg(skippedSegment ? QStringLiteral("yes") : QStringLiteral("no"))
                             .arg(writeFailed ? QStringLiteral("yes") : QStringLiteral("no"))
                             .arg(QString::number(quint32(hr), 16))
                             .arg(offset / 10000)
                             .arg(totalTimer.elapsed());
    return ok;
}

// Core probe: opens the segment through Media Foundation and looks for one
// video sample. The CALLER must already have COM (MTA) and MF started on this
// thread - a source reader cannot be created otherwise.
bool probeReadableVideo(const std::wstring& path)
{
    if (QFileInfo(fromWide(path)).size() <= 0)
        return false;   // the empty fMP4 a crash leaves behind
    bool readable = false;
    SegmentSource src;
    if (openSegment(path, 0, QStringLiteral("restore-check"), src)) {
        // The first reads can be stream ticks or format changes without a
        // sample; a few attempts are enough to reach a frame or the end.
        for (int attempt = 0; attempt < 8 && !readable; ++attempt) {
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            IMFSample* sample = nullptr;
            const HRESULT hr = src.reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                                      0, nullptr, &flags, &timestamp, &sample);
            if (sample) {
                readable = SUCCEEDED(hr);
                sample->Release();
            }
            if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)))
                break;
        }
        src.release();
    }
    return readable;
}

// Standalone probe: brings COM and MF up for the call and tears them down again.
bool hasReadableVideoImpl(const std::wstring& path)
{
    const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool startedMf = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    const bool readable = probeReadableVideo(path);
    if (startedMf)
        MFShutdown();
    if (SUCCEEDED(hrCo))
        CoUninitialize();
    return readable;
}

bool copyRgb32Rows(QImage& out, const BYTE* buffer, DWORD length, qint64 firstRow, qint64 pitch,
                   UINT32 width, UINT32 height)
{
    const qint64 rowBytes = qint64(width) * 4;
    const qint64 lastRow = firstRow + pitch * (height - 1);
    if (!buffer || qAbs(pitch) < rowBytes || qMin(firstRow, lastRow) < 0
        || qMax(firstRow, lastRow) > qint64(length) - rowBytes)
        return false;

    QImage image(int(width), int(height), QImage::Format_RGB32);
    if (image.isNull())
        return false;
    for (UINT32 y = 0; y < height; ++y)
        std::memcpy(image.scanLine(int(y)), buffer + firstRow + pitch * y, size_t(rowBytes));
    out = std::move(image);
    return true;
}

bool copyRgb32Frame(IMFMediaBuffer* media, UINT32 width, UINT32 height, LONG defaultStride, QImage& out)
{
    // The packed media length does not describe the padding in a 2-D lock.
    IMF2DBuffer2* surface = nullptr;
    if (SUCCEEDED(media->QueryInterface(IID_IMF2DBuffer2, reinterpret_cast<void**>(&surface)))) {
        const auto releaseSurface = qScopeGuard([surface] { surface->Release(); });
        BYTE* scan0 = nullptr;
        BYTE* buffer = nullptr;
        LONG pitch = 0;
        DWORD length = 0;
        if (SUCCEEDED(surface->Lock2DSize(MF2DBuffer_LockFlags_Read, &scan0, &pitch, &buffer, &length))) {
            const auto unlockSurface = qScopeGuard([surface] { surface->Unlock2D(); });
            const auto rowStart = reinterpret_cast<quintptr>(scan0);
            const auto bufferStart = reinterpret_cast<quintptr>(buffer);
            return scan0 && rowStart >= bufferStart && rowStart - bufferStart <= length
                && copyRgb32Rows(out, buffer, length, qint64(rowStart - bufferStart), pitch, width, height);
        }
    }

    // MediaBuffer::Lock gives a contiguous view even on older 2-D buffers.
    BYTE* data = nullptr;
    DWORD length = 0;
    if (FAILED(media->Lock(&data, nullptr, &length)))
        return false;
    const auto unlockMedia = qScopeGuard([media] { media->Unlock(); });
    const qint64 pitch = defaultStride ? defaultStride : qint64(width) * 4;
    const qint64 firstRow = pitch < 0 ? -pitch * (height - 1) : 0;
    return copyRgb32Rows(out, data, length, firstRow, pitch, width, height);
}

// Decode the first frame of an MP4 to RGB32 (== BGRA == QImage::Format_RGB32).
bool grabThumbnailImpl(const std::wstring& file, QImage& out)
{
    const bool startedMf = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));

    // The scanner feeds this any video file in a watched folder, so every
    // step is checked and nothing trusts the file's own sizes.
    IMFAttributes* rattr = nullptr;
    if (FAILED(MFCreateAttributes(&rattr, 1)) || !rattr) {
        if (startedMf) MFShutdown();
        return false;
    }
    rattr->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);

    IMFSourceReader* reader = nullptr;
    HRESULT hr = MFCreateSourceReaderFromURL(file.c_str(), rattr, &reader);
    if (rattr) rattr->Release();
    if (FAILED(hr) || !reader) { if (startedMf) MFShutdown(); return false; }

    reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS,        FALSE);
    reader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);

    IMFMediaType* want = nullptr;
    if (FAILED(MFCreateMediaType(&want)) || !want) {
        reader->Release();
        if (startedMf) MFShutdown();
        return false;
    }
    want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    want->SetGUID(MF_MT_SUBTYPE,    MFVideoFormat_RGB32);
    hr = reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, want);
    want->Release();
    if (FAILED(hr)) { reader->Release(); if (startedMf) MFShutdown(); return false; }

    UINT32 width = 0, height = 0;
    LONG   defStride = 0;
    IMFMediaType* cur = nullptr;
    if (SUCCEEDED(reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur))) {
        MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &width, &height);
        UINT32 s = 0;
        if (SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &s)))
            defStride = (LONG)s;
        else
            MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1, width, &defStride);
        cur->Release();
    }
    constexpr UINT32 kMaxDimension = 16384;
    if (width == 0 || height == 0 || width > kMaxDimension || height > kMaxDimension) {
        reader->Release();
        if (startedMf) MFShutdown();
        return false;
    }

    // A stream of empty reads (format changes, gaps) must not spin forever.
    IMFSample* sample = nullptr;
    for (int attempt = 0; attempt < 64; ++attempt) {
        DWORD    flags = 0;
        LONGLONG ts    = 0;
        hr = reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                0, nullptr, &flags, &ts, &sample);
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) break;
        if (sample) break;                 // got a decoded frame
    }
    if (!sample) { reader->Release(); if (startedMf) MFShutdown(); return false; }

    IMFMediaBuffer* buf = nullptr;
    hr = sample->ConvertToContiguousBuffer(&buf);   // flattens multi-buffer samples
    if (FAILED(hr) || !buf) { sample->Release(); reader->Release(); if (startedMf) MFShutdown(); return false; }

    const bool okResult = copyRgb32Frame(buf, width, height, defStride, out);

    buf->Release();
    sample->Release();
    reader->Release();
    if (startedMf) MFShutdown();
    return okResult;
}

} // namespace

namespace ReplayExporter {

bool concat(const QStringList& inputFiles, const QString& outputFile, const QString& traceId,
            Audio audio)
{
    std::vector<std::wstring> in;
    in.reserve(inputFiles.size());
    for (const QString& f : inputFiles)
        in.push_back(f.toStdWString());
    return remuxConcatImpl(in, outputFile.toStdWString(), traceId, audio == Audio::Required);
}

bool grabThumbnail(const QString& file, QImage& out)
{
    return grabThumbnailImpl(file.toStdWString(), out);
}

bool hasReadableVideo(const QString& file)
{
    return hasReadableVideoImpl(file.toStdWString());
}

bool hasReadableVideoStarted(const QString& file)
{
    return probeReadableVideo(file.toStdWString());
}

} // namespace ReplayExporter
