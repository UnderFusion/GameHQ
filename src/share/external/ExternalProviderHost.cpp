#include "share/external/ExternalProviderHost.h"

#include <QDebug>
#include <QJsonArray>
#include <QLocalSocket>
#include <QTimer>

namespace share::external
{

ExternalProviderHost::ExternalProviderHost(ProviderRegistry* registry, QObject* parent)
    : QObject(parent)
    , m_registry(registry)
{
    // Same Windows user only: no network listener, no cross-user access.
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    m_server.setMaxPendingConnections(kMaxConnections);
    connect(&m_server, &QLocalServer::newConnection, this, &ExternalProviderHost::acceptConnections);
}

ExternalProviderHost::~ExternalProviderHost()
{
    stop();
}

bool ExternalProviderHost::start(QString& error, const QString& serverName)
{
    error.clear();
    if (m_server.isListening())
        return true;
    const QString name = serverName.isEmpty() ? QString::fromLatin1(kServerName) : serverName;
    if (!m_server.listen(name)) {
        error = m_server.errorString();
        return false;
    }
    qInfo() << "Share provider host listening on" << name << "with same-user access";
    return true;
}

void ExternalProviderHost::stop()
{
    m_server.close();
    const auto sockets = m_conns.keys();
    for (QLocalSocket* socket : sockets)
        closeConnection(socket, QStringLiteral("host stopped"));
}

int ExternalProviderHost::providerCount() const
{
    int n = 0;
    for (const auto& c : m_conns) {
        if (c->handshaken)
            ++n;
    }
    return n;
}

void ExternalProviderHost::acceptConnections()
{
    while (QLocalSocket* socket = m_server.nextPendingConnection()) {
        if (m_conns.size() >= kMaxConnections) {
            sendError(socket, QString::fromLatin1(ErrorCode::TooManyProviders), {});
            socket->abort();
            socket->deleteLater();
            continue;
        }
        auto conn = std::make_shared<Conn>();
        conn->socket = socket;
        // A peer that never introduces itself does not hold a slot forever.
        conn->handshakeTimer = new QTimer(socket);
        conn->handshakeTimer->setSingleShot(true);
        conn->handshakeTimer->setInterval(m_handshakeTimeoutMs);
        connect(conn->handshakeTimer, &QTimer::timeout, this, [this, socket] {
            sendError(socket, QString::fromLatin1(ErrorCode::HandshakeTimeout), {});
            closeConnection(socket, QStringLiteral("handshake timeout"));
        });
        conn->handshakeTimer->start();
        m_conns.insert(socket, conn);
        connect(socket, &QLocalSocket::readyRead, this, [this, socket] { readSocket(socket); });
        connect(socket, &QLocalSocket::disconnected, this,
                [this, socket] { closeConnection(socket, QStringLiteral("peer disconnected")); });
    }
}

void ExternalProviderHost::readSocket(QLocalSocket* socket)
{
    const auto it = m_conns.constFind(socket);
    if (it == m_conns.cend())
        return;
    const std::shared_ptr<Conn> conn = *it;   // keeps the state alive across a mid-loop close
    QList<QJsonObject> messages;
    QString error;
    if (!conn->decoder.append(socket->readAll(), messages, error)) {
        // The stream can no longer be trusted, so there is nothing to reply to.
        closeConnection(socket, error);
        return;
    }
    for (const QJsonObject& message : std::as_const(messages)) {
        if (!m_conns.contains(socket))
            return;   // a handler closed this connection
        dispatch(conn, message);
    }
}

void ExternalProviderHost::dispatch(const std::shared_ptr<Conn>& conn, const QJsonObject& message)
{
    const QString type = message.value(QStringLiteral("type")).toString();
    const QJsonValue reqV = message.value(QStringLiteral("requestId"));
    const QString requestId = reqV.isString() && reqV.toString().size() <= 256 ? reqV.toString() : QString();

    if (!conn->handshaken) {
        if (type != QLatin1String("hello")) {
            sendError(conn->socket, QString::fromLatin1(ErrorCode::NotHandshaken), requestId);
            closeConnection(conn->socket, QStringLiteral("message before handshake"));
            return;
        }
        handleHello(conn, message);
        return;
    }
    if (!isAllowedProviderMessage(type)) {
        violation(conn, QString::fromLatin1(ErrorCode::UnknownType), requestId);
        return;
    }
    if (type == QLatin1String("hello")) {
        violation(conn, QString::fromLatin1(ErrorCode::AlreadyHandshaken), requestId);
        return;
    }
    if (type == QLatin1String("goodbye")) {
        closeConnection(conn->socket, QStringLiteral("provider said goodbye"));
        return;
    }
    const QString code = conn->provider->handleMessage(type, message);
    if (!code.isEmpty())
        violation(conn, code, requestId);
}

void ExternalProviderHost::handleHello(const std::shared_ptr<Conn>& conn, const QJsonObject& message)
{
    const QString requestId = message.value(QStringLiteral("requestId")).toString().left(256);
    Manifest manifest;
    QString code;
    if (!parseHello(message, &manifest, &code)) {
        sendError(conn->socket, code, requestId);
        closeConnection(conn->socket, QStringLiteral("hello rejected: ") + code);
        return;
    }

    QLocalSocket* socket = conn->socket;
    auto* provider = new ExternalProvider(
        manifest, [this, socket](const QJsonObject& m) { return sendTo(socket, m); }, this);
    provider->setTargetsTimeoutMs(m_targetsTimeoutMs);
    if (m_registry->add(provider) != ProviderRegistry::AddError::None) {
        delete provider;
        // One live provider per identity: the newcomer loses, the running one
        // keeps working (a hijack attempt cannot displace it).
        sendError(socket, QString::fromLatin1(ErrorCode::DuplicateProvider), requestId);
        closeConnection(socket, QStringLiteral("duplicate provider identity"));
        return;
    }
    conn->provider = provider;
    conn->handshaken = true;
    conn->handshakeTimer->stop();

    QJsonObject ack;
    ack.insert(QStringLiteral("type"), QStringLiteral("hello.ack"));
    ack.insert(QStringLiteral("requestId"), requestId);
    ack.insert(QStringLiteral("appVersion"), m_appVersion);
    ack.insert(QStringLiteral("protocolMin"), kProtocolMin);
    ack.insert(QStringLiteral("protocolMax"), kProtocolMax);
    ack.insert(QStringLiteral("protocolSelected"), manifest.selectedProtocol);
    ack.insert(QStringLiteral("providerId"), manifest.id);
    ack.insert(QStringLiteral("capabilities"), QJsonArray::fromStringList(manifest.acceptedCapabilityNames));
    ack.insert(QStringLiteral("ignoredCapabilities"), QJsonArray::fromStringList(manifest.ignoredCapabilityNames));
    ack.insert(QStringLiteral("limits"), QJsonObject{
        { QStringLiteral("maxFrameBytes"), double(kMaxFrameBytes) },
        { QStringLiteral("maxTargets"), kMaxTargets },
        { QStringLiteral("jobTimeoutMs"), manifest.jobTimeoutMs },
    });
    sendTo(socket, ack);
    qInfo() << "Share provider registered:" << manifest.id << "protocol" << manifest.selectedProtocol;
    emit providerRegistered(manifest.id);
}

void ExternalProviderHost::violation(const std::shared_ptr<Conn>& conn, const QString& code,
                                     const QString& requestId)
{
    sendError(conn->socket, code, requestId);
    if (++conn->violations >= kMaxViolations) {
        sendError(conn->socket, QString::fromLatin1(ErrorCode::TooManyViolations), {});
        closeConnection(conn->socket, QStringLiteral("too many protocol violations"));
    }
}

void ExternalProviderHost::sendError(QLocalSocket* socket, const QString& code, const QString& requestId)
{
    QJsonObject msg;
    msg.insert(QStringLiteral("type"), QStringLiteral("error"));
    msg.insert(QStringLiteral("code"), code);
    if (!requestId.isEmpty())
        msg.insert(QStringLiteral("requestId"), requestId);
    sendTo(socket, msg);
}

bool ExternalProviderHost::sendTo(QLocalSocket* socket, const QJsonObject& message)
{
    if (!socket || socket->state() != QLocalSocket::ConnectedState)
        return false;
    QString error;
    const QByteArray frame = encodeFrame(message, error);
    if (frame.isEmpty()) {
        qWarning() << "Share provider message rejected:" << error;
        return false;
    }
    // A peer that stops reading cannot make GameHQ buffer without bound.
    if (socket->bytesToWrite() > kMaxBufferedBytes - frame.size()) {
        qWarning() << "Share provider outbound queue exceeded its limit";
        socket->abort();
        return false;
    }
    if (socket->write(frame) != frame.size()) {
        socket->abort();   // a truncated frame would corrupt the stream
        return false;
    }
    socket->flush();
    return true;
}

void ExternalProviderHost::closeConnection(QLocalSocket* socket, const QString& reason)
{
    const auto it = m_conns.find(socket);
    if (it == m_conns.end())
        return;
    const std::shared_ptr<Conn> conn = *it;
    m_conns.erase(it);
    qInfo() << "Share provider connection closed:" << reason;
    if (conn->provider) {
        const QString id = conn->provider->id();
        // Ends any running job as unconfirmed before the provider disappears.
        conn->provider->connectionLost();
        m_registry->remove(id);
        conn->provider->deleteLater();
        conn->provider = nullptr;
        emit providerRemoved(id);
    }
    socket->disconnect(this);
    socket->abort();
    socket->deleteLater();
}

} // namespace share::external
