#pragma once

#include "share/ShareProviderRegistry.h"
#include "share/external/ExternalProvider.h"
#include "share/external/ExternalProviderProtocol.h"

#include <QHash>
#include <QLocalServer>
#include <QObject>

#include <memory>

class QLocalSocket;
class QTimer;

namespace share::external
{

// Listens on the same-user pipe `GameHQ.Share.Provider.v1` and turns each
// handshaken out-of-process provider into a registered Share provider for as
// long as its connection lives.
//
// Trust model (docs/share-provider-api-v1.md): the pipe admits only processes
// of the same Windows user, but such a process is not trusted. It can only
// register a destination, answer target queries and report results; it can
// never make GameHQ read, list or execute anything. GameHQ does not discover
// or launch providers: a provider process connects on its own.
class ExternalProviderHost : public QObject
{
    Q_OBJECT
public:
    static constexpr int kMaxConnections = 8;
    static constexpr int kMaxViolations = 5;
    static constexpr int kDefaultHandshakeTimeoutMs = 5000;
    // After a final `error`, the pipe stays open this long so the peer can
    // read it: tearing down the server end of a Windows pipe discards unread
    // data, which would turn "protocol_incompatible" into a bare disconnect.
    static constexpr int kCloseGraceMs = 300;
    static constexpr int kMaxLingering = 16;

    explicit ExternalProviderHost(ProviderRegistry* registry, QObject* parent = nullptr);
    ~ExternalProviderHost() override;

    // `serverName` overrides the production pipe name; only tests pass one.
    bool start(QString& error, const QString& serverName = QString());
    void stop();

    void setAppVersion(const QString& version) { m_appVersion = version; }
    void setHandshakeTimeoutMs(int ms) { m_handshakeTimeoutMs = ms; }
    void setTargetsTimeoutMs(int ms) { m_targetsTimeoutMs = ms; }

    int connectionCount() const { return m_conns.size(); }
    int providerCount() const;

signals:
    void providerRegistered(const QString& providerId);
    void providerRemoved(const QString& providerId);

private:
    struct Conn
    {
        QLocalSocket* socket = nullptr;
        FrameDecoder decoder;
        QTimer* handshakeTimer = nullptr;
        ExternalProvider* provider = nullptr;
        int violations = 0;
        bool handshaken = false;
    };

    void acceptConnections();
    void readSocket(QLocalSocket* socket);
    void dispatch(const std::shared_ptr<Conn>& conn, const QJsonObject& message);
    void handleHello(const std::shared_ptr<Conn>& conn, const QJsonObject& message);
    void violation(const std::shared_ptr<Conn>& conn, const QString& code, const QString& requestId);
    bool sendTo(QLocalSocket* socket, const QJsonObject& message);
    void sendError(QLocalSocket* socket, const QString& code, const QString& requestId);
    // Immediate teardown: the peer is gone, the stream is broken, or we stop.
    void closeConnection(QLocalSocket* socket, const QString& reason);
    // Sends a final error, drops the provider now, and closes the pipe after
    // a short grace so the error can be read.
    void failConnection(QLocalSocket* socket, const QString& code, const QString& requestId,
                        const QString& reason);
    void releaseProvider(const std::shared_ptr<Conn>& conn);
    void lingerThenClose(QLocalSocket* socket);

    ProviderRegistry* m_registry;
    QLocalServer m_server;
    QHash<QLocalSocket*, std::shared_ptr<Conn>> m_conns;
    QString m_appVersion;
    int m_handshakeTimeoutMs = kDefaultHandshakeTimeoutMs;
    int m_targetsTimeoutMs = 10000;
    int m_lingering = 0;
};

} // namespace share::external
