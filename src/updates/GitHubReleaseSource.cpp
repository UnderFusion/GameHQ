#include "updates/GitHubReleaseSource.h"
#include "updates/ReleaseCatalog.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

namespace
{
constexpr int kTransferTimeoutMs = 15000;
constexpr int kMaxAttempts = 2; // one retry on a transient network error
}

GitHubReleaseSource::GitHubReleaseSource(QString owner, QString repo, QObject *parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
    , m_owner(std::move(owner))
    , m_repo(std::move(repo))
{
}

void GitHubReleaseSource::checkLatest(const QString &ifNoneMatchEtag)
{
    m_best.reset();
    m_firstPageEtag.clear();
    sendRequest(ifNoneMatchEtag, kMaxAttempts, 1);
}

void GitHubReleaseSource::sendRequest(const QString &ifNoneMatchEtag, int attemptsLeft, int page)
{
    const QString url = QStringLiteral("%1/repos/%2/%3/releases?per_page=%4&page=%5")
                            .arg(m_apiBase, m_owner, m_repo)
                            .arg(ReleaseCatalog::kPageSize)
                            .arg(page);
    QNetworkRequest request{QUrl(url)};
    request.setHeader(QNetworkRequest::UserAgentHeader,
                       QStringLiteral("GameHQ-UpdateChecker (+https://github.com/%1/%2)").arg(m_owner, m_repo));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    // The ETag belongs to page 1: a conditional request for a later page would
    // compare against the wrong resource.
    if (!ifNoneMatchEtag.isEmpty() && page == 1)
        request.setRawHeader("If-None-Match", ifNoneMatchEtag.toUtf8());
    request.setTransferTimeout(kTransferTimeoutMs);

    QNetworkReply *reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, ifNoneMatchEtag, attemptsLeft, page]() {
        reply->deleteLater();
        handleReply(reply, ifNoneMatchEtag, attemptsLeft, page);
    });
}

void GitHubReleaseSource::handleReply(QNetworkReply *reply, const QString &ifNoneMatchEtag,
                                       int attemptsLeft, int page)
{
    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    const ReleaseCatalog::RateLimit limit = ReleaseCatalog::rateLimitFrom(
        httpStatus, reply->rawHeader("x-ratelimit-remaining"), reply->rawHeader("x-ratelimit-reset"),
        reply->rawHeader("retry-after"), QDateTime::currentSecsSinceEpoch());
    if (limit.limited) {
        Q_EMIT rateLimited(limit.resetEpochSeconds);
        return;
    }

    if (httpStatus == 304) {
        Q_EMIT unchanged(ifNoneMatchEtag);
        return;
    }

    if (httpStatus == 404) {
        // A later page past the end is not a missing repository.
        if (page > 1)
            finish();
        else
            Q_EMIT notFound();
        return;
    }

    if (reply->error() != QNetworkReply::NoError || httpStatus != 200) {
        // Transient (no confirmed HTTP status yet, e.g. timeout/DNS failure):
        // retry once before giving up.
        const bool transient = httpStatus == 0;
        if (transient && attemptsLeft > 1) {
            QTimer::singleShot(1000, this, [this, ifNoneMatchEtag, attemptsLeft, page]() {
                sendRequest(ifNoneMatchEtag, attemptsLeft - 1, page);
            });
            return;
        }
        // Pages already read still hold a usable answer; only a failure on the
        // first page leaves nothing to report.
        if (page > 1 && m_best.has_value()) {
            finish();
            return;
        }
        Q_EMIT failed(reply->errorString());
        return;
    }

    const QByteArray body = reply->readAll();
    if (body.size() > 8 * 1024 * 1024) {
        Q_EMIT failed(QStringLiteral("release response is unreasonably large"));
        return;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (!doc.isArray()) {
        Q_EMIT failed(QStringLiteral("malformed release JSON"));
        return;
    }

    if (page == 1)
        m_firstPageEtag = QString::fromUtf8(reply->rawHeader("ETag"));

    const QJsonArray releases = doc.array();
    const auto candidate = ReleaseCatalog::selectBest(releases, m_includeBetas);
    // Pages arrive newest first, so the first page that yields a candidate
    // already holds the highest version; keep looking only while nothing has
    // been found. A repo that publishes a plugin release between every app
    // release used to push the app off a single 20-entry page entirely.
    if (candidate.has_value() && !m_best.has_value())
        m_best = candidate;

    if (!m_best.has_value() && ReleaseCatalog::mayHaveMorePages(releases.size(), page)) {
        sendRequest(ifNoneMatchEtag, kMaxAttempts, page + 1);
        return;
    }
    finish();
}

void GitHubReleaseSource::finish()
{
    if (!m_best.has_value()) {
        Q_EMIT notFound();
        return;
    }
    Q_EMIT succeeded(*m_best, m_firstPageEtag);
}
