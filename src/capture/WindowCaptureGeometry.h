#pragma once

#include <QRect>

namespace WindowCaptureGeometry
{
// Both rectangles are in physical screen pixels. The capture texture starts
// at the visible frame's origin, which can be outside the monitor.
inline QRect clientCrop(const QRect& frame, const QRect& client, const QSize& content)
{
    if (frame.size() != content || client.width() < 2 || client.height() < 2
        || !frame.contains(client))
        return {};
    return client.translated(-frame.topLeft());
}
}
