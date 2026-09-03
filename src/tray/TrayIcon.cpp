#include "tray/TrayIcon.h"
#include "Brand.h"
#include "localization/NativeText.h"

#include <QAction>
#include <QCoreApplication>
#include <QIcon>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>

namespace {

// Tray menu glyphs are drawn, not bundled.
//
// They have to follow the *system* menu palette (this is a native QMenu, so it
// is dark or light with Windows, not with our skin), and a shipped PNG cannot
// do that — it would be a white glyph on a white menu the moment the user
// switches to light mode. An SVG could not either without a recolor pass. Each
// glyph is a few strokes, so drawing them costs less than an asset would.
//
// Everything is authored on a 24x24 grid and scaled per size, so the strokes
// land on pixel centers instead of being downsampled from one large bitmap.
using GlyphFn = void (*)(QPainter&);

QIcon makeGlyph(const QColor& color, GlyphFn draw)
{
    QIcon icon;
    for (int size : {16, 20, 24, 32}) {
        QPixmap pm(size, size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.scale(size / 24.0, size / 24.0);
        QPen pen(color, 1.8);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        draw(p);
        p.end();
        icon.addPixmap(pm);
    }
    return icon;
}

// Gallery: four tiles, the app's own grid.
void drawGallery(QPainter& p)
{
    p.drawRoundedRect(QRectF(4, 4, 7, 7), 1.5, 1.5);
    p.drawRoundedRect(QRectF(13, 4, 7, 7), 1.5, 1.5);
    p.drawRoundedRect(QRectF(4, 13, 7, 7), 1.5, 1.5);
    p.drawRoundedRect(QRectF(13, 13, 7, 7), 1.5, 1.5);
}

// Rescan: an open circular arrow.
void drawRescan(QPainter& p)
{
    const QRectF r(4.5, 4.5, 15, 15);
    p.drawArc(r, 60 * 16, 280 * 16);   // gap at the top-right for the head
    QPainterPath head;
    head.moveTo(15.5, 3.5);
    head.lineTo(19.5, 6.2);
    head.lineTo(15.2, 8.6);
    p.drawPath(head);
}

// Screenshot: camera body plus lens.
void drawScreenshot(QPainter& p)
{
    p.drawRoundedRect(QRectF(3, 7, 18, 13), 2.5, 2.5);
    QPainterPath hump;
    hump.moveTo(8, 7);
    hump.lineTo(9.8, 4);
    hump.lineTo(14.2, 4);
    hump.lineTo(16, 7);
    p.drawPath(hump);
    p.drawEllipse(QPointF(12, 13.5), 3.6, 3.6);
}

// Save Replay: a clip rewinding — arrow curling back into a frame.
void drawReplay(QPainter& p)
{
    p.drawRoundedRect(QRectF(3.5, 5.5, 17, 13), 2.0, 2.0);
    QPainterPath play;
    play.moveTo(10, 9.2);
    play.lineTo(15.5, 12);
    play.lineTo(10, 14.8);
    play.closeSubpath();
    p.fillPath(play, p.pen().color());
}

// Exit: the standard power mark.
void drawExit(QPainter& p)
{
    p.drawArc(QRectF(4.5, 5.5, 15, 15), 55 * 16, 250 * 16);
    p.drawLine(QPointF(12, 3.5), QPointF(12, 11.5));
}

}  // namespace

TrayIcon::TrayIcon(QObject* parent, bool show)
    : QObject(parent)
    , m_tray(new QSystemTrayIcon(this))
    , m_menu(new QMenu)
{
    // Glyph color comes from the menu's own palette, so the icons track the
    // Windows light/dark setting the same way the menu text does.
    const QColor ink = m_menu->palette().color(QPalette::WindowText);

    m_openAction = m_menu->addAction(makeGlyph(ink, drawGallery), QString());
    connect(m_openAction, &QAction::triggered, this, &TrayIcon::openGalleryRequested);

    m_rescanAction = m_menu->addAction(makeGlyph(ink, drawRescan), QString());
    connect(m_rescanAction, &QAction::triggered, this, &TrayIcon::rescanRequested);

    m_menu->addSeparator();
    m_screenshotAction = m_menu->addAction(makeGlyph(ink, drawScreenshot), QString());
    connect(m_screenshotAction, &QAction::triggered, this, &TrayIcon::screenshotRequested);
    // Qt fades the icon for us in the disabled state.
    m_replayAction = m_menu->addAction(makeGlyph(ink, drawReplay), QString());
    m_replayAction->setEnabled(false);
    m_menu->addSeparator();

    m_quitAction = m_menu->addAction(makeGlyph(ink, drawExit), QString());
    connect(m_quitAction, &QAction::triggered, this, &TrayIcon::quitRequested);

    m_tray->setIcon(QIcon(QStringLiteral(":/icons/gamehq.ico")));
    m_tray->setToolTip(QString::fromLatin1(Brand::Name) + QLatin1Char(' ')
                       + QStringLiteral(GAMEHQ_VERSION));
    m_tray->setContextMenu(m_menu);
    connect(m_tray, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger
                    || reason == QSystemTrayIcon::DoubleClick)
                    emit openGalleryRequested();
            });
    retranslate();
    if (show)
        m_tray->show();
}

TrayIcon::~TrayIcon()
{
    delete m_menu;
}

void TrayIcon::showNotification(const QString& title, const QString& body)
{
    if (QSystemTrayIcon::supportsMessages())
        m_tray->showMessage(title, body, QIcon(QStringLiteral(":/icons/gamehq.ico")), 3000);
}

QString TrayIcon::openGalleryText() const
{
    return m_openAction ? m_openAction->text() : QString();
}

QAction* TrayIcon::actionForId(const QString& id) const
{
    if (id == QLatin1String("open_gallery")) return m_openAction;
    if (id == QLatin1String("rescan")) return m_rescanAction;
    if (id == QLatin1String("screenshot")) return m_screenshotAction;
    if (id == QLatin1String("save_replay")) return m_replayAction;
    if (id == QLatin1String("quit")) return m_quitAction;
    return nullptr;
}

int TrayIcon::menuActionCount() const
{
    return m_menu ? m_menu->actions().size() : 0;
}

void TrayIcon::retranslate()
{
    if (!m_openAction)
        return;

    m_openAction->setText(NativeText::get(
        //: Tray menu command that opens the desktop gallery.
        //% "Open Gallery"
        QT_TRID_NOOP("gamehq.tray.open_gallery"), "Open Gallery"));
    m_rescanAction->setText(NativeText::get(
        //: Tray menu command that scans capture folders again.
        //% "Rescan Captures"
        QT_TRID_NOOP("gamehq.tray.rescan_captures"), "Rescan Captures"));
    m_screenshotAction->setText(NativeText::get(
        //: Tray menu command that captures the current game image.
        //% "Take Screenshot"
        QT_TRID_NOOP("gamehq.tray.take_screenshot"), "Take Screenshot"));
    m_replayAction->setText(NativeText::get(
        //: Disabled tray menu placeholder for saving the replay buffer.
        //% "Save Replay"
        QT_TRID_NOOP("gamehq.tray.save_replay"), "Save Replay"));
    m_quitAction->setText(NativeText::get(
        //: Tray menu command that exits GameHQ.
        //% "Exit"
        QT_TRID_NOOP("gamehq.tray.exit"), "Exit"));
}
