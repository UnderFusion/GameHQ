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
    , m_toasts(this)
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
bool NotificationCenter::failOperation(quint64 id, const QString& reason)
{
    if (!id || !m_toasts.fail(QStringLiteral("capture:%1").arg(id), reason)) return false;
    emit updated(id);
    return true;
}
void NotificationCenter::dismiss(const QString& key, int revision)
{
    m_toasts.dismiss(key, revision);
    if (!m_toasts.rowCount()) hideWindow();
}

void NotificationCenter::hideWindow()
{
    if (m_window && m_window->isVisible())
        m_window->hide();
}
