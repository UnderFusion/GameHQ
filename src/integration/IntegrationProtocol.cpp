#include "integration/IntegrationProtocol.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QStringDecoder>
#include <QtEndian>

#include <array>
#include <utility>

namespace
{
constexpr std::array<const char *, 12> kAllowedInboundTypes = {
    "hello",
    "app.activate",
    "app.open_gallery",
    "status.request",
    "playnite.application.started",
    "playnite.application.stopping",
    "playnite.game.starting",
    "playnite.game.started",
    "playnite.game.stopped",
    "playnite.game.startup_cancelled",
    "playnite.state.sync",
    "client.goodbye"
};
}

namespace integration
{
bool isAllowedInboundType(const QString &type)
{
    for (const char *allowed : kAllowedInboundTypes) {
        if (type == QLatin1StringView(allowed))
            return true;
    }
    return false;
}

bool parsePayload(const QByteArray &payload, Message &message, QString &error)
{
    error.clear();
    if (payload.isEmpty() || payload.size() > MaximumPayloadBytes) {
        error = QStringLiteral("frame length is outside the allowed range");
        return false;
    }

    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString text = decoder.decode(payload);
    if (decoder.hasError()) {
        error = QStringLiteral("payload is not valid UTF-8");
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(text.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        error = QStringLiteral("payload is not a JSON object");
        return false;
    }

    const QJsonObject object = document.object();
    const QJsonValue typeValue = object.value(QStringLiteral("type"));
    if (!typeValue.isString() || typeValue.toString().isEmpty()
        || typeValue.toString().size() > 96) {
        error = QStringLiteral("message type is missing or invalid");
        return false;
    }
    const QString type = typeValue.toString();
    if (!isAllowedInboundType(type)) {
        error = QStringLiteral("message type is not allowed");
        return false;
    }

    const QJsonValue requestValue = object.value(QStringLiteral("requestId"));
    if (!requestValue.isUndefined()
        && (!requestValue.isString() || requestValue.toString().size() > 256)) {
        error = QStringLiteral("requestId is invalid");
        return false;
    }

    message.type = type;
    message.requestId = requestValue.toString();
    message.object = object;
    return true;
}

QByteArray encodeFrame(const QJsonObject &object, QString &error)
{
    error.clear();
    const QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (payload.isEmpty() || payload.size() > MaximumPayloadBytes) {
        error = QStringLiteral("outbound payload exceeds the frame limit");
        return {};
    }
    QByteArray frame(4, Qt::Uninitialized);
    qToLittleEndian<quint32>(static_cast<quint32>(payload.size()), frame.data());
    frame.append(payload);
    return frame;
}

bool FrameDecoder::append(const QByteArray &bytes, QList<Message> &messages, QString &error)
{
    return append(bytes, messages, error, -1);
}

bool FrameDecoder::hasCompleteFrame() const
{
    if (m_buffer.size() < 4)
        return false;
    const quint32 length = qFromLittleEndian<quint32>(m_buffer.constData());
    return m_buffer.size() >= 4 + static_cast<qsizetype>(length);
}

bool FrameDecoder::append(const QByteArray &bytes, QList<Message> &messages, QString &error,
                          int maxFrames)
{
    error.clear();
    if (bytes.size() > MaximumBufferedBytes - m_buffer.size()) {
        error = QStringLiteral("client input queue exceeded its limit");
        return false;
    }
    m_buffer.append(bytes);

    int taken = 0;
    while (m_buffer.size() >= 4) {
        const quint32 length = qFromLittleEndian<quint32>(m_buffer.constData());
        if (length == 0 || length > MaximumPayloadBytes) {
            error = QStringLiteral("frame length is outside the allowed range");
            return false;
        }
        const qsizetype frameSize = 4 + static_cast<qsizetype>(length);
        if (m_buffer.size() < frameSize || (maxFrames >= 0 && taken >= maxFrames))
            return true;
        ++taken;

        Message message;
        if (!parsePayload(m_buffer.mid(4, length), message, error))
            return false;
        messages.push_back(std::move(message));
        m_buffer.remove(0, frameSize);
    }
    return true;
}

void FrameDecoder::clear()
{
    m_buffer.clear();
}
} // namespace integration
