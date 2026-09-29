#include "share/providers/DiscordWebhookProvider.h"

#include "localization/NativeText.h"

#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QUrlQuery>

namespace share
{

namespace
{
// Response bodies are tiny (a message object or an error); never buffer more.
constexpr qint64 kMaxResponseBytes = 256 * 1024;
// Discord API error code for "Request entity too large".
constexpr int kDiscordTooLargeCode = 40005;

} // namespace

DiscordWebhookProvider::DiscordWebhookProvider(DiscordWebhookStore* store, QObject* parent)
    : Provider(parent)
    , m_store(store)
{
}

DiscordWebhookProvider::DiscordWebhookProvider(std::unique_ptr<DiscordWebhookStore> store,
                                               QObject* parent)
    : Provider(parent)
    , m_ownedStore(std::move(store))
    , m_store(m_ownedStore.get())
{
}

QString DiscordWebhookProvider::displayName() const
{
    return NativeText::get(
        //: Share destination that posts the capture to a Discord channel the user configured.
        //% "Discord channel"
        QT_TRID_NOOP("gamehq.share.discord_webhook.name"), "Discord channel");
}

Availability DiscordWebhookProvider::availability() const
{
    return m_store->list().isEmpty() ? Availability::Disabled : Availability::Available;
}

QString DiscordWebhookProvider::availabilityReason() const
{
    if (availability() == Availability::Available)
        return {};
    return NativeText::get(
        //: Share destination is disabled until the user adds a Discord channel in Settings.
        //% "Add a Discord channel in Settings first."
        QT_TRID_NOOP("gamehq.share.discord_webhook.not_configured"),
        "Add a Discord channel in Settings first.");
}

QString DiscordWebhookProvider::privacyNotice() const
{
    return NativeText::get(
        //: Shown under the Discord channel destination. It posts as a webhook, not as the user.
        //% "Posts to the channel as a webhook, not as your Discord account. Everyone in the channel can see it."
        QT_TRID_NOOP("gamehq.share.discord_webhook.privacy"),
        "Posts to the channel as a webhook, not as your Discord account. Everyone in the channel can see it.");
}

void DiscordWebhookProvider::disconnectAccount()
{
    m_store->removeAll();
    emit stateChanged();
}

QVector<Provider::SavedDestination> DiscordWebhookProvider::savedDestinations() const
{
    QVector<SavedDestination> out;
    for (const DiscordWebhookStore::Destination& d : m_store->list())
        out.append({ d.id, d.name });
    return out;
}

QString DiscordWebhookProvider::addSavedDestination(const QString& name, const QString& secret)
{
    const QString code = m_store->add(name, secret);
    if (code.isEmpty())
        emit stateChanged();
    return code;
}

bool DiscordWebhookProvider::removeSavedDestination(const QString& id)
{
    const bool removed = m_store->remove(id);
    if (removed)
        emit stateChanged();
    return removed;
}

void DiscordWebhookProvider::requestTargets(const QString& queryId, const Request& request,
                                            const QString& query)
{
    Q_UNUSED(request);
    Q_UNUSED(query);
    QVector<Target> targets;
    for (const DiscordWebhookStore::Destination& d : m_store->list()) {
        Target t;
        t.id = d.id;
        t.kind = TargetKind::Channel;
        t.displayName = d.name;
        t.capabilities = Capability::Image | Capability::Video;
        targets.append(t);
    }
    emit targetsReady(queryId, targets, {});
}

void DiscordWebhookProvider::start(const Job& job, const Request& request, const Target& target)
{
    m_jobId = job.id;
    m_destinationId = target.id;

    const auto fail = [&](const char* code) {
        Result r;
        r.jobId = job.id;
        r.outcome = Outcome::Failed;
        r.errorCode = QString::fromLatin1(code);
        finish(r);
    };

    if (request.sizeBytes() > m_maxUploadBytes)
        return fail("too_large");
    const QString url = m_store->webhookUrl(target.id);
    if (url.isEmpty())
        return fail("webhook_missing");   // secret deleted behind our back

    // wait=true makes Discord answer with the created message, which is the
    // only thing that lets this provider say "sent".
    QUrl endpoint(url);
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("wait"), QStringLiteral("true"));
    endpoint.setQuery(query);

    // No text and no mentions: the message is just the attachment.
    QJsonObject payload;
    payload.insert(QStringLiteral("allowed_mentions"),
                   QJsonObject{ { QStringLiteral("parse"), QJsonArray() } });
    payload.insert(QStringLiteral("attachments"),
                   QJsonArray{ QJsonObject{ { QStringLiteral("id"), 0 },
                                            { QStringLiteral("filename"), request.fileName() } } });

    emit jobProgress(job.id, JobState::Preparing, -1.0);
    auto* upload = new DiscordWebhookUpload(this);
    m_upload = upload;
    connect(upload, &DiscordWebhookUpload::progress, this, [this, upload](qint64 sent, qint64 total) {
        if (upload == m_upload)
            emit jobProgress(m_jobId, JobState::Transferring,
                             total > 0 ? double(sent) / double(total) : -1.0);
    });
    connect(upload, &DiscordWebhookUpload::finished, this,
            [this, upload](int status, const QByteArray& body, DiscordWebhookUpload::Failure failure) {
                if (upload != m_upload)
                    return;
                onUploadFinished(status, body, failure);
            });
    // From here data may leave the PC, so a cancel must wait for the verdict.
    emit jobProgress(job.id, JobState::Transferring, 0.0);
    upload->start(endpoint, QJsonDocument(payload).toJson(QJsonDocument::Compact),
                  { request.filePath(), request.fileName(), request.mimeType() });
}

void DiscordWebhookProvider::cancel(const QString& jobId)
{
    if (jobId != m_jobId || !m_upload)
        return;
    m_upload->abort();   // finished() then reports what is actually known
}

Result DiscordWebhookProvider::resultFor(int status, const QByteArray& body,
                                         DiscordWebhookUpload::Failure failure) const
{
    using Failure = DiscordWebhookUpload::Failure;
    Result r;
    r.jobId = m_jobId;

    // The uploader reports exactly how far the request got: if the whole
    // request never left, no message can exist.
    const bool wholeRequestSent = m_upload && m_upload->requestFullySent();

    switch (failure) {
    case Failure::Aborted:
        if (wholeRequestSent) {
            r.outcome = Outcome::Unconfirmed;
            r.errorCode = QStringLiteral("cancelled_late");
        } else {
            r.outcome = Outcome::Cancelled;
        }
        return r;
    case Failure::Connect:
    case Failure::Tls:
        r.outcome = Outcome::Failed;
        r.errorCode = QStringLiteral("network_error");
        return r;
    case Failure::Io:
        // A dropped connection is only ambiguous once the whole body was out.
        r.outcome = wholeRequestSent ? Outcome::Unconfirmed : Outcome::Failed;
        r.errorCode = QStringLiteral("network_error");
        return r;
    case Failure::None:
        break;
    }

    const QJsonObject json = QJsonDocument::fromJson(body).object();

    if (status >= 200 && status < 300) {
        // Only a body carrying the created message's id proves delivery.
        if (json.value(QStringLiteral("id")).toString().isEmpty()) {
            r.outcome = Outcome::Unconfirmed;
            r.errorCode = QStringLiteral("unexpected_response");
        } else {
            r.outcome = Outcome::Sent;
        }
        return r;
    }

    r.outcome = Outcome::Failed;
    if (status == 401 || status == 403 || status == 404) {
        r.errorCode = QStringLiteral("webhook_revoked");
    } else if (status == 413 || json.value(QStringLiteral("code")).toInt() == kDiscordTooLargeCode) {
        r.errorCode = QStringLiteral("too_large");
    } else if (status == 429) {
        r.errorCode = QStringLiteral("rate_limited");
    } else if (status >= 500) {
        // The server may or may not have created the message.
        r.outcome = Outcome::Unconfirmed;
        r.errorCode = QStringLiteral("server_error");
    } else {
        r.errorCode = QStringLiteral("rejected");
    }
    return r;
}

void DiscordWebhookProvider::onUploadFinished(int status, const QByteArray& body,
                                              DiscordWebhookUpload::Failure failure)
{
    Result r = resultFor(status, body, failure);
    if (m_upload)
        m_upload->deleteLater();
    m_upload.clear();
    if (r.outcome == Outcome::Sent)
        m_store->markUsed(m_destinationId);
    finish(r);
}

void DiscordWebhookProvider::finish(const Result& result)
{
    // Codes and outcomes only: the URL, token and response text stay out.
    qInfo() << "Share: Discord channel upload ended" << outcomeName(result.outcome)
            << result.errorCode;
    m_jobId.clear();
    emit jobFinished(result);
}

} // namespace share
