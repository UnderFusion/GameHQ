#include "share/external/ExternalProviderProtocol.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QtEndian>

#include <array>

namespace share::external
{

namespace
{
constexpr std::array<const char*, 5> kProviderMessages = {
    "hello", "targets.result", "job.progress", "job.result", "goodbye",
};

constexpr std::array<const char*, 9> kDeclarable = {
    "image", "video", "contacts", "groups", "channels",
    "target_search", "direct_send", "external_handoff", "caption",
};

constexpr std::array<const char*, 2> kReserved = { "requires_account", "saved_targets" };
} // namespace

bool isDeclarableCapability(const QString& name)
{
    for (const char* n : kDeclarable) {
        if (name == QLatin1StringView(n))
            return true;
    }
    return false;
}

bool isReservedCapability(const QString& name)
{
    for (const char* n : kReserved) {
        if (name == QLatin1StringView(n))
            return true;
    }
    return false;
}

Capability capabilityFromName(const QString& name)
{
    if (name == QLatin1String("image"))            return Capability::Image;
    if (name == QLatin1String("video"))            return Capability::Video;
    if (name == QLatin1String("contacts"))         return Capability::Contacts;
    if (name == QLatin1String("groups"))           return Capability::Groups;
    if (name == QLatin1String("channels"))         return Capability::Channels;
    if (name == QLatin1String("target_search"))    return Capability::TargetSearch;
    if (name == QLatin1String("direct_send"))      return Capability::DirectSend;
    if (name == QLatin1String("external_handoff")) return Capability::ExternalHandoff;
    if (name == QLatin1String("caption"))          return Capability::Caption;
    return Capability::None;
}

bool isAllowedProviderMessage(const QString& type)
{
    for (const char* t : kProviderMessages) {
        if (type == QLatin1StringView(t))
            return true;
    }
    return false;
}

QString sanitizeText(const QString& text, int maxChars)
{
    QString out;
    out.reserve(qMin<qsizetype>(text.size(), maxChars));
    for (const QChar c : text) {
        // Control characters, line/paragraph separators and format characters
        // (bidi overrides) never reach the UI.
        const QChar::Category cat = c.category();
        if (c.isNull() || cat == QChar::Other_Control || cat == QChar::Other_Format
            || cat == QChar::Separator_Line || cat == QChar::Separator_Paragraph
            || cat == QChar::Other_Surrogate || cat == QChar::Other_PrivateUse
            || cat == QChar::Other_NotAssigned) {
            if (c == QLatin1Char('\t') || c == QLatin1Char('\n') || c == QLatin1Char('\r'))
                out.append(QLatin1Char(' '));
            continue;
        }
        out.append(c);
    }
    out = out.simplified();
    if (out.size() > maxChars)
        out.truncate(maxChars);
    return out;
}

bool isPlainId(const QString& text, int maxChars)
{
    if (text.isEmpty() || text.size() > maxChars)
        return false;
    static const QRegularExpression kPlain(QStringLiteral("^[A-Za-z0-9._:-]+$"));
    return kPlain.match(text).hasMatch();
}

QByteArray encodeFrame(const QJsonObject& object, QString& error)
{
    error.clear();
    const QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (payload.isEmpty() || payload.size() > qsizetype(kMaxFrameBytes)) {
        error = QStringLiteral("outbound payload exceeds the frame limit");
        return {};
    }
    QByteArray frame(4, Qt::Uninitialized);
    qToLittleEndian<quint32>(static_cast<quint32>(payload.size()), frame.data());
    frame.append(payload);
    return frame;
}

bool FrameDecoder::append(const QByteArray& bytes, QList<QJsonObject>& messages, QString& error)
{
    error.clear();
    if (bytes.size() > kMaxBufferedBytes - m_buffer.size()) {
        error = QStringLiteral("input queue exceeded its limit");
        return false;
    }
    m_buffer.append(bytes);

    while (m_buffer.size() >= 4) {
        const quint32 length = qFromLittleEndian<quint32>(m_buffer.constData());
        if (length == 0 || length > kMaxFrameBytes) {
            error = QStringLiteral("frame length is outside the allowed range");
            return false;
        }
        const qsizetype frameSize = 4 + static_cast<qsizetype>(length);
        if (m_buffer.size() < frameSize)
            return true;

        QStringDecoder decoder(QStringDecoder::Utf8);
        const QString text = decoder.decode(m_buffer.mid(4, length));
        if (decoder.hasError()) {
            error = QStringLiteral("payload is not valid UTF-8");
            return false;
        }
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            error = QStringLiteral("payload is not a JSON object");
            return false;
        }
        const QJsonValue type = doc.object().value(QStringLiteral("type"));
        if (!type.isString() || type.toString().isEmpty() || type.toString().size() > 64) {
            error = QStringLiteral("message type is missing or invalid");
            return false;
        }
        messages.append(doc.object());
        m_buffer.remove(0, frameSize);
    }
    return true;
}

bool parseHello(const QJsonObject& hello, Manifest* manifest, QString* errorCode)
{
    const auto fail = [&](const char* code) {
        *errorCode = QString::fromLatin1(code);
        return false;
    };

    // ── Protocol range ──────────────────────────────────────────────────
    const QJsonValue minV = hello.value(QStringLiteral("protocolMin"));
    const QJsonValue maxV = hello.value(QStringLiteral("protocolMax"));
    if (!minV.isDouble() || !maxV.isDouble())
        return fail(ErrorCode::InvalidManifest);
    const int pMin = minV.toInt(-1);
    const int pMax = maxV.toInt(-1);
    if (pMin < 1 || pMax < pMin || pMax > 1000 || double(pMin) != minV.toDouble()
        || double(pMax) != maxV.toDouble())
        return fail(ErrorCode::InvalidManifest);
    const int selected = qMin(pMax, kProtocolMax);
    if (selected < pMin || selected < kProtocolMin)
        return fail(ErrorCode::ProtocolIncompatible);

    // ── Identity and display data ───────────────────────────────────────
    const QJsonValue providerV = hello.value(QStringLiteral("provider"));
    if (!providerV.isObject())
        return fail(ErrorCode::InvalidManifest);
    const QJsonObject p = providerV.toObject();

    Manifest m;
    m.selectedProtocol = selected;

    const QJsonValue idV = p.value(QStringLiteral("id"));
    if (!idV.isString())
        return fail(ErrorCode::InvalidManifest);
    m.id = idV.toString();
    // "ext." keeps third parties out of the built-in namespace, so a provider
    // can never register as "telegram.desktop" or shadow a future built-in.
    if (!m.id.startsWith(QLatin1String("ext.")) || !isValidProviderId(m.id))
        return fail(ErrorCode::InvalidManifest);

    const QJsonValue nameV = p.value(QStringLiteral("name"));
    if (!nameV.isString())
        return fail(ErrorCode::InvalidManifest);
    m.name = sanitizeText(nameV.toString(), kMaxNameChars);
    if (m.name.isEmpty())
        return fail(ErrorCode::InvalidManifest);

    if (p.contains(QStringLiteral("version"))) {
        if (!p.value(QStringLiteral("version")).isString())
            return fail(ErrorCode::InvalidManifest);
        m.version = sanitizeText(p.value(QStringLiteral("version")).toString(), kMaxVersionChars);
    }
    if (p.contains(QStringLiteral("privacy"))) {
        if (!p.value(QStringLiteral("privacy")).isString())
            return fail(ErrorCode::InvalidManifest);
        m.privacy = sanitizeText(p.value(QStringLiteral("privacy")).toString(), kMaxPrivacyChars);
    }
    if (p.contains(QStringLiteral("access"))) {
        const QString a = p.value(QStringLiteral("access")).toString();
        if (a == QLatin1String("none"))
            m.access = AccountAccess::None;
        else if (a == QLatin1String("share_token"))
            m.access = AccountAccess::ShareToken;
        else if (a == QLatin1String("full_account"))
            m.access = AccountAccess::FullAccountSession;
        else
            return fail(ErrorCode::InvalidManifest);
    }
    if (p.contains(QStringLiteral("jobTimeoutMs"))) {
        const QJsonValue t = p.value(QStringLiteral("jobTimeoutMs"));
        if (!t.isDouble())
            return fail(ErrorCode::InvalidManifest);
        m.jobTimeoutMs = qBound(kMinJobTimeoutMs, t.toInt(kDefaultJobTimeoutMs), kMaxJobTimeoutMs);
    }

    // ── Capabilities ────────────────────────────────────────────────────
    const QJsonValue capsV = p.value(QStringLiteral("capabilities"));
    if (!capsV.isArray() || capsV.toArray().size() > 32)
        return fail(ErrorCode::InvalidManifest);
    for (const QJsonValue& v : capsV.toArray()) {
        if (!v.isString() || v.toString().size() > 32)
            return fail(ErrorCode::InvalidManifest);
        const QString name = v.toString();
        if (isDeclarableCapability(name)) {
            if (!m.acceptedCapabilityNames.contains(name)) {
                m.acceptedCapabilityNames.append(name);
                m.capabilities |= capabilityFromName(name);
            }
        } else if (!m.ignoredCapabilityNames.contains(name)) {
            // Reserved or simply unknown to this GameHQ: never an error, so a
            // newer provider still works with an older GameHQ.
            m.ignoredCapabilityNames.append(name);
        }
    }
    if (!m.capabilities.testAnyFlags(Capability::Image | Capability::Video))
        return fail(ErrorCode::NoMediaCapability);
    if (m.capabilities.testFlag(Capability::DirectSend)
        && m.capabilities.testFlag(Capability::ExternalHandoff))
        return fail(ErrorCode::InvalidCapabilities);

    *manifest = m;
    return true;
}

} // namespace share::external
