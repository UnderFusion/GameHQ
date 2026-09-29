#include "share/providers/DiscordWebhookStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUrl>
#include <QUuid>
#include <QtGlobal>

#include <algorithm>

namespace share
{

namespace
{
const QString kProviderId = QStringLiteral("discord.webhook");
} // namespace

DiscordWebhookStore::DiscordWebhookStore(QString metadataPath, SecretStore secrets,
                                         UrlValidator validator)
    : m_path(std::move(metadataPath))
    , m_secrets(std::move(secrets))
    , m_validator(validator ? std::move(validator) : UrlValidator(&isDiscordWebhookUrl))
{
}

bool DiscordWebhookStore::isDiscordWebhookUrl(const QString& text)
{
    const QUrl url(text.trimmed(), QUrl::StrictMode);
    if (!url.isValid() || url.scheme() != QLatin1String("https"))
        return false;
    if (!url.userInfo().isEmpty() || url.port() != -1 || url.hasQuery() || url.hasFragment())
        return false;
    const QString host = url.host().toLower();
    static const QStringList kHosts = {
        QStringLiteral("discord.com"), QStringLiteral("discordapp.com"),
        QStringLiteral("ptb.discord.com"), QStringLiteral("canary.discord.com"),
    };
    if (!kHosts.contains(host))
        return false;
    static const QRegularExpression kPath(
        QStringLiteral("^/api(/v[0-9]{1,2})?/webhooks/[0-9]{15,25}/[A-Za-z0-9_-]{20,120}/?$"));
    return kPath.match(url.path(QUrl::FullyEncoded)).hasMatch();
}

QString DiscordWebhookStore::secretName(const QString& id)
{
    return QStringLiteral("dest-") + id;
}

QVector<DiscordWebhookStore::Destination> DiscordWebhookStore::load() const
{
    QVector<Destination> out;
    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly))
        return out;
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    static const QRegularExpression kId(QStringLiteral("^[0-9a-f]{32}$"));
    for (const QJsonValue& v : root.value(QStringLiteral("destinations")).toArray()) {
        const QJsonObject o = v.toObject();
        Destination d;
        d.id = o.value(QStringLiteral("id")).toString();
        d.name = o.value(QStringLiteral("name")).toString();
        d.lastUsedMs = static_cast<qint64>(o.value(QStringLiteral("lastUsedMs")).toDouble());
        if (kId.match(d.id).hasMatch() && !d.name.isEmpty())
            out.append(d);
    }
    return out;
}

bool DiscordWebhookStore::save(const QVector<Destination>& all) const
{
    QJsonArray array;
    for (const Destination& d : all)
        array.append(QJsonObject{ { QStringLiteral("id"), d.id },
                                  { QStringLiteral("name"), d.name },
                                  { QStringLiteral("lastUsedMs"), static_cast<double>(d.lastUsedMs) } });
    QDir().mkpath(QFileInfo(m_path).absolutePath());
    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(QJsonObject{ { QStringLiteral("version"), 1 },
                                          { QStringLiteral("destinations"), array } })
                   .toJson(QJsonDocument::Indented));
    return file.commit();
}

QVector<DiscordWebhookStore::Destination> DiscordWebhookStore::list() const
{
    QVector<Destination> all = load();
    // Stable: never-used destinations keep their add order at the end.
    std::stable_sort(all.begin(), all.end(), [](const Destination& a, const Destination& b) {
        return a.lastUsedMs > b.lastUsedMs;
    });
    return all;
}

QString DiscordWebhookStore::add(const QString& name, const QString& url, QString* id)
{
    const QString cleanName = name.simplified();
    if (cleanName.isEmpty() || cleanName.size() > kMaxNameLength)
        return QStringLiteral("invalid_name");
    const QString cleanUrl = url.trimmed();
    if (!m_validator(cleanUrl) || cleanUrl.toUtf8().size() > SecretStore::kMaxSecretBytes)
        return QStringLiteral("invalid_secret");
    QVector<Destination> all = load();
    if (all.size() >= kMaxDestinations)
        return QStringLiteral("too_many");

    Destination d;
    d.id = QUuid::createUuid().toString(QUuid::Id128);
    d.name = cleanName;
    if (!m_secrets.write(kProviderId, secretName(d.id), cleanUrl.toUtf8()))
        return QStringLiteral("storage_failed");
    all.append(d);
    if (!save(all)) {
        m_secrets.remove(kProviderId, secretName(d.id));   // no orphan credential
        return QStringLiteral("storage_failed");
    }
    if (id)
        *id = d.id;
    return {};
}

bool DiscordWebhookStore::remove(const QString& id)
{
    QVector<Destination> all = load();
    const auto it = std::find_if(all.begin(), all.end(),
                                 [&](const Destination& d) { return d.id == id; });
    if (it == all.end())
        return false;
    all.erase(it);
    m_secrets.remove(kProviderId, secretName(id));
    return save(all);
}

int DiscordWebhookStore::removeAll()
{
    const int count = static_cast<int>(load().size());
    // Every secret of the provider, including any whose metadata row was lost.
    m_secrets.removeAll(kProviderId);
    QFile::remove(m_path);
    return count;
}

QString DiscordWebhookStore::webhookUrl(const QString& id) const
{
    bool found = false;
    const QByteArray secret = m_secrets.read(kProviderId, secretName(id), &found);
    return found ? QString::fromUtf8(secret) : QString();
}

void DiscordWebhookStore::markUsed(const QString& id)
{
    QVector<Destination> all = load();
    for (Destination& d : all) {
        if (d.id == id) {
            d.lastUsedMs = QDateTime::currentMSecsSinceEpoch();
            save(all);
            return;
        }
    }
}

} // namespace share
