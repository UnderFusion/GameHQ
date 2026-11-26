#include "tray/TrayIconRenderer.h"

#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPointF>
#include <QRectF>

#include <algorithm>

namespace {

// Same rationale as the tray menu glyphs in TrayIcon.cpp: everything is
// authored on a 24x24 grid and scaled per size, so strokes land on pixel
// centres instead of being downsampled from one large bitmap.
constexpr int kGrid = 24;

// The tray asks for these sizes; 48 covers 200 % DPI for the large-icon
// setting, 16 is the classic notification area.
constexpr int kSizes[] = { 16, 20, 24, 32, 48 };

using GlyphFn = void (*)(QPainter&);

void drawCheck(QPainter& p)
{
    QPainterPath path;
    path.moveTo(5.0, 12.4);
    path.lineTo(9.8, 17.2);
    path.lineTo(19.0, 7.2);
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
}

// "Screenshot": camera body plus lens. A plain square reads as "no image" at
// tray sizes, the lens tells them apart.
void drawCamera(QPainter& p)
{
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(2.6, 7.0, 18.8, 12.0), 2.6, 2.6);
    p.drawEllipse(QPointF(12.0, 13.0), 3.7, 3.7);
}

// "Clip saved": the recording dot.
void drawRecordDot(QPainter& p)
{
    p.setBrush(p.pen().color());
    p.setPen(Qt::NoPen);
    p.drawEllipse(QPointF(12.0, 12.0), 7.0, 7.0);
}

// "Capture problem": exclamation mark.
void drawWarning(QPainter& p)
{
    p.setBrush(p.pen().color());
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(QRectF(10.5, 3.6, 3.0, 11.2), 1.5, 1.5);
    p.drawEllipse(QPointF(12.0, 18.6), 1.7, 1.7);
}

GlyphFn glyphFor(TrayStatus::Visual visual)
{
    switch (visual) {
    case TrayStatus::Visual::Ready:
        return &drawCheck;
    case TrayStatus::Visual::ScreenshotSaved:
        return &drawCamera;
    case TrayStatus::Visual::ClipSaved:
        return &drawRecordDot;
    case TrayStatus::Visual::CaptureProblem:
        return &drawWarning;
    case TrayStatus::Visual::Idle:
        break;
    }
    return &drawCheck;
}

QColor statusColor(TrayStatus::Visual visual)
{
    switch (visual) {
    case TrayStatus::Visual::Ready:
        return QColor(TrayIconRenderer::SuccessColor);
    case TrayStatus::Visual::ScreenshotSaved:
        return QColor(TrayIconRenderer::WarningColor);
    case TrayStatus::Visual::ClipSaved:
    case TrayStatus::Visual::CaptureProblem:
        return QColor(TrayIconRenderer::DangerColor);
    case TrayStatus::Visual::Idle:
        break;
    }
    return QColor(TrayIconRenderer::SuccessColor);
}

// Ink knocked out of a status plate. Near-black rather than pure black: it
// reads on every plate colour without the harsh edge pure black gives on a
// thin glyph.
QColor inkColor()
{
    return QColor(0x10, 0x12, 0x16);
}

// Paints `visual`'s glyph inside `box` (device pixels), on the 24-grid.
void paintGlyph(QPainter& p, const QRectF& box, const QColor& ink,
                TrayStatus::Visual visual)
{
    if (box.width() <= 0.0 || box.height() <= 0.0)
        return;
    p.save();
    p.translate(box.topLeft());
    p.scale(box.width() / kGrid, box.height() / kGrid);
    QPen pen(ink, 3.1);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    glyphFor(visual)(p);
    p.restore();
}

// Filled plate with the glyph knocked out of it — the shape shared by the
// corner badge and the full-size replacement icon.
void paintPlate(QPainter& p, const QRectF& plate, qreal radius,
                TrayStatus::Visual visual)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(statusColor(visual));
    p.drawRoundedRect(plate, radius, radius);
    p.restore();

    // A glyph inset of ~17 % keeps the plate readable as a shape at 16 px.
    const qreal inset = plate.width() * 0.17;
    paintGlyph(p, plate.adjusted(inset, inset, -inset, -inset),
               inkColor(), visual);
}

bool resourceIconAvailable()
{
    // Probed once, and the probe icon dies immediately: keeping a QIcon alive
    // in a function-local static would outlive QGuiApplication at exit.
    static const bool available = [] {
        const QIcon probe(QStringLiteral(":/icons/gamehq.ico"));
        return !probe.isNull() && !probe.pixmap(QSize(16, 16)).isNull();
    }();
    return available;
}

QIcon buildBadged(TrayStatus::Visual visual)
{
    QIcon icon;
    for (int size : kSizes) {
        QPixmap pm = TrayIconRenderer::basePixmap(size);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        // Corner badge, bottom-right, ~58 % of the tile. On the smallest sizes
        // a hard floor keeps it from collapsing to a few pixels.
        const qreal d = std::max<qreal>(7.0, size * 0.58);
        const QRectF plate(size - d - 0.5, size - d - 0.5, d, d);
        paintPlate(p, plate, d / 2.0, visual);
        if (size >= 24) {
            // Separates the badge from a dark icon behind it; skipped at 16 px
            // where a ring would eat the glyph.
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor(0, 0, 0, 90), 1.0));
            p.drawEllipse(plate.adjusted(0.5, 0.5, -0.5, -0.5));
        }
        p.end();
        icon.addPixmap(pm);
    }
    return icon;
}

QIcon buildReplacement(TrayStatus::Visual visual)
{
    QIcon icon;
    for (int size : kSizes) {
        QPixmap pm(size, size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QRectF plate(0.5, 0.5, size - 1.0, size - 1.0);
        paintPlate(p, plate, size * 0.18, visual);
        p.end();
        icon.addPixmap(pm);
    }
    return icon;
}

}  // namespace

TrayIconRenderer::Mode TrayIconRenderer::modeFromConfig(const QString& value)
{
    if (value == QLatin1String("replace"))
        return Mode::Replace;
    if (value == QLatin1String("static"))
        return Mode::Static;
    return Mode::Badge;
}

QString TrayIconRenderer::configValue(Mode mode)
{
    switch (mode) {
    case Mode::Replace:
        return QStringLiteral("replace");
    case Mode::Static:
        return QStringLiteral("static");
    case Mode::Badge:
        break;
    }
    return QStringLiteral("badge");
}

quint64 TrayIconRenderer::cacheKey(Mode mode, TrayStatus::Visual visual)
{
    return (quint64(static_cast<int>(mode)) << 8)
        | quint64(static_cast<int>(visual));
}

QIcon TrayIconRenderer::render(Mode mode, TrayStatus::Visual visual)
{
    if (mode == Mode::Static || visual == TrayStatus::Visual::Idle) {
        // Idle shows the product icon in every mode: "nothing to report" is
        // itself the original GameHQ tray icon, not a blank or a grey glyph.
        QIcon icon;
        for (int size : kSizes)
            icon.addPixmap(basePixmap(size));
        return icon;
    }
    return mode == Mode::Replace ? buildReplacement(visual) : buildBadged(visual);
}

QIcon TrayIconRenderer::icon(TrayStatus::Visual visual, Mode mode)
{
    const quint64 key = cacheKey(mode, visual);
    const auto it = m_cache.constFind(key);
    if (it != m_cache.constEnd())
        return it.value();
    // Bounded by construction: the key space is 3 x 5. The cap only guards
    // against a future state being added without this comment being updated.
    if (m_cache.size() >= kMaxCacheEntries)
        m_cache.clear();
    const QIcon built = render(mode, visual);
    m_cache.insert(key, built);
    return built;
}

QPixmap TrayIconRenderer::basePixmap(int size)
{
    if (resourceIconAvailable()) {
        const QPixmap pm = QIcon(QStringLiteral(":/icons/gamehq.ico"))
                               .pixmap(QSize(size, size));
        if (!pm.isNull()) {
            if (pm.width() == size && pm.height() == size)
                return pm;
            return pm.scaled(size, size, Qt::KeepAspectRatio,
                             Qt::SmoothTransformation);
        }
    }

    // Fallback plate for a build without the resource bundle. Colour-neutral on
    // purpose: it must not look like either a Ready or a warning state.
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x3A, 0x40, 0x4E));
    const qreal radius = size * 0.22;
    p.drawRoundedRect(QRectF(0.5, 0.5, size - 1.0, size - 1.0), radius, radius);
    p.end();
    return pm;
}
