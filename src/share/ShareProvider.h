#pragma once

#include "share/ShareTypes.h"

#include <QObject>
#include <QString>
#include <QVector>

namespace share
{

// The one contract every Share destination implements: Telegram, Discord,
// webhooks, and later out-of-process providers bridged by the External
// Provider API. The UI knows providers only through this surface, so adding
// one never needs provider-specific QML.
//
// Rules a provider must follow (enforced where the service can):
//  - It only ever touches the file of the Request it is handed.
//  - It reports Outcome::Sent only after the remote side confirmed delivery.
//    A hand-off to another app is HandedOff; an ambiguous end is Unconfirmed.
//  - It never retries a send on its own after an ambiguous failure.
//  - errorCode/detail and log lines never carry tokens, webhook URLs, session
//    material or message contents.
//  - All calls and signals happen on the GUI thread; long work is the
//    provider's own business (worker, async I/O) and reports back by signal.
class Provider : public QObject
{
    Q_OBJECT
public:
    explicit Provider(QObject* parent = nullptr) : QObject(parent) {}
    ~Provider() override = default;

    virtual QString id() const = 0;             // see isValidProviderId()
    virtual QString displayName() const = 0;    // already localized
    virtual QString iconSource() const { return {}; }   // qrc/file URL for QML
    virtual Capabilities capabilities() const = 0;
    virtual AuthState authState() const { return AuthState::NotRequired; }
    virtual Availability availability() const { return Availability::Available; }
    // Localized, user-safe reason when availability() != Available.
    virtual QString availabilityReason() const { return {}; }
    // Privacy boundary (t11): what the provider can reach and a short,
    // localized plain-language notice the Share UI shows next to it.
    virtual AccountAccess accountAccess() const { return AccountAccess::None; }
    virtual QString privacyNotice() const { return {}; }
    // Called on Disconnect: drop the session, secrets and local session data.
    // Providers without an account need nothing here.
    virtual void disconnectAccount() {}
    // Providers with Capability::SavedTargets keep user-configured
    // destinations (webhooks, folders). The Settings UI manages them through
    // these three calls only; `secret` never comes back out. The return value
    // of add is "" on success, else a stable code ("invalid_name",
    // "invalid_secret", "too_many", "storage_failed").
    struct SavedDestination
    {
        QString id;
        QString name;
        bool pinned = false;   // pinned destinations are listed first
    };
    virtual QVector<SavedDestination> savedDestinations() const { return {}; }
    virtual QString addSavedDestination(const QString& name, const QString& secret)
    {
        Q_UNUSED(name);
        Q_UNUSED(secret);
        return QStringLiteral("unsupported");
    }
    virtual bool removeSavedDestination(const QString& id)
    {
        Q_UNUSED(id);
        return false;
    }
    // Rename returns "" on success, else a stable code ("invalid_name",
    // "not_found"). Pinning keeps the secret and id untouched.
    virtual QString renameSavedDestination(const QString& id, const QString& name)
    {
        Q_UNUSED(id);
        Q_UNUSED(name);
        return QStringLiteral("unsupported");
    }
    virtual bool setSavedDestinationPinned(const QString& id, bool pinned)
    {
        Q_UNUSED(id);
        Q_UNUSED(pinned);
        return false;
    }
    // How long a started job may stay silent before the service ends it as
    // Unconfirmed. Hand-offs are quick; uploads get longer.
    virtual int jobTimeoutMs() const { return 120000; }

    // Lists targets for a request. Must answer with targetsReady(queryId, ...)
    // exactly once, synchronously or later. `query` is empty for "default
    // list"; providers without TargetSearch may ignore it.
    virtual void requestTargets(const QString& queryId, const Request& request,
                                const QString& query) = 0;

    // Starts delivering `request` to `target`. Must end with exactly one
    // jobFinished() for job.id unless cancel() arrived first.
    virtual void start(const Job& job, const Request& request, const Target& target) = 0;

    // Best effort. The service reports Cancelled itself only if nothing left
    // GameHQ yet; otherwise it waits for the provider's own result.
    virtual void cancel(const QString& jobId) { Q_UNUSED(jobId); }

signals:
    void targetsReady(const QString& queryId, const QVector<share::Target>& targets,
                      const QString& errorCode);
    void jobProgress(const QString& jobId, share::JobState state, double progress);
    void jobFinished(const share::Result& result);
    // Availability, auth state or display data changed.
    void stateChanged();
};

} // namespace share
