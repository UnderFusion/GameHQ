#pragma once

#include "share/ShareProvider.h"
#include "share/providers/DiscordWebhookStore.h"
#include "share/providers/DiscordWebhookUpload.h"

#include <QPointer>

#include <memory>

namespace share
{

// Discord channel destinations through incoming webhooks (plan t17). The
// capture is uploaded natively (multipart file, no link, no third-party host)
// and the message appears in the channel under the webhook's identity, NOT as
// the user's personal Discord account. A webhook can only post to its one
// channel (`access: share_token`).
//
// Outcome rules (docs/share-platform.md):
//  - `sent` only from a 2xx response to `?wait=true` that carries the created
//    message id;
//  - a definite rejection (revoked webhook, rate limit, too large) is `failed`;
//  - anything ambiguous after the whole body left (5xx, dropped connection,
//    cancel at the last moment) is `unconfirmed` and is never retried here.
// The webhook URL is never logged, put in an error code, or returned.
class DiscordWebhookProvider : public Provider
{
    Q_OBJECT
public:
    // Local sanity ceiling only. Discord's real per-upload limit is not in the
    // webhook reference and changes with the server, so the server's 413 is
    // the authority; this just avoids uploading something absurd.
    static constexpr qint64 kSanityLimitBytes = 100LL * 1024 * 1024;

    explicit DiscordWebhookProvider(DiscordWebhookStore* store, QObject* parent = nullptr);
    // Production form: the provider owns its store.
    explicit DiscordWebhookProvider(std::unique_ptr<DiscordWebhookStore> store,
                                    QObject* parent = nullptr);

    QString id() const override { return QStringLiteral("discord.webhook"); }
    QString displayName() const override;
    QString iconSource() const override { return QStringLiteral(""); }   // Segoe Fluent: Chat
    Capabilities capabilities() const override
    {
        return Capability::Image | Capability::Video | Capability::Channels
             | Capability::DirectSend | Capability::SavedTargets;
    }
    Availability availability() const override;
    QString availabilityReason() const override;
    AccountAccess accountAccess() const override { return AccountAccess::ShareToken; }
    QString privacyNotice() const override;
    void disconnectAccount() override;
    int jobTimeoutMs() const override { return 60000; }

    QVector<SavedDestination> savedDestinations() const override;
    QString addSavedDestination(const QString& name, const QString& secret) override;
    bool removeSavedDestination(const QString& id) override;

    void requestTargets(const QString& queryId, const Request& request,
                        const QString& query) override;
    void start(const Job& job, const Request& request, const Target& target) override;
    void cancel(const QString& jobId) override;

    void setMaxUploadBytes(qint64 bytes) { m_maxUploadBytes = bytes; }

private:
    void finish(const Result& result);
    void onUploadFinished(int status, const QByteArray& body,
                          DiscordWebhookUpload::Failure failure);
    Result resultFor(int status, const QByteArray& body,
                     DiscordWebhookUpload::Failure failure) const;

    std::unique_ptr<DiscordWebhookStore> m_ownedStore;
    DiscordWebhookStore* m_store;
    qint64 m_maxUploadBytes = kSanityLimitBytes;

    // One job at a time (the service enforces it).
    QString m_jobId;
    QString m_destinationId;
    QPointer<DiscordWebhookUpload> m_upload;
};

} // namespace share
