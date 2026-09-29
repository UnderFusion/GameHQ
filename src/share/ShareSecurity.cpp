#include "share/ShareSecurity.h"

#include "share/ShareTypes.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QDebug>

#include <windows.h>
#include <wincred.h>

namespace share
{

QString redactSecrets(const QString& text)
{
    static const QString kMark = QStringLiteral("<redacted>");
    struct Rule { QRegularExpression pattern; QString replacement; };
    static const Rule kRules[] = {
        // Discord webhooks: keep the id (useful for support), drop the token.
        { QRegularExpression(QStringLiteral(
              R"((https?://(?:[a-z]+\.)?discord(?:app)?\.com/api(?:/v\d+)?/webhooks/\d+/)[A-Za-z0-9_\-\.]+)"),
              QRegularExpression::CaseInsensitiveOption),
          QStringLiteral("\\1") + kMark },
        // Telegram bot tokens (123456789:AA...).
        { QRegularExpression(QStringLiteral(R"(\b\d{6,12}:[A-Za-z0-9_\-]{30,}\b)")), kMark },
        // Authorization headers / bearer values.
        { QRegularExpression(QStringLiteral(R"(\b(Bearer|Bot|Basic)\s+[A-Za-z0-9_\-\.=+/]{8,})"),
                             QRegularExpression::CaseInsensitiveOption),
          QStringLiteral("\\1 ") + kMark },
        // Secret-looking query/form parameters.
        { QRegularExpression(QStringLiteral(
              R"(([?&;](?:access_token|refresh_token|token|key|api_key|apikey|secret|client_secret|password|auth|code|sig|signature)=)[^&#\s]+)"),
              QRegularExpression::CaseInsensitiveOption),
          QStringLiteral("\\1") + kMark },
        // "token": "...", password=... in JSON-ish or key=value text.
        { QRegularExpression(QStringLiteral(
              R"re(("?(?:access_token|refresh_token|token|api_key|secret|client_secret|password|session)"?\s*[:=]\s*"?)[^"\s,}&]+)re"),
              QRegularExpression::CaseInsensitiveOption),
          QStringLiteral("\\1") + kMark },
        // Long opaque key-like runs (hex/base64url), e.g. session keys.
        { QRegularExpression(QStringLiteral(R"(\b[A-Za-z0-9_\-]{40,}\b)")), kMark },
    };
    QString out = text;
    for (const Rule& rule : kRules)
        out.replace(rule.pattern, rule.replacement);
    return out;
}

// ── SecretStore ─────────────────────────────────────────────────────────

SecretStore::SecretStore(QString ns)
    : m_namespace(std::move(ns))
{
}

bool SecretStore::validName(const QString& name)
{
    static const QRegularExpression kName(QStringLiteral("^[a-z0-9][a-z0-9._-]{0,63}$"));
    return kName.match(name).hasMatch();
}

QString SecretStore::target(const QString& providerId, const QString& name) const
{
    if (!isValidProviderId(providerId) || !validName(name))
        return {};
    return m_namespace + QLatin1Char('/') + providerId + QLatin1Char('/') + name;
}

bool SecretStore::write(const QString& providerId, const QString& name, const QByteArray& secret)
{
    const QString t = target(providerId, name);
    if (t.isEmpty() || secret.isEmpty() || secret.size() > kMaxSecretBytes)
        return false;
    const std::wstring wideTarget = t.toStdWString();
    const std::wstring user = QStringLiteral("GameHQ").toStdWString();
    CREDENTIALW cred{};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = const_cast<LPWSTR>(wideTarget.c_str());
    cred.CredentialBlobSize = static_cast<DWORD>(secret.size());
    cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(secret.constData()));
    // Local to this user on this PC: account sessions must never roam.
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
    cred.UserName = const_cast<LPWSTR>(user.c_str());
    if (!CredWriteW(&cred, 0)) {
        qWarning() << "Share: could not store a secret for" << providerId << "error" << GetLastError();
        return false;
    }
    return true;
}

QByteArray SecretStore::read(const QString& providerId, const QString& name, bool* found) const
{
    if (found)
        *found = false;
    const QString t = target(providerId, name);
    if (t.isEmpty())
        return {};
    PCREDENTIALW cred = nullptr;
    if (!CredReadW(reinterpret_cast<LPCWSTR>(t.utf16()), CRED_TYPE_GENERIC, 0, &cred))
        return {};
    QByteArray out(reinterpret_cast<const char*>(cred->CredentialBlob),
                   static_cast<int>(cred->CredentialBlobSize));
    SecureZeroMemory(cred->CredentialBlob, cred->CredentialBlobSize);
    CredFree(cred);
    if (found)
        *found = true;
    return out;
}

bool SecretStore::remove(const QString& providerId, const QString& name)
{
    const QString t = target(providerId, name);
    if (t.isEmpty())
        return false;
    return CredDeleteW(reinterpret_cast<LPCWSTR>(t.utf16()), CRED_TYPE_GENERIC, 0) != FALSE;
}

QStringList SecretStore::names(const QString& providerId) const
{
    QStringList out;
    if (!isValidProviderId(providerId))
        return out;
    const QString prefix = m_namespace + QLatin1Char('/') + providerId + QLatin1Char('/');
    const QString filter = prefix + QLatin1Char('*');
    DWORD count = 0;
    PCREDENTIALW* creds = nullptr;
    if (!CredEnumerateW(reinterpret_cast<LPCWSTR>(filter.utf16()), 0, &count, &creds))
        return out;
    for (DWORD i = 0; i < count; ++i) {
        const QString t = QString::fromWCharArray(creds[i]->TargetName);
        if (t.startsWith(prefix))
            out.append(t.mid(prefix.size()));
    }
    CredFree(creds);
    return out;
}

int SecretStore::removeAll(const QString& providerId)
{
    int removed = 0;
    for (const QString& name : names(providerId)) {
        if (remove(providerId, name))
            ++removed;
    }
    return removed;
}

// ── SessionStorage ──────────────────────────────────────────────────────

SessionStorage::SessionStorage(QString root)
    : m_root(QDir::cleanPath(std::move(root)))
{
}

QString SessionStorage::directoryFor(const QString& providerId, bool create) const
{
    if (!isValidProviderId(providerId) || m_root.isEmpty())
        return {};
    const QString dir = m_root + QLatin1Char('/') + providerId;
    if (create)
        QDir().mkpath(dir);
    return dir;
}

bool SessionStorage::removeAll(const QString& providerId) const
{
    const QString dir = directoryFor(providerId, false);
    if (dir.isEmpty())
        return false;
    const QFileInfo info(dir);
    if (!info.exists())
        return true;
    // Never follow a link out of the session root.
    if (info.isSymLink() || info.isJunction() || !info.isDir())
        return false;
    const bool ok = QDir(dir).removeRecursively();
    if (!ok)
        qWarning() << "Share: could not remove session data for" << providerId;
    return ok;
}

} // namespace share
