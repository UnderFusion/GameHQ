#include "notify/NotificationCenter.h"

#include <QCursor>
#include <QPlatformSurfaceEvent>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QScreen>
#include <QUrl>
#include <QDebug>

#include <windows.h>

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011   // Windows 10 2004 (build 19041)+
#endif

NotificationCenter::NotificationCenter(QQmlApplicationEngine* engine, QObject* parent)
    : QObject(parent)
    , m_toasts(this)
    , m_engine(engine)
{
    // Cheap cursor sampling while cards are on screen; no mouse hook needed.
    m_pointerPoll.setInterval(100);
    connect(&m_pointerPoll, &QTimer::timeout, this, &NotificationCenter::pollPointer);
    m_pointerIdle.setSingleShot(true);
    m_pointerIdle.setInterval(3000);
    connect(&m_pointerIdle, &QTimer::timeout, this, [this] {
        if (!m_pointerInside)
            setPointerState(false, false);
    });
}

void NotificationCenter::setStackRect(const QRectF& rect)
{
    if (rect == m_stackRect) return;
    m_stackRect = rect;
    emit stackRectChanged();
}

void NotificationCenter::pollPointer()
{
    if (!m_window || !m_window->isVisible()) return;
    CURSORINFO info{};
    info.cbSize = sizeof(info);
    // A game that hides the cursor (mouse-look) never gets close buttons:
    // movement there steers the camera, it is not someone reaching for a card.
    const bool shown = GetCursorInfo(&info) && (info.flags & CURSOR_SHOWING);
    const QPoint pos = QCursor::pos();
    const bool moved = pos != m_lastCursor;
    m_lastCursor = pos;
    bool active = m_pointerActive;
    if (!shown)
        active = false;
    else if (moved) {
        active = true;
        m_pointerIdle.start();
    }
    const bool inside = active && m_stackRect.contains(QPointF(m_window->mapFromGlobal(pos)));
    if (m_pointerInside && !inside && active)
        m_pointerIdle.start();   // leaving the stack starts the fade-out grace
    setPointerState(active, inside);
}

void NotificationCenter::setPointerState(bool active, bool inside)
{
    if (inside != m_pointerInside && m_window) {
        // Toggle click-through through Qt, not raw WS_EX_TRANSPARENT: Qt skips
        // windows flagged WindowTransparentForInput when resolving the window
        // under the pointer, so hover, clicks and the cursor shape never reach
        // the cards otherwise. WindowDoesNotAcceptFocus stays set, so a click
        // on a card never takes focus from the game.
        m_window->setFlag(Qt::WindowTransparentForInput, !inside);
        applyCaptureExclusion();   // a flag change may have replaced the HWND
    }
    if (active != m_pointerActive) {
        m_pointerActive = active;
        emit pointerActiveChanged();
    }
    if (inside != m_pointerInside) {
        m_pointerInside = inside;
        emit pointerInsideChanged();
    }
}

bool NotificationCenter::ensureLoaded()
{
    if (m_window)
        return true;
    m_engine->loadFromModule("GameHQ", "ToastWindow");
    const auto roots = m_engine->rootObjects();
    for (QObject* root : roots) {
        if (root->objectName() == QLatin1String("gamehqToasts")) {
            m_window = qobject_cast<QQuickWindow*>(root);
            break;
        }
    }
    if (!m_window) {
        qCritical() << "Notifications: failed to load ToastWindow.qml";
        return false;
    }
    // Non-activating (WS_EX_NOACTIVATE → SW_SHOWNOACTIVATE on show), click-through
    // (WS_EX_TRANSPARENT), topmost, off the taskbar. Posting must never pull the
    // game out of focus.
    m_window->setFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                       | Qt::Tool | Qt::WindowDoesNotAcceptFocus
                       | Qt::WindowTransparentForInput);
    // Qt creates the HWND lazily and may recreate it; every new surface gets
    // the capture exclusion again (affinity belongs to the HWND, not the QWindow).
    m_window->installEventFilter(this);
    return true;
}

bool NotificationCenter::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_window && event->type() == QEvent::PlatformSurface
        && static_cast<QPlatformSurfaceEvent*>(event)->surfaceEventType()
               == QPlatformSurfaceEvent::SurfaceCreated)
        applyCaptureExclusion();
    return QObject::eventFilter(watched, event);
}

// Keep toasts out of screenshots: the SDR path BitBlts the screen with
// CAPTUREBLT, which would otherwise copy this topmost layered window, including
// the "capture request received" card posted just before the grab. Only this
// window is excluded; the player still sees it. Older Windows rejects the flag:
// toasts then keep working and may still be captured, which is logged once.
void NotificationCenter::applyCaptureExclusion()
{
    if (!m_window || !m_window->handle())
        return;   // no native window yet; SurfaceCreated will call back
    const HWND hwnd = reinterpret_cast<HWND>(m_window->winId());
    DWORD affinity = WDA_NONE;
    if (GetWindowDisplayAffinity(hwnd, &affinity) && affinity == WDA_EXCLUDEFROMCAPTURE)
        return;
    const bool ok = SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
    const DWORD error = ok ? 0 : GetLastError();
    const quintptr key = reinterpret_cast<quintptr>(hwnd);
    if (key == m_reportedHwnd)
        return;
    m_reportedHwnd = key;
    if (ok)
        qInfo() << "Notifications: toast window excluded from capture, hwnd" << Qt::hex << key;
    else
        qWarning() << "Notifications: capture exclusion unavailable (needs Windows 10 2004+),"
                   << "toasts may appear in screenshots; SetWindowDisplayAffinity error" << error;
}

void NotificationCenter::positionAndShow()
{
    // Show the stack on the monitor the active app (usually the game) is on.
    QScreen* target = QGuiApplication::primaryScreen();
    if (HWND fg = GetForegroundWindow()) {
        const HMONITOR mon = MonitorFromWindow(fg, MONITOR_DEFAULTTOPRIMARY);
        const auto screens = QGuiApplication::screens();
        for (QScreen* s : screens) {
            const QPoint c = s->geometry().center();
            if (MonitorFromPoint(POINT{ c.x(), c.y() }, MONITOR_DEFAULTTONULL) == mon) {
                target = s;
                break;
            }
        }
    }

    // Pin the window's bottom-right to the work-area corner; the QML stack keeps
    // its own inner margin so the cards sit a little off the edge.
    const QRect area = target->availableGeometry();
    m_window->setX(area.right() - m_window->width() + 1);
    m_window->setY(area.bottom() - m_window->height() + 1);

    if (!m_window->isVisible()) {
        m_window->create();
        applyCaptureExclusion();   // before the first frame reaches the screen
        m_window->show();   // SW_SHOWNOACTIVATE via WindowDoesNotAcceptFocus
        // A cursor that has not moved since the stack appeared does not count.
        m_lastCursor = QCursor::pos();
        m_pointerPoll.start();
    }
    m_window->raise();      // stay above the game for subsequent toasts
}

void NotificationCenter::post(const QString& title, const QString& body,
                              const QString& imagePath, const QString& kind,
                              const QDateTime& when, bool isVideo)
{
    post(0, title, body, imagePath, kind, when, isVideo);
}

void NotificationCenter::setVisibleLimit(int limit)
{
    if (limit == m_toasts.limit()) return;
    m_toasts.setLimit(limit);
    emit visibleLimitChanged();
    if (!m_toasts.rowCount()) hideWindow();
}
void NotificationCenter::post(quint64 id, const QString& title, const QString& body,
                               const QString& imagePath, const QString& kind,
                               const QDateTime& when, bool video)
{
    if (m_engine && !ensureLoaded()) return;
    const QString key = id ? QStringLiteral("capture:%1").arg(id)
                           : QStringLiteral("toast:%1").arg(++m_nextToast);
    const QString url = imagePath.isEmpty() ? QString() : QUrl::fromLocalFile(imagePath).toString();
    if (!m_toasts.post({key,title,body,url,kind,when,video,id != 0 && kind == "info"})) return;
    if (m_window) positionAndShow();
    emit posted(title, body, url, kind, when, video);
    qInfo() << "Notification:" << key << kind << title << body;
}
bool NotificationCenter::update(quint64 id, const QString& title, const QString& body,
                                 const QString& imagePath, const QString& kind,
                                 const QDateTime& when, bool video)
{
    if (!id) return false;
    const QString url = imagePath.isEmpty() ? QString() : QUrl::fromLocalFile(imagePath).toString();
    if (!m_toasts.update({QStringLiteral("capture:%1").arg(id),title,body,url,kind,when,video,false})) return false;
    emit updated(id);
    qInfo() << "Notification updated:" << id << kind << title << body;
    return true;
}
void NotificationCenter::dismiss(const QString& key, int revision)
{
    m_toasts.dismiss(key, revision);
    if (!m_toasts.rowCount()) hideWindow();
}

void NotificationCenter::hideWindow()
{
    m_pointerPoll.stop();
    m_pointerIdle.stop();
    setPointerState(false, false);   // back to click-through before hiding
    if (m_window && m_window->isVisible())
        m_window->hide();
}
