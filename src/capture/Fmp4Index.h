#pragma once

#include <QByteArray>
#include <QString>

#include <vector>

// Sample index of a fragmented MP4 replay segment, read straight from the
// boxes (moov/trak/mdhd/hdlr/trex, moof/traf/tfhd/tfdt/trun) without Media
// Foundation. mdat payloads are skipped, so indexing costs a few kilobytes of
// reads per segment.
//
// Why: Windows' MPEG-4 source occasionally stops at a fragment that is
// perfectly valid (it depends only on where the fragment sits in the file;
// moving the same bytes 160 bytes later reads fine), losing the end of a
// segment. The exporter compares what Media Foundation returned with this
// index and copies any missing samples from the file itself (#39).
namespace Fmp4Index {

struct Sample {
    qint64 offset = 0;     // file offset of the payload
    quint32 size = 0;
    qint64 time = 0;       // presentation time, 100-ns units from the track start
    qint64 duration = 0;   // 100-ns units
    bool sync = false;
};

struct Track {
    quint32 id = 0;
    quint32 timescale = 0;
    bool video = false;
    bool audio = false;
    std::vector<Sample> samples;   // decode order
};

// False (with why) when the file is not a well-formed fragmented MP4 or an
// offset/size points outside the file.
bool read(const QString& path, std::vector<Track>& tracks, QString* why = nullptr);

// Payload bytes of one sample.
bool readPayload(const QString& path, const Sample& sample, QByteArray& out);

// Rewrite length-prefixed (4-byte) H.264 NAL units as Annex-B start codes,
// the form Media Foundation's MP4 source hands out. False if the lengths do
// not add up to the sample size.
bool lengthPrefixedToAnnexB(QByteArray& sample);

} // namespace Fmp4Index
