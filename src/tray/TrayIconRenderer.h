#pragma once

#include <QColor>
#include <QHash>
#include <QIcon>
#include <QPixmap>
#include <QString>

#include "tray/TrayStatus.h"

// Tray icon composition (issue #38).
//
// Three presentation modes, all driven by one TrayStatus::Visual:
//   Badge   — the ordinary GameHQ icon with a small corner badge (default)
//   Replace — the whole icon becomes the status glyph; base icon when idle
//   Static  — always the plain GameHQ icon; only the tooltip reports anything
//
// Icons are expensive to build (five painted pixmaps each), so every
// (mode, state) pair is rendered once and kept. The key space is finite and
// the cache has a hard cap, so a long-running session cannot grow it and no
// update after warm-up allocates.
class TrayIconRenderer
{
public:
    enum class Mode { Badge, Replace, Static };

    // Wire value of ui.tray_status_display. Unknown values fall back to Badge,
    // so a hand-edited config.json cannot leave the tray iconless.
    static Mode modeFromConfig(const QString& value);
    static QString configValue(Mode mode);

    // Hard cap. 3 modes x 5 states = 15 is the real key space; the extra slot
    // keeps the cap a safety net rather than a load-bearing assumption.
    static constexpr int kMaxCacheEntries = 16;

    QIcon icon(TrayStatus::Visual visual, Mode mode);

    // The shipped icon, at one pixel size. Falls back to a drawn plate when
    // the resource bundle is absent (unit tests, early startup), so a missing
    // .ico can never turn into a null tray icon.
    static QPixmap basePixmap(int size);

    int cacheSize() const { return m_cache.size(); }
    void clearCache() { m_cache.clear(); }

    // Status colours. These mirror the skin tokens in
    // src/ui/qml/themes/Skin.qml (success / warning / danger) because a tray
    // icon is painted by the process, not by QML, and must not change with the
    // in-app theme — the Windows tray has its own light/dark surface.
    static constexpr QRgb SuccessColor = 0xFF4ADE80;   // ready: green check
    static constexpr QRgb WarningColor = 0xFFFFC24D;   // screenshot: amber/yellow
    static constexpr QRgb DangerColor = 0xFFFF5D73;    // clip: red record dot

private:
    static quint64 cacheKey(Mode mode, TrayStatus::Visual visual);
    static QIcon render(Mode mode, TrayStatus::Visual visual);

    QHash<quint64, QIcon> m_cache;
};
