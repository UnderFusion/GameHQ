#include "core/WindowPlacement.h"

#include <QtGlobal>

namespace {

// A stored size of 0 (or a negative one from a hand-edited config) means "no
// usable size"; anything smaller than the window's minimum would open a window
// the layout cannot draw.
QSize resolveSize(const WindowPlacement::Saved& saved, const QSize& defaultSize,
                  const QSize& minimumSize)
{
    const QSize size(saved.width > 0 ? saved.width : defaultSize.width(),
                     saved.height > 0 ? saved.height : defaultSize.height());
    return size.expandedTo(minimumSize);
}

}  // namespace

bool WindowPlacement::isReachable(const QRect& rect, const QList<QRect>& screens)
{
    for (const QRect& screen : screens) {
        const QRect visible = rect.intersected(screen);
        if (visible.width() >= kMinVisibleWidth && visible.height() >= kMinVisibleHeight)
            return true;
    }
    return false;
}

QRect WindowPlacement::restore(const Saved& saved, const QList<QRect>& screens,
                               const QRect& fallbackScreen, const QSize& defaultSize,
                               const QSize& minimumSize)
{
    const QSize size = resolveSize(saved, defaultSize, minimumSize);

    if (saved.x != kUnsetCoordinate && saved.y != kUnsetCoordinate) {
        const QRect restored(QPoint(saved.x, saved.y), size);
        // Partly off the edge is still the user's own window: only a rectangle
        // that no screen can show is moved.
        if (isReachable(restored, screens))
            return restored;
    }

    if (!fallbackScreen.isValid())
        return QRect(QPoint(0, 0), size);   // headless / no screens: nothing to centre on

    // Recovery: centre on the fallback screen. The saved size survives unless
    // the screen is too small for it, in which case filling the screen beats
    // opening with edges nobody can reach.
    const QSize recovered = size.boundedTo(fallbackScreen.size());
    const int x = fallbackScreen.x() + (fallbackScreen.width() - recovered.width()) / 2;
    const int y = fallbackScreen.y() + (fallbackScreen.height() - recovered.height()) / 2;
    return QRect(QPoint(x, y), recovered);
}

QRect WindowPlacement::repair(const QRect& current, const QList<QRect>& screens,
                              const QRect& currentScreen, const QSize& minimumSize)
{
    const bool tooSmall = current.width() < minimumSize.width()
        || current.height() < minimumSize.height();
    if (!tooSmall && isReachable(current, screens))
        return current;
    if (!currentScreen.isValid())
        return QRect(current.topLeft(), current.size().expandedTo(minimumSize));

    const QSize size = current.size().expandedTo(minimumSize).boundedTo(currentScreen.size());
    // Keep the user's corner where possible; only slide back far enough that the
    // grown window fits on the screen it is on.
    const int x = qBound(currentScreen.left(), current.x(),
                         currentScreen.x() + currentScreen.width() - size.width());
    const int y = qBound(currentScreen.top(), current.y(),
                         currentScreen.y() + currentScreen.height() - size.height());
    return QRect(QPoint(x, y), size);
}
