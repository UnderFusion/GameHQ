#pragma once

#include "integration/ExternalGameContext.h"
#include "integration/LocalIntegrationServer.h"

#include <QHash>
#include <QObject>

class IntegrationService final : public QObject
{
    Q_OBJECT
public:
    explicit IntegrationService(QString appVersion, QObject *parent = nullptr,
                                int disconnectGraceMs = 10000);

    // serverName overrides the production pipe name; only tests should pass one.
    bool start(QString &error, const QString &serverName = QString())
    {
        return m_server.start(error, serverName);
    }
    integration::ExternalGameContext *externalContext() { return &m_context; }
    int clientCount() const { return m_server.clientCount(); }

    // True when the executable is a Playnite host process that legitimately
    // carries the GameHQ.Playnite add-on.
    static bool isExpectedPlayniteHost(const QString &executablePath);

    void broadcastMaintenance(int retryAfterSeconds);
    void cancelMaintenance() { m_maintenance = false; }

signals:
    void activateRequested();
    void openGalleryRequested();

private:
    struct Client
    {
        bool handshaken = false;
        QString name;
        int protocol = 0;
    };

    void onConnected(quint64 clientId);
    void onDisconnected(quint64 clientId);
    void onMessage(quint64 clientId, const integration::Message &message);
    bool handleHello(quint64 clientId, const integration::Message &message);
    bool handleLifecycle(quint64 clientId, const integration::Message &message);
    void sendAck(quint64 clientId, const integration::Message &request);
    void sendError(quint64 clientId, const integration::Message &request,
                   const QString &code, const QString &text, bool disconnect = false);

    QString m_appVersion;
    int m_disconnectGraceMs;
    LocalIntegrationServer m_server;
    integration::ExternalGameContext m_context;
    QHash<quint64, Client> m_clients;
    bool m_maintenance = false;
};
