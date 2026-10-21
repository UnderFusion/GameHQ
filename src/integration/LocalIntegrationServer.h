#pragma once

#include "integration/IntegrationProtocol.h"
#include "integration/LocalPeer.h"

#include <QHash>
#include <QLocalServer>
#include <QObject>

class QLocalSocket;

class LocalIntegrationServer final : public QObject
{
    Q_OBJECT
public:
    static constexpr auto ServerName = "GameHQ.Local.v1";

    explicit LocalIntegrationServer(QObject *parent = nullptr);
    ~LocalIntegrationServer() override;

    // serverName overrides the production pipe name; only tests should ever
    // pass one, so a real running instance can never collide with the suite.
    bool start(QString &error, const QString &serverName = QString());
    void stop();
    bool send(quint64 clientId, const QJsonObject &message);
    void disconnectClient(quint64 clientId);
    void broadcast(const QJsonObject &message);
    int clientCount() const { return m_clients.size(); }
    // The process behind a connected client, read from the pipe when it was
    // accepted; empty for an unknown id.
    integration::LocalPeer clientPeer(quint64 clientId) const;

signals:
    void clientConnected(quint64 clientId);
    void messageReceived(quint64 clientId, const integration::Message &message);
    void clientDisconnected(quint64 clientId);

private:
    struct ClientState
    {
        quint64 id = 0;
        integration::FrameDecoder decoder;
        integration::LocalPeer peer;
    };

    void acceptConnections();
    void readClient(QLocalSocket *socket);
    void rejectClient(QLocalSocket *socket, const QString &reason);

    QLocalServer m_server;
    QHash<QLocalSocket *, ClientState> m_clients;
    quint64 m_nextClientId = 1;
};
