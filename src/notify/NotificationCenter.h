#pragma once
#include <QDateTime>
#include <QObject>
#include <QPoint>
#include <QRectF>
#include <QTimer>
#include <QString>
#include "notify/ToastModel.h"

class QQmlApplicationEngine;
class QQuickWindow;

// App-wide toast notifications (docs/notifications.md). Lazy-loads a frameless,
// topmost, click-through, NON-ACTIVATING window pinned to the bottom-right of
// the monitor the active app (usually the game) is on, so posting a toast never
// steals focus from the game. The QML side stacks Toast cards that auto-dismiss;
// it calls hideWindow() when the last one is gone.
//
// Reusable: any subsystem can call post(title, body, imagePath, kind).
class NotificationCenter : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QAbstractItemModel* visibleToasts READ visibleToasts CONSTANT)
    Q_PROPERTY(int visibleLimit READ visibleLimit WRITE setVisibleLimit NOTIFY visibleLimitChanged)
    // The window stays click-through. Only after the visible mouse cursor moves
    // do cards offer a close button, and only while the pointer is over the
    // stack does the window take clicks, so a game underneath keeps its input.
    Q_PROPERTY(bool pointerActive READ pointerActive NOTIFY pointerActiveChanged)
    Q_PROPERTY(bool pointerInside READ pointerInside NOTIFY pointerInsideChanged)
    Q_PROPERTY(QRectF stackRect READ stackRect WRITE setStackRect NOTIFY stackRectChanged)
public:
    explicit NotificationCenter(QQmlApplicationEngine* engine, QObject* parent = nullptr);

    // kind: "success" | "info" | "warning" | "error" (tints the accent bar;
    // warning/error add a caution badge and stay up for 10 s).
    // imagePath: optional local file path for a thumbnail ("" = text only).
    // when: optional event time, formatted by QML using the effective locale.
    // isVideo: shows a play badge over the thumbnail (imagePath is a clip frame).
    Q_INVOKABLE void post(const QString& title, const QString& body = {},
                          const QString& imagePath = {},
                          const QString& kind = QStringLiteral("info"),
                          const QDateTime& when = {},
                          bool isVideo = false);

    QAbstractItemModel* visibleToasts() { return &m_toasts; }
    int visibleLimit() const { return m_toasts.limit(); }
    void setVisibleLimit(int limit);
    bool pointerActive() const { return m_pointerActive; }
    bool pointerInside() const { return m_pointerInside; }
    QRectF stackRect() const { return m_stackRect; }
    void setStackRect(const QRectF& rect);
    void post(quint64 operationId, const QString& title, const QString& body = {},
              const QString& imagePath = {}, const QString& kind = QStringLiteral("info"),
              const QDateTime& when = {}, bool isVideo = false);
    bool update(quint64 operationId, const QString& title, const QString& body = {},
                const QString& imagePath = {}, const QString& kind = QStringLiteral("success"),
                const QDateTime& when = {}, bool isVideo = false);
    Q_INVOKABLE void dismiss(const QString& key, int revision);

    Q_INVOKABLE void hideWindow();   // QML calls this once the stack empties

signals:
    void visibleLimitChanged();
    void pointerActiveChanged();
    void pointerInsideChanged();
    void stackRectChanged();
    void updated(quint64 operationId);
    void posted(const QString& title, const QString& body,
                const QString& imageUrl, const QString& kind,
                const QDateTime& when, bool isVideo);

private:
    bool ensureLoaded();
    void positionAndShow();
    void pollPointer();
    void setPointerState(bool active, bool inside);

    ToastModel m_toasts;
    quint64 m_nextToast = 0;
    QQmlApplicationEngine* m_engine;
    QQuickWindow* m_window = nullptr;
    QTimer m_pointerPoll;
    QTimer m_pointerIdle;
    QPoint m_lastCursor;
    QRectF m_stackRect;
    bool m_pointerActive = false;
    bool m_pointerInside = false;
};
