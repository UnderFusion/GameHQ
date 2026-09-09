#include "capture/CaptureRequest.h"

#include <QElapsedTimer>

#include <atomic>

namespace
{
// Requests are minted on the input thread and read on the main thread, so the
// counter is atomic. It starts at 1: 0 means "no request" everywhere.
std::atomic<quint64> g_nextId{1};

// Started by the first request rather than at load time, so the first stage
// line reads +0ms instead of "however long startup took".
qint64 elapsedMs()
{
    static QElapsedTimer timer;
    if (!timer.isValid()) {
        timer.start();
        return 0;
    }
    return timer.elapsed();
}
}

CaptureRequest CaptureRequest::create(Source source)
{
    CaptureRequest request;
    request.id = g_nextId.fetch_add(1, std::memory_order_relaxed);
    request.source = source;
    request.monotonicMs = elapsedMs();
    return request;
}

CaptureRequest::Source CaptureRequest::sourceForDeviceGroup(const QString& deviceGroup)
{
    if (deviceGroup == QLatin1String("controller"))
        return Source::Controller;
    if (deviceGroup == QLatin1String("keyboard"))
        return Source::Keyboard;
    if (deviceGroup == QLatin1String("mouse"))
        return Source::Mouse;
    return Source::Unknown;
}

QString CaptureRequest::label(Source source)
{
    switch (source) {
    case Source::Controller: return QStringLiteral("controller");
    case Source::Keyboard:   return QStringLiteral("keyboard");
    case Source::Mouse:      return QStringLiteral("mouse");
    case Source::Overlay:    return QStringLiteral("overlay");
    case Source::Ui:         return QStringLiteral("ui");
    case Source::Tray:       return QStringLiteral("tray");
    case Source::Unknown:    break;
    }
    return QStringLiteral("unknown");
}

QString CaptureRequest::tag() const
{
    if (!isValid())
        return QStringLiteral("- src=%1").arg(label(source));
    return QStringLiteral("%1 src=%2 +%3ms")
        .arg(id)
        .arg(label(source))
        .arg(monotonicMs);
}
