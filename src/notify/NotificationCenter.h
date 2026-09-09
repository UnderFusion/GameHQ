#pragma once
#include <QDateTime>
#include <QObject>
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
public:
    explicit NotificationCenter(QQmlApplicationEngine* engine, QObject* parent = nullptr);

    // kind: "success" | "info" | "error" (tints the accent bar).
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
    void updated(quint64 operationId);
    void posted(const QString& title, const QString& body,
                const QString& imageUrl, const QString& kind,
                const QDateTime& when, bool isVideo);

private:
    bool ensureLoaded();
    void positionAndShow();

    ToastModel m_toasts;
    quint64 m_nextToast = 0;
    QQmlApplicationEngine* m_engine;
    QQuickWindow* m_window = nullptr;
};
