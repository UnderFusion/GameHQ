#pragma once
#include <QList>
#include <QRect>
#include <QSize>

#include <limits>

// Where the desktop window reopens. The saved rectangle is kept whenever the
// user can still see and grab it on *some* connected screen — a monitor left of
// or above the primary has negative coordinates and is just as valid as any
// other — and the window is only recentred when the saved place is gone
// (unset, or a display that was unplugged since the last run).
//
// This lives in core/ and speaks plain QRect on purpose: it needs no QScreen,
// no window and no ConfigManager, so every M11 case is a unit test. AppController
// supplies the real screen list, ConfigManager the stored numbers.
namespace WindowPlacement {

// x/y sentinel for "this profile has never saved a position". A real window can
// never sit here, which is what lets a legitimate negative coordinate stay
// legitimate — the old code read a missing position as -1 and therefore had to
// reject every negative coordinate with it.
inline constexpr int kUnsetCoordinate = std::numeric_limits<int>::min();

// The window's default and smallest size, mirrored by Main.qml's minimumWidth /
// minimumHeight.
inline constexpr int kDefaultWidth = 1280;
inline constexpr int kDefaultHeight = 760;
inline constexpr int kMinimumWidth = 1024;
inline constexpr int kMinimumHeight = 640;

// How much of the window has to land on one screen for the saved position to
// count as reachable: enough title bar to see it and drag it back. Measured as
// intersection *dimensions*, not area, so a 2 px wide full-height sliver does
// not qualify.
inline constexpr int kMinVisibleWidth = 120;
inline constexpr int kMinVisibleHeight = 32;

// The four numbers as stored in config.json (ui.window_*).
struct Saved
{
    int x = kUnsetCoordinate;
    int y = kUnsetCoordinate;
    int width = 0;
    int height = 0;
};

// True when `rect` overlaps one of `screens` by at least kMinVisibleWidth x
// kMinVisibleHeight. Each screen is passed as its own available geometry rather
// than one virtual bounding box, because the bounding box of an L-shaped
// arrangement covers gaps where no display exists.
bool isReachable(const QRect& rect, const QList<QRect>& screens);

// The rectangle the window should open with. Keeps the saved position untouched
// when it is reachable; otherwise centres the saved size on `fallbackScreen`,
// shrinking it only where that screen cannot hold it.
QRect restore(const Saved& saved,
              const QList<QRect>& screens,
              const QRect& fallbackScreen,
              const QSize& defaultSize = QSize(kDefaultWidth, kDefaultHeight),
              const QSize& minimumSize = QSize(kMinimumWidth, kMinimumHeight));

}  // namespace WindowPlacement
