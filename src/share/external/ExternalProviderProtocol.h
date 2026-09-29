#pragma once

#include "share/ShareTypes.h"

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

// Share Provider API v1 wire protocol (plan t18, docs/share-provider-api-v1.md).
// A dedicated same-user channel, deliberately separate from GameHQ.Local.v1 so
// the two allowlists, lifecycles and versions never constrain each other.
// Framing is the same proven shape: 4-byte little-endian length + UTF-8 JSON.
namespace share::external
{

inline constexpr auto kServerName = "GameHQ.Share.Provider.v1";
inline constexpr int kProtocolMin = 1;
inline constexpr int kProtocolMax = 1;

inline constexpr quint32 kMaxFrameBytes = 64 * 1024;
inline constexpr qsizetype kMaxBufferedBytes = 4 * (qsizetype(kMaxFrameBytes) + 4);

// Bounds applied to every provider-supplied value before GameHQ uses it.
inline constexpr int kMaxNameChars = 48;
inline constexpr int kMaxVersionChars = 32;
inline constexpr int kMaxPrivacyChars = 200;
inline constexpr int kMaxTargetIdChars = 128;
inline constexpr int kMaxTargetNameChars = 64;
inline constexpr int kMaxTargetSubtitleChars = 96;
inline constexpr int kMaxTargets = 200;
inline constexpr int kMaxQueryChars = 128;
inline constexpr int kMaxDetailChars = 200;
inline constexpr int kMinJobTimeoutMs = 1000;
inline constexpr int kMaxJobTimeoutMs = 600000;
inline constexpr int kDefaultJobTimeoutMs = 120000;

// Stable error codes carried by `error` messages (and reused as reasons in
// logs). Documented in the API specification.
namespace ErrorCode
{
inline constexpr auto ProtocolIncompatible = "protocol_incompatible";
inline constexpr auto UnknownType = "unknown_type";
inline constexpr auto Malformed = "malformed";
inline constexpr auto NotHandshaken = "not_handshaken";
inline constexpr auto AlreadyHandshaken = "already_handshaken";
inline constexpr auto DuplicateProvider = "duplicate_provider";
inline constexpr auto InvalidManifest = "invalid_manifest";
inline constexpr auto NoMediaCapability = "no_media_capability";
inline constexpr auto InvalidCapabilities = "invalid_capabilities";
inline constexpr auto InvalidField = "invalid_field";
inline constexpr auto StaleJob = "stale_job";
inline constexpr auto TooManyProviders = "too_many_providers";
inline constexpr auto HandshakeTimeout = "handshake_timeout";
inline constexpr auto TooManyViolations = "too_many_violations";
} // namespace ErrorCode

// Capabilities an external provider may declare. Everything else is either
// reserved for built-in providers (requires_account, saved_targets) or unknown
// to this GameHQ; both are ignored and reported back in `hello.ack`.
bool isDeclarableCapability(const QString& name);
bool isReservedCapability(const QString& name);
Capability capabilityFromName(const QString& name);

// Messages a provider may send. Anything else is `unknown_type`.
bool isAllowedProviderMessage(const QString& type);

class FrameDecoder
{
public:
    // Appends bytes and extracts complete frames as JSON objects that carry a
    // string `type`. Returns false on a broken stream (bad length, invalid
    // UTF-8/JSON, not an object, missing type, or an input queue over its
    // limit); the connection must then be dropped.
    bool append(const QByteArray& bytes, QList<QJsonObject>& messages, QString& error);

private:
    QByteArray m_buffer;
};

QByteArray encodeFrame(const QJsonObject& object, QString& error);

// Strips control characters and collapses whitespace, then cuts to maxChars.
QString sanitizeText(const QString& text, int maxChars);
// A token-like identifier: [A-Za-z0-9._:-]{1,maxChars}.
bool isPlainId(const QString& text, int maxChars);

struct Manifest
{
    QString id;               // "ext." + ...
    QString name;
    QString version;
    QString privacy;          // provider's own words, untrusted display text
    AccountAccess access = AccountAccess::FullAccountSession;   // undeclared = widest
    Capabilities capabilities;            // accepted subset only
    QStringList acceptedCapabilityNames;
    QStringList ignoredCapabilityNames;   // unknown or reserved
    int jobTimeoutMs = kDefaultJobTimeoutMs;
    int selectedProtocol = kProtocolMax;
};

// Validates a `hello`. On success fills *manifest. On failure *errorCode is one
// of ErrorCode::{ProtocolIncompatible, InvalidManifest, NoMediaCapability,
// InvalidCapabilities, InvalidField} and the connection must be closed.
bool parseHello(const QJsonObject& hello, Manifest* manifest, QString* errorCode);

} // namespace share::external
