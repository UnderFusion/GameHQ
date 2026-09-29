#pragma once

#include "share/ShareSecurity.h"

#include <QDateTime>
#include <QString>
#include <QVector>

#include <functional>

namespace share
{

// The user's configured Discord channel destinations (plan t17). The webhook
// URL is a credential: it lives only in the Windows Credential Manager (via
// SecretStore). The metadata file next to the config holds ids, names and a
// last-used time and nothing else, so it is safe to show in a support bundle.
class DiscordWebhookStore
{
public:
    struct Destination
    {
        QString id;
        QString name;
        qint64 lastUsedMs = 0;   // 0 = never used
        bool pinned = false;
    };
    // Decides whether a pasted URL may be stored and later posted to.
    using UrlValidator = std::function<bool(const QString& url)>;

    static constexpr int kMaxDestinations = 20;
    static constexpr int kMaxNameLength = 48;

    DiscordWebhookStore(QString metadataPath, SecretStore secrets, UrlValidator validator);

    // Pinned first, then the rest; inside each group most recently used
    // first, and never-used ones keep the order they were added in.
    QVector<Destination> list() const;
    // "" on success (and *id set), else "invalid_name", "invalid_secret",
    // "too_many" or "storage_failed".
    QString add(const QString& name, const QString& url, QString* id = nullptr);
    bool remove(const QString& id);
    // "" on success, else "invalid_name" or "not_found". Only the label
    // changes; the id, the secret and the pin state stay.
    QString rename(const QString& id, const QString& name);
    bool setPinned(const QString& id, bool pinned);
    int removeAll();
    // Empty when the destination or its secret is gone.
    QString webhookUrl(const QString& id) const;
    void markUsed(const QString& id);

    // The production rule: https, a Discord host, /api[/vN]/webhooks/<id>/<token>,
    // no query, fragment, userinfo or port.
    static bool isDiscordWebhookUrl(const QString& url);

private:
    QVector<Destination> load() const;
    bool save(const QVector<Destination>& all) const;
    static QString secretName(const QString& id);

    QString m_path;
    mutable SecretStore m_secrets;
    UrlValidator m_validator;
};

} // namespace share
