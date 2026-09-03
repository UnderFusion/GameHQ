#include "notify/NotificationCenter.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QScreen>
#include <QUrl>
#include <QDebug>

#include <windows.h>

NotificationCenter::NotificationCenter(QQmlApplicationEngine* engine, QObject* parent)
    : QObject(parent)
    , m_engine(engine)
{
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
    return true;
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

    if (!m_window->isVisible())
        m_window->show();   // SW_SHOWNOACTIVATE via WindowDoesNotAcceptFocus
    m_window->raise();      // stay above the game for subsequent toasts
}

void NotificationCenter::post(const QString& title, const QString& body,
                              const QString& imagePath, const QString& kind,
                              const QDateTime& when, bool isVideo)
{
    if (!ensureLoaded())
        return;
    positionAndShow();
    const QString url = imagePath.isEmpty()
        ? QString()
        : QUrl::fromLocalFile(imagePath).toString();
    emit posted(title, body, url, kind, when, isVideo);
    qInfo() << "Notification:" << kind << title << body;
}

void NotificationCenter::hideWindow()
{
    if (m_window && m_window->isVisible())
        m_window->hide();
}
