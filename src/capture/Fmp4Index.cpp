#include "capture/Fmp4Index.h"

#include <QFile>

#include <map>

namespace Fmp4Index {
namespace {

quint32 be32(const char* p)
{
    const auto* u = reinterpret_cast<const unsigned char*>(p);
    return (quint32(u[0]) << 24) | (quint32(u[1]) << 16) | (quint32(u[2]) << 8) | quint32(u[3]);
}
quint64 be64(const char* p)
{
    return (quint64(be32(p)) << 32) | be32(p + 4);
}

// One box inside a buffer: [start, end) is the payload after the header.
struct Box {
    QByteArray type;
    qint64 start = 0;
    qint64 end = 0;
};

// Children of a box payload held in memory.
bool children(const QByteArray& buf, qint64 begin, qint64 end, std::vector<Box>& out)
{
    qint64 off = begin;
    while (off + 8 <= end) {
        quint64 size = be32(buf.constData() + off);
        qint64 header = 8;
        if (size == 1) {
            if (off + 16 > end) return false;
            size = be64(buf.constData() + off + 8);
            header = 16;
        } else if (size == 0) {
            size = quint64(end - off);
        }
        if (size < quint64(header) || off + qint64(size) > end) return false;
        out.push_back({ buf.mid(off + 4, 4), off + header, off + qint64(size) });
        off += qint64(size);
    }
    return off == end;
}

const Box* find(const std::vector<Box>& boxes, const char* type)
{
    for (const Box& b : boxes)
        if (b.type == type) return &b;
    return nullptr;
}

qint64 to100ns(quint64 value, quint32 timescale)
{
    // Split to avoid overflow on long tracks: value * 1e7 / timescale.
    const quint64 whole = value / timescale, rest = value % timescale;
    return qint64(whole * 10000000ull + rest * 10000000ull / timescale);
}

struct TrackDefaults { quint32 duration = 0, size = 0, flags = 0; };

bool parseMoov(const QByteArray& moov, std::vector<Track>& tracks,
               std::map<quint32, TrackDefaults>& defaults)
{
    std::vector<Box> top;
    if (!children(moov, 0, moov.size(), top)) return false;
    for (const Box& trak : top) {
        if (trak.type == "mvex") {
            std::vector<Box> mv;
            if (!children(moov, trak.start, trak.end, mv)) return false;
            for (const Box& trex : mv) {
                if (trex.type != "trex" || trex.end - trex.start < 24) continue;
                const char* p = moov.constData() + trex.start;
                TrackDefaults& d = defaults[be32(p + 4)];
                d.duration = be32(p + 12);
                d.size = be32(p + 16);
                d.flags = be32(p + 20);
            }
            continue;
        }
        if (trak.type != "trak") continue;
        std::vector<Box> tk;
        if (!children(moov, trak.start, trak.end, tk)) return false;
        Track t;
        if (const Box* tkhd = find(tk, "tkhd")) {
            const char* p = moov.constData() + tkhd->start;
            t.id = be32(p + (p[0] == 1 ? 20 : 12));
        }
        const Box* mdia = find(tk, "mdia");
        if (!mdia) return false;
        std::vector<Box> md;
        if (!children(moov, mdia->start, mdia->end, md)) return false;
        if (const Box* mdhd = find(md, "mdhd")) {
            const char* p = moov.constData() + mdhd->start;
            t.timescale = be32(p + (p[0] == 1 ? 20 : 12));
        }
        if (const Box* hdlr = find(md, "hdlr")) {
            const QByteArray h = moov.mid(hdlr->start + 8, 4);
            t.video = h == "vide";
            t.audio = h == "soun";
        }
        if (!t.id || !t.timescale) return false;
        tracks.push_back(std::move(t));
    }
    return !tracks.empty();
}

} // namespace

bool read(const QString& path, std::vector<Track>& tracks, QString* why)
{
    const auto fail = [&](const QString& reason) {
        if (why) *why = reason;
        tracks.clear();
        return false;
    };
    tracks.clear();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("cannot open"));
    const qint64 fileSize = f.size();
    std::map<quint32, TrackDefaults> defaults;
    std::map<quint32, quint64> decodeTime;   // running decode time per track (track units)
    bool haveMoov = false, haveMoof = false;
    qint64 off = 0;
    while (off + 8 <= fileSize) {
        if (!f.seek(off)) return fail(QStringLiteral("seek failed"));
        const QByteArray hdr = f.read(16);
        if (hdr.size() < 8) return fail(QStringLiteral("short box header"));
        quint64 size = be32(hdr.constData());
        qint64 header = 8;
        if (size == 1) {
            if (hdr.size() < 16) return fail(QStringLiteral("short large box header"));
            size = be64(hdr.constData() + 8);
            header = 16;
        } else if (size == 0) {
            size = quint64(fileSize - off);
        }
        if (size < quint64(header) || off + qint64(size) > fileSize)
            return fail(QStringLiteral("box at %1 runs past the end of the file").arg(off));
        const QByteArray type = hdr.mid(4, 4);
        // Only metadata boxes are loaded; a moof is a few hundred bytes.
        if (type == "moov" || type == "moof") {
            if (size > 16u * 1024u * 1024u) return fail(QStringLiteral("oversized %1").arg(QString::fromLatin1(type)));
            if (!f.seek(off + header)) return fail(QStringLiteral("seek failed"));
            const QByteArray body = f.read(qint64(size) - header);
            if (body.size() != qint64(size) - header) return fail(QStringLiteral("short read"));
            if (type == "moov") {
                if (!parseMoov(body, tracks, defaults)) return fail(QStringLiteral("malformed moov"));
                haveMoov = true;
            } else {
                if (!haveMoov) return fail(QStringLiteral("moof before moov"));
                haveMoof = true;
                std::vector<Box> trafs;
                if (!children(body, 0, body.size(), trafs)) return fail(QStringLiteral("malformed moof"));
                for (const Box& traf : trafs) {
                    if (traf.type != "traf") continue;
                    std::vector<Box> parts;
                    if (!children(body, traf.start, traf.end, parts)) return fail(QStringLiteral("malformed traf"));
                    const Box* tfhd = find(parts, "tfhd");
                    if (!tfhd || tfhd->end - tfhd->start < 8) return fail(QStringLiteral("traf without tfhd"));
                    const char* h = body.constData() + tfhd->start;
                    const quint32 hflags = be32(h) & 0xffffff;
                    const quint32 trackId = be32(h + 4);
                    Track* track = nullptr;
                    for (Track& t : tracks) if (t.id == trackId) track = &t;
                    if (!track) return fail(QStringLiteral("fragment for unknown track %1").arg(trackId));
                    TrackDefaults d = defaults[trackId];
                    qint64 p = 8;
                    qint64 base = off;   // default: the moof itself
                    const auto need = [&](qint64 n) { return tfhd->start + p + n <= tfhd->end; };
                    if (hflags & 0x1) { if (!need(8)) return fail(QStringLiteral("short tfhd")); base = qint64(be64(h + p)); p += 8; }
                    if (hflags & 0x2) p += 4;
                    if (hflags & 0x8) { if (!need(4)) return fail(QStringLiteral("short tfhd")); d.duration = be32(h + p); p += 4; }
                    if (hflags & 0x10) { if (!need(4)) return fail(QStringLiteral("short tfhd")); d.size = be32(h + p); p += 4; }
                    if (hflags & 0x20) { if (!need(4)) return fail(QStringLiteral("short tfhd")); d.flags = be32(h + p); p += 4; }
                    if (const Box* tfdt = find(parts, "tfdt")) {
                        const char* t = body.constData() + tfdt->start;
                        decodeTime[trackId] = t[0] == 1 ? be64(t + 4) : be32(t + 4);
                    }
                    qint64 dataPos = base;
                    for (const Box& trun : parts) {
                        if (trun.type != "trun") continue;
                        const char* r = body.constData() + trun.start;
                        const qint64 len = trun.end - trun.start;
                        if (len < 8) return fail(QStringLiteral("short trun"));
                        const int version = r[0];
                        const quint32 rflags = be32(r) & 0xffffff;
                        const quint32 count = be32(r + 4);
                        qint64 q = 8;
                        if (rflags & 0x1) { if (q + 4 > len) return fail(QStringLiteral("short trun")); dataPos = base + qint32(be32(r + q)); q += 4; }
                        quint32 firstFlags = d.flags;
                        const bool hasFirst = rflags & 0x4;
                        if (hasFirst) { if (q + 4 > len) return fail(QStringLiteral("short trun")); firstFlags = be32(r + q); q += 4; }
                        const qint64 per = ((rflags & 0x100) ? 4 : 0) + ((rflags & 0x200) ? 4 : 0)
                                         + ((rflags & 0x400) ? 4 : 0) + ((rflags & 0x800) ? 4 : 0);
                        if (q + per * qint64(count) > len) return fail(QStringLiteral("trun entries past its end"));
                        for (quint32 i = 0; i < count; ++i) {
                            quint32 dur = d.duration, sz = d.size, fl = (i == 0 && hasFirst) ? firstFlags : d.flags;
                            qint64 cto = 0;
                            if (rflags & 0x100) { dur = be32(r + q); q += 4; }
                            if (rflags & 0x200) { sz = be32(r + q); q += 4; }
                            if (rflags & 0x400) { fl = be32(r + q); q += 4; }
                            if (rflags & 0x800) { const quint32 c = be32(r + q); cto = version == 0 ? qint64(c) : qint64(qint32(c)); q += 4; }
                            if (dataPos < 0 || dataPos + qint64(sz) > fileSize)
                                return fail(QStringLiteral("sample data outside the file"));
                            Sample s;
                            s.offset = dataPos;
                            s.size = sz;
                            const quint64 dts = decodeTime[trackId];
                            s.time = to100ns(dts, track->timescale) + (cto >= 0 ? to100ns(quint64(cto), track->timescale)
                                                                               : -to100ns(quint64(-cto), track->timescale));
                            s.duration = to100ns(dur, track->timescale);
                            s.sync = !(fl & 0x10000);   // sample_is_non_sync_sample
                            track->samples.push_back(s);
                            decodeTime[trackId] = dts + dur;
                            dataPos += sz;
                        }
                    }
                }
            }
        }
        off += qint64(size);
    }
    if (!haveMoov) return fail(QStringLiteral("no moov"));
    if (!haveMoof) return fail(QStringLiteral("not a fragmented MP4"));
    return true;
}

bool readPayload(const QString& path, const Sample& sample, QByteArray& out)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || !f.seek(sample.offset))
        return false;
    out = f.read(sample.size);
    return out.size() == qint64(sample.size);
}

bool lengthPrefixedToAnnexB(QByteArray& sample)
{
    char* p = sample.data();
    const qint64 n = sample.size();
    qint64 off = 0;
    while (off + 4 <= n) {
        const quint32 len = be32(p + off);
        if (len == 0 || off + 4 + qint64(len) > n)
            return false;
        p[off] = 0; p[off + 1] = 0; p[off + 2] = 0; p[off + 3] = 1;
        off += 4 + qint64(len);
    }
    return off == n;
}

} // namespace Fmp4Index
