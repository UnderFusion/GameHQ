#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

// Share Platform security boundaries (plan t11, docs/share-platform.md).
namespace share
{

// Replaces anything that looks like auth material with a marker: Discord
// webhook tokens, Telegram bot tokens, bearer/authorization values, secret
// query parameters and long opaque key-like runs. Use on every string a
// provider logs or shows that may have come from a server or from the user's
// own configuration. Idempotent.
QString redactSecrets(const QString& text);

// Per-user secret storage in the Windows Credential Manager (DPAPI-backed,
// never roams, never in config.json). Entries are named
// "<namespace>/<providerId>/<name>". One store per namespace; production uses
// the default, tests use a throwaway one.
class SecretStore
{
public:
    static QString defaultNamespace() { return QStringLiteral("GameHQ.Share"); }
    explicit SecretStore(QString ns = defaultNamespace());

    // Largest secret Windows accepts for a generic credential (2560 bytes).
    static constexpr int kMaxSecretBytes = 5 * 512;

    bool write(const QString& providerId, const QString& name, const QByteArray& secret);
    // Empty and *found=false when absent.
    QByteArray read(const QString& providerId, const QString& name, bool* found = nullptr) const;
    bool remove(const QString& providerId, const QString& name);
    // Names stored for a provider.
    QStringList names(const QString& providerId) const;
    // Deletes every secret of a provider (Disconnect). Returns how many went.
    int removeAll(const QString& providerId);

private:
    QString target(const QString& providerId, const QString& name) const;
    static bool validName(const QString& name);

    QString m_namespace;
};

// Local session data a provider keeps between runs (e.g. an account
// library's database), one directory per provider under a fixed root.
class SessionStorage
{
public:
    explicit SessionStorage(QString root);

    // "<root>/<providerId>", created on demand; empty for an invalid id.
    QString directoryFor(const QString& providerId, bool create = true) const;
    // Deletes that provider's directory and everything in it. Refuses any id
    // that is not a valid provider id, so nothing outside the root can go.
    bool removeAll(const QString& providerId) const;

private:
    QString m_root;
};

} // namespace share
