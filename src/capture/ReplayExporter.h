#pragma once

#include <QString>
#include <QStringList>

class QImage;

// 0.5 Step 8 — turn buffered replay segments into a final saved clip.
//
// concat(): remux-concat H.264 fragmented-MP4 segments (in chronological order)
// into ONE standard MP4, copying the compressed samples with stitched timestamps
// — NO re-encode. All inputs must share identical encode params (our SegmentRecorder
// guarantees this) and start on an IDR keyframe.
//
// grabThumbnail(): decode the first frame of an MP4 to a BGRA QImage for a gallery
// video thumbnail.
//
// Both use Media Foundation and must run on an MTA thread (the frame-pump worker).
namespace ReplayExporter
{
    // Audio presence must match across all inputs in either case; Required also
    // refuses a clip whose segments carry no audio track or no audio samples.
    enum class Audio { Any, Required };

    bool concat(const QStringList& inputFiles, const QString& outputFile,
                const QString& traceId = QString(), Audio audio = Audio::Any);
    bool grabThumbnail(const QString& file, QImage& out);
}
