#include "integration/IntegrationService.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>

#include <utility>

namespace
{
constexpr int kProtocolMin = 1;
constexpr int kProtocolMax = 1;
constexpr int kHandshakeTimeoutMs = 5000;

void copyRequestId(const integration::Message &request, QJsonObject &reply)
{
    if (!request.requestId.isEmpty())
        reply.insert(QStringLiteral("requestId"), request.requestId);
}

bool boundedRequiredString(const QJsonObject &object, const QString &key, int maximum,
                           QString &value)
{
    const QJsonValue field = object.value(key);
    if (!field.isString() || field.toString().isEmpty() || field.toString().size() > maximum)
        return false;
    value = field.toString();
    return true;
}
}

IntegrationService::IntegrationService(QString appVersion, QObject *parent,
                                       int disconnectGraceMs)
    : QObject(parent)
    , m_appVersion(std::move(appVersion))
    , m_disconnectGraceMs(disconnectGraceMs)
{
    connect(&m_server, &LocalIntegrationServer::clientConnected,
            this, &IntegrationService::onConnected);
    connect(&m_server, &LocalIntegrationServer::clientDisconnected,
            this, &IntegrationService::onDisconnected);
    connect(&m_server, &LocalIntegrationServer::messageReceived,
            this, &IntegrationService::onMessage);
}

void IntegrationService::onConnected(quint64 clientId)
{
    m_clients.insert(clientId, {});
    qInfo() << "Integration client connected" << clientId;
    QTimer::singleShot(kHandshakeTimeoutMs, this, [this, clientId] {
        const auto it = m_clients.constFind(clientId);
        if (it != m_clients.cend() && !it->handshaken)
            m_server.disconnectClient(clientId);
    });
}

void IntegrationService::onDisconnected(quint64 clientId)
{
    const Client client = m_clients.take(clientId);
    if (!client.handshaken || client.name != QStringLiteral("GameHQ.Playnite"))
        return;
    qInfo() << "Playnite integration disconnected" << clientId
            << "- its game context expires unless it reconnects";
    // Playnite can reconnect before its previous socket reports the
    // disconnect. The replacement's handshake already cancelled the expiry, so
    // scheduling a new one here would wipe the live client's state a few
    // seconds later. Only the last remaining Playnite client starts the clock.
    const bool anotherRemains = std::any_of(
        m_clients.cbegin(), m_clients.cend(), [&client](const Client &other) {
            return other.handshaken && other.name == client.name;
        });
    if (!anotherRemains)
        m_context.scheduleSourceExpiry(client.name, m_disconnectGraceMs);
}

bool IntegrationService::handleHello(quint64 clientId,
                                     const integration::Message &message)
{
    auto it = m_clients.find(clientId);
    if (it == m_clients.end())
        return false;
    QString clientName;
    QString clientVersion;
    const QJsonValue minimum = message.object.value(QStringLiteral("protocolMin"));
    const QJsonValue maximum = message.object.value(QStringLiteral("protocolMax"));
    if (!boundedRequiredString(message.object, QStringLiteral("client"), 64, clientName)
        || !boundedRequiredString(message.object, QStringLiteral("clientVersion"), 32,
                                  clientVersion)
        || !minimum.isDouble() || !maximum.isDouble()) {
        sendError(clientId, message, QStringLiteral("malformed"),
                  QStringLiteral("The hello message is missing required fields."), true);
        return false;
    }
    const int clientMin = minimum.toInt(-1);
    const int clientMax = maximum.toInt(-1);
    if (clientMin < 1 || clientMax < clientMin || clientMax > 100) {
        sendError(clientId, message, QStringLiteral("malformed"),
                  QStringLiteral("The protocol range is invalid."), true);
        return false;
    }
    const int selected = qMin(kProtocolMax, clientMax);
    if (selected < qMax(kProtocolMin, clientMin)) {
        sendError(clientId, message, QStringLiteral("protocol_incompatible"),
                  QStringLiteral("This client and GameHQ do not share a protocol version."), true);
        return false;
    }

    it->handshaken = true;
    it->name = clientName;
    it->protocol = selected;
    if (clientName == QStringLiteral("GameHQ.Playnite"))
        m_context.cancelSourceExpiry(clientName);
    qInfo().noquote() << "Integration client" << clientId << "handshake:" << clientName
                      << clientVersion;

    QJsonObject reply {
        { QStringLiteral("type"), QStringLiteral("hello.ack") },
        { QStringLiteral("appVersion"), m_appVersion },
        { QStringLiteral("protocolMin"), kProtocolMin },
        { QStringLiteral("protocolMax"), kProtocolMax },
        { QStringLiteral("protocolSelected"), selected },
        { QStringLiteral("capabilities"), QJsonArray {
              QStringLiteral("app.activate"), QStringLiteral("app.open_gallery"),
              QStringLiteral("game.lifecycle.v1"), QStringLiteral("status.v1") } }
    };
    copyRequestId(message, reply);
    m_server.send(clientId, reply);
    return true;
}

void IntegrationService::onMessage(quint64 clientId,
                                   const integration::Message &message)
{
    auto it = m_clients.find(clientId);
    if (it == m_clients.end())
        return;
    if (message.type == QStringLiteral("hello")) {
        if (it->handshaken) {
            sendError(clientId, message, QStringLiteral("malformed"),
                      QStringLiteral("The client has already completed its handshake."), true);
            return;
        }
        handleHello(clientId, message);
        return;
    }
    if (!it->handshaken) {
        sendError(clientId, message, QStringLiteral("not_handshaken"),
                  QStringLiteral("Send hello before any other message."));
        return;
    }
    if (message.type == QStringLiteral("app.activate")) {
        emit activateRequested();
        sendAck(clientId, message);
    } else if (message.type == QStringLiteral("app.open_gallery")) {
        emit openGalleryRequested();
        sendAck(clientId, message);
    } else if (message.type == QStringLiteral("status.request")) {
        QJsonObject reply {
            { QStringLiteral("type"), QStringLiteral("status.response") },
            { QStringLiteral("appVersion"), m_appVersion },
            { QStringLiteral("protocol"), it->protocol },
            { QStringLiteral("externalGameCount"), m_context.sessionCount() },
            { QStringLiteral("maintenance"), m_maintenance }
        };
        copyRequestId(message, reply);
        m_server.send(clientId, reply);
    } else if (message.type == QStringLiteral("client.goodbye")) {
        m_server.disconnectClient(clientId);
    } else {
        handleLifecycle(clientId, message);
    }
}

bool IntegrationService::handleLifecycle(quint64 clientId,
                                         const integration::Message &message)
{
    const Client client = m_clients.value(clientId);
    if (client.name != QStringLiteral("GameHQ.Playnite")) {
        sendError(clientId, message, QStringLiteral("unavailable"),
                  QStringLiteral("Game lifecycle messages require the Playnite client."));
        return false;
    }
    QString error;
    bool accepted = true;
    if (message.type == QStringLiteral("playnite.application.started")) {
        m_context.cancelSourceExpiry(client.name);
    } else if (message.type == QStringLiteral("playnite.application.stopping")) {
        m_context.clearSource(client.name);
    } else if (message.type == QStringLiteral("playnite.game.starting")) {
        accepted = m_context.upsert(client.name, message.object, QStringLiteral("starting"), error);
    } else if (message.type == QStringLiteral("playnite.game.started")) {
        accepted = m_context.upsert(client.name, message.object, QStringLiteral("started"), error);
    } else if (message.type == QStringLiteral("playnite.game.stopped")
               || message.type == QStringLiteral("playnite.game.startup_cancelled")) {
        QString sessionId;
        accepted = boundedRequiredString(message.object, QStringLiteral("sessionId"),
                                         128, sessionId);
        if (accepted)
            m_context.remove(client.name, sessionId);
        else
            error = QStringLiteral("sessionId is missing or invalid");
    } else if (message.type == QStringLiteral("playnite.state.sync")) {
        const QJsonValue games = message.object.value(QStringLiteral("games"));
        if (!games.isArray()) {
            accepted = false;
            error = QStringLiteral("games must be an array");
        } else {
            accepted = m_context.replaceSource(client.name, games.toArray(), error);
        }
    } else {
        accepted = false;
        error = QStringLiteral("message type is unavailable");
    }
    if (!accepted) {
        sendError(clientId, message, QStringLiteral("malformed"), error);
        return false;
    }
    sendAck(clientId, message);
    return true;
}

void IntegrationService::sendAck(quint64 clientId,
                                 const integration::Message &request)
{
    QJsonObject reply {
        { QStringLiteral("type"), QStringLiteral("ack") },
        { QStringLiteral("acceptedType"), request.type }
    };
    copyRequestId(request, reply);
    m_server.send(clientId, reply);
}

void IntegrationService::sendError(quint64 clientId,
                                   const integration::Message &request,
                                   const QString &code, const QString &text,
                                   bool disconnect)
{
    QJsonObject reply {
        { QStringLiteral("type"), QStringLiteral("error") },
        { QStringLiteral("code"), code },
        { QStringLiteral("message"), text }
    };
    copyRequestId(request, reply);
    m_server.send(clientId, reply);
    if (disconnect)
        m_server.disconnectClient(clientId);
}

void IntegrationService::broadcastMaintenance(int retryAfterSeconds)
{
    m_maintenance = true;
    const QJsonObject message {
        { QStringLiteral("type"), QStringLiteral("app.maintenance") },
        { QStringLiteral("reason"), QStringLiteral("update") },
        { QStringLiteral("retryAfterSeconds"), qBound(1, retryAfterSeconds, 3600) }
    };
    // send() can reject a client whose queue overflowed, which removes it from
    // m_clients through clientDisconnected. Snapshot the recipients first so
    // the loop never walks a container the send is mutating.
    QList<quint64> recipients;
    recipients.reserve(m_clients.size());
    for (auto it = m_clients.cbegin(); it != m_clients.cend(); ++it) {
        if (it->handshaken)
            recipients.push_back(it.key());
    }
    for (quint64 clientId : std::as_const(recipients))
        m_server.send(clientId, message);
}
