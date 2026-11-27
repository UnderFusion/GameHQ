#pragma once

#include "integration/IntegrationMessage.h"

#include <QByteArray>
#include <QList>

namespace integration
{
inline constexpr quint32 MaximumPayloadBytes = 64 * 1024;
inline constexpr qsizetype MaximumBufferedBytes = 4 * (MaximumPayloadBytes + 4);

class FrameDecoder
{
public:
    bool append(const QByteArray &bytes, QList<Message> &messages, QString &error);
    // The same, but extracts at most maxFrames complete frames; the rest stay
    // buffered for the next call (bytes may be empty to continue draining).
    bool append(const QByteArray &bytes, QList<Message> &messages, QString &error,
                int maxFrames);
    bool hasCompleteFrame() const;
    qsizetype bufferedBytes() const { return m_buffer.size(); }
    void clear();

private:
    QByteArray m_buffer;
};

bool parsePayload(const QByteArray &payload, Message &message, QString &error);
QByteArray encodeFrame(const QJsonObject &object, QString &error);
bool isAllowedInboundType(const QString &type);
} // namespace integration
