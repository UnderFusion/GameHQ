#include "updates/RemoteReleaseNotes.h"
#include "app/ReleaseNotes.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUrl>

namespace { constexpr qint64 kMaxBytes = 128 * 1024; }

RemoteReleaseNotes::RemoteReleaseNotes(QString owner, QString repo, QString cacheRoot,
                                       QObject *parent, QNetworkAccessManager *network)
    : QObject(parent), m_network(network ? network : new QNetworkAccessManager(this)),
      m_owner(std::move(owner)), m_repo(std::move(repo)), m_cacheRoot(std::move(cacheRoot)) {}

QVariantList RemoteReleaseNotes::blocks() const
{
    return ReleaseNotes::blocksFromMarkdown(m_markdown);
}

void RemoteReleaseNotes::present(const QString &markdown)
{
    m_markdown = markdown;
    Q_EMIT changed();
}

QString RemoteReleaseNotes::cachePath(const QString &locale) const
{
    const QByteArray key = (m_owner + '/' + m_repo + '/' + m_tag + '/' + locale).toUtf8();
    return QDir(m_cacheRoot).filePath(QString::fromLatin1(
        QCryptographicHash::hash(key, QCryptographicHash::Sha256).toHex()) + ".md");
}

bool RemoteReleaseNotes::validDocument(const QByteArray &bytes) const
{
    return !bytes.isEmpty() && bytes.size() <= kMaxBytes && !bytes.contains('\0')
        && QString::fromUtf8(bytes).startsWith(QStringLiteral("# GameHQ %1 (").arg(m_version))
        && !ReleaseNotes::blocksFromMarkdown(QString::fromUtf8(bytes)).isEmpty();
}

void RemoteReleaseNotes::select(const QString &version, const QString &tag,
                               const QString &body, const QString &locale)
{
    ++m_generation;
    m_version = version;
    m_tag = tag;
    static const QRegularExpression safeLocale(QStringLiteral("^[a-z]{2,3}(?:-[A-Za-z0-9]{2,8})*$"));
    m_locale = safeLocale.match(locale).hasMatch() ? locale : QStringLiteral("en-US");
    // The release body already arrived with discovery; show it without waiting
    // for the translated publication. Exclude the website's language index.
    m_body = body.left(kMaxBytes).section(QStringLiteral("<!-- gamehq:locale-index -->"), 0, 0).trimmed();
    m_markdown = m_body;
    static const QRegularExpression safeVersion(QStringLiteral("^[0-9]+\\.[0-9]+\\.[0-9]+$"));
    if (!safeVersion.match(version).hasMatch()
        || (tag != version && tag != "v" + version)) {
        m_loading = false;
        Q_EMIT changed();
        return;
    }
    const QString bodyPath = cachePath(QStringLiteral("discovery-body"));
    if (!m_body.isEmpty()) {
        QDir().mkpath(m_cacheRoot);
        QSaveFile cached(bodyPath);
        const QByteArray bytes = m_body.toUtf8();
        if (bytes.size() <= kMaxBytes && cached.open(QIODevice::WriteOnly)
            && cached.write(bytes) == bytes.size())
            cached.commit();
    } else {
        QFile cached(bodyPath);
        if (cached.open(QIODevice::ReadOnly) && cached.size() <= kMaxBytes)
            m_markdown = m_body = QString::fromUtf8(cached.readAll());
    }
    // A same-version cache is usable across restarts, including offline checks.
    for (const QString &candidate : {QStringLiteral("en-US"), m_locale}) {
        QFile cached(cachePath(candidate));
        if (cached.open(QIODevice::ReadOnly) && cached.size() <= kMaxBytes) {
            const QByteArray bytes = cached.readAll();
            if (validDocument(bytes))
                m_markdown = QString::fromUtf8(bytes);
        }
    }
    m_loading = true;
    Q_EMIT changed();
    fetch(m_locale, m_generation);
}

void RemoteReleaseNotes::retry()
{
    if (!m_loading && !m_version.isEmpty())
        select(m_version, m_tag, m_body, m_locale);
}

void RemoteReleaseNotes::fetch(const QString &locale, quint64 generation)
{
    QNetworkRequest request{QUrl(QStringLiteral(
        "https://raw.githubusercontent.com/%1/%2/%3/assets/release-notes/publication/%4/release-notes.%5.md")
        .arg(m_owner, m_repo, m_tag, m_version, locale))};
    request.setTransferTimeout(10000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("GameHQ-ReleaseNotes"));
    QNetworkReply *reply = m_network->get(request);
    // Enforce the limit during transfer as well as before parsing/caching.
    connect(reply, &QIODevice::readyRead, this, [reply] {
        if (reply->bytesAvailable() > kMaxBytes)
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, locale, generation] {
        reply->deleteLater();
        if (generation != m_generation)
            return;
        const QByteArray bytes = reply->read(kMaxBytes + 1);
        const bool ok = reply->error() == QNetworkReply::NoError
            && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200
            && validDocument(bytes);
        if (ok) {
            QDir().mkpath(m_cacheRoot);
            QSaveFile cached(cachePath(locale));
            if (cached.open(QIODevice::WriteOnly) && cached.write(bytes) == bytes.size())
                cached.commit();
            m_loading = false;
            present(QString::fromUtf8(bytes));
        } else if (locale != QStringLiteral("en-US") && m_markdown == m_body) {
            fetch(QStringLiteral("en-US"), generation);
        } else {
            // Keep the complete cached document or discovery body on failure.
            m_loading = false;
            Q_EMIT changed();
        }
    });
}
