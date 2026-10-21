#include "integration/LocalIntegrationServer.h"

#include <QJsonObject>
#include <QLocalSocket>

#include <utility>

namespace
{
constexpr int kMaximumClients = 8;
}

LocalIntegrationServer::LocalIntegrationServer(QObject *parent)
    : QObject(parent)
{
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    m_server.setMaxPendingConnections(kMaximumClients);
    connect(&m_server, &QLocalServer::newConnection,
            this, &LocalIntegrationServer::acceptConnections);
}

LocalIntegrationServer::~LocalIntegrationServer()
{
    stop();
}

bool LocalIntegrationServer::start(QString &error, const QString &serverName)
{
    error.clear();
    if (m_server.isListening())
        return true;
    const QString name = serverName.isEmpty() ? QString::fromLatin1(ServerName) : serverName;
    if (!m_server.listen(name)) {
        error = m_server.errorString();
        return false;
    }
    qInfo() << "Integration server listening on" << name
            << "with same-user access";
    return true;
}

void LocalIntegrationServer::stop()
{
    m_server.close();
    const auto sockets = m_clients.keys();
    for (QLocalSocket *socket : sockets) {
        socket->disconnect(this);
        socket->abort();
        socket->deleteLater();
    }
    m_clients.clear();
}

void LocalIntegrationServer::acceptConnections()
{
    while (QLocalSocket *socket = m_server.nextPendingConnection()) {
        if (m_clients.size() >= kMaximumClients) {
            qWarning() << "Integration client rejected: connection limit reached";
            socket->abort();
            socket->deleteLater();
            continue;
        }
        ClientState state;
        state.id = m_nextClientId++;
        state.peer = integration::identifyLocalPeer(socket);
        qInfo().noquote() << "Integration client" << state.id << "is"
                          << integration::describeLocalPeer(state.peer);
        m_clients.insert(socket, state);
        emit clientConnected(state.id);
        connect(socket, &QLocalSocket::readyRead, this,
                [this, socket] { readClient(socket); });
        connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
            const auto it = m_clients.find(socket);
            if (it != m_clients.end()) {
                const quint64 id = it->id;
                m_clients.erase(it);
                emit clientDisconnected(id);
            }
            socket->deleteLater();
        });
    }
}

integration::LocalPeer LocalIntegrationServer::clientPeer(quint64 clientId) const
{
    for (auto it = m_clients.cbegin(); it != m_clients.cend(); ++it) {
        if (it->id == clientId)
            return it->peer;
    }
    return {};
}

void LocalIntegrationServer::disconnectClient(quint64 clientId)
{
    for (auto it = m_clients.cbegin(); it != m_clients.cend(); ++it) {
        if (it->id == clientId) {
            it.key()->disconnectFromServer();
            return;
        }
    }
}

void LocalIntegrationServer::readClient(QLocalSocket *socket)
{
    auto it = m_clients.find(socket);
    if (it == m_clients.end())
        return;
    const quint64 id = it->id;
    QList<integration::Message> messages;
    QString error;
    if (!it->decoder.append(socket->readAll(), messages, error)) {
        rejectClient(socket, error);
        return;
    }
    // One read can carry several frames, and messageReceived is delivered
    // synchronously. A handler is allowed to disconnect this client - a
    // malformed lifecycle message does exactly that - which erases the entry
    // and invalidates every iterator into m_clients. Re-find the client before
    // each dispatch and stop as soon as it is gone, instead of holding `it`
    // across the loop. The entry is only erased, never freed, while we are
    // inside this slot, so looking it up by socket pointer stays safe.
    for (const integration::Message &message : std::as_const(messages)) {
        const auto current = m_clients.constFind(socket);
        if (current == m_clients.cend() || current->id != id)
            return;
        emit messageReceived(id, message);
    }
}

void LocalIntegrationServer::rejectClient(QLocalSocket *socket, const QString &reason)
{
    qWarning() << "Integration client disconnected:" << reason;
    // abort() is not guaranteed to emit disconnected(), so clean up here or
    // the entry would hold one of the kMaximumClients slots forever.
    const auto it = m_clients.find(socket);
    if (it != m_clients.end()) {
        const quint64 id = it->id;
        m_clients.erase(it);
        emit clientDisconnected(id);
    }
    socket->disconnect(this);
    socket->abort();
    socket->deleteLater();
}

bool LocalIntegrationServer::send(quint64 clientId, const QJsonObject &message)
{
    QLocalSocket *target = nullptr;
    for (auto it = m_clients.cbegin(); it != m_clients.cend(); ++it) {
        if (it->id == clientId) {
            target = it.key();
            break;
        }
    }
    if (!target || target->state() != QLocalSocket::ConnectedState)
        return false;
    QString error;
    const QByteArray frame = integration::encodeFrame(message, error);
    if (frame.isEmpty()) {
        qWarning() << "Integration reply rejected:" << error;
        return false;
    }
    if (target->bytesToWrite() > integration::MaximumBufferedBytes - frame.size()) {
        rejectClient(target, QStringLiteral("outbound queue exceeded its limit"));
        return false;
    }
    const qint64 written = target->write(frame);
    if (written != frame.size()) {
        // Anything less than the whole frame leaves a truncated prefix in the
        // socket buffer, and the next message would be appended straight onto
        // it - every later frame on this connection would be garbage. There is
        // no way to repair the stream, so drop the connection instead of
        // reporting a recoverable failure.
        rejectClient(target, QStringLiteral("the reply could not be queued completely"));
        return false;
    }
    target->flush();
    return true;
}

void LocalIntegrationServer::broadcast(const QJsonObject &message)
{
    QList<quint64> ids;
    ids.reserve(m_clients.size());
    for (const ClientState &state : std::as_const(m_clients))
        ids.push_back(state.id);
    for (quint64 id : std::as_const(ids))
        send(id, message);
}
