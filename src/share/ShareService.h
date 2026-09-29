#pragma once

#include "share/ShareProviderRegistry.h"
#include "share/ShareTypes.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

class QTimer;

namespace share
{

// Orchestrates one Share session at a time and is the only thing QML talks
// to (desktop gallery, lightbox and overlay all use this same object).
//
// Safety rules it enforces for every provider:
//  - the request is frozen when Share opens; a job starts only if the file is
//    still byte-for-byte the same size/mtime at the same path;
//  - a target id is accepted only from the provider's latest answer for this
//    request, so a stale list can never redirect a send;
//  - one job at a time, and a second send of the same capture to the same
//    target after Sent/Unconfirmed needs explicit confirmation (no duplicate
//    sends, no blind retries);
//  - a provider without DirectSend can never report Sent (coerced to
//    HandedOff); a silent job ends as Unconfirmed, never Sent or Failed;
//  - logs name the provider, outcome, error code and media kind only.
class Service : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY sessionChanged)
    Q_PROPERTY(QString fileName READ fileName NOTIFY sessionChanged)
    Q_PROPERTY(QString mediaKind READ mediaKindName NOTIFY sessionChanged)
    Q_PROPERTY(QString phase READ phaseName NOTIFY phaseChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY phaseChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(QVariantMap lastResult READ lastResult NOTIFY lastResultChanged)
    Q_PROPERTY(QString targetsProviderId READ targetsProviderId NOTIFY targetsChanged)
    Q_PROPERTY(bool targetsLoading READ targetsLoading NOTIFY targetsChanged)

public:
    enum class Phase { Idle, Choosing, Sending, Finished };

    explicit Service(QObject* parent = nullptr);
    ~Service() override;

    ProviderRegistry* registry() const { return m_registry; }

    // Opens a session for one capture. Fails (false, lastError set) for a
    // missing/unsupported file; any previous finished session is replaced.
    Q_INVOKABLE bool open(const QString& filePath, const QString& gameName = QString());
    // Ends the session. Refused while a job is transferring (cancel first).
    Q_INVOKABLE bool close();

    // Providers that accept this capture's media, in registry order:
    // { id, name, icon, available, reason, auth, capabilities: [..] }.
    Q_INVOKABLE QVariantList providers() const;

    // Asks one provider for targets; the answer arrives as targetsChanged().
    Q_INVOKABLE bool requestTargets(const QString& providerId, const QString& query = QString());
    // Latest targets: { id, providerId, kind, name, subtitle }.
    Q_INVOKABLE QVariantList targets() const;

    // Starts the send. Returns the job id, or "" with lastError set.
    Q_INVOKABLE QString share(const QString& providerId, const QString& targetId,
                              bool confirmResend = false);
    Q_INVOKABLE void cancel();
    // Disconnect (t11): the provider drops its session, secrets and local
    // session data. Refused while that provider has a job running.
    Q_INVOKABLE bool disconnectProvider(const QString& providerId);

    bool active() const { return m_request.isValid(); }
    QString fileName() const { return m_request.fileName(); }
    QString mediaKindName() const;
    Phase phase() const { return m_phase; }
    QString phaseName() const;
    bool busy() const { return m_phase == Phase::Sending; }
    QString lastError() const { return m_lastError; }
    QVariantMap lastResult() const { return m_lastResult; }
    QString targetsProviderId() const { return m_targetsProviderId; }
    bool targetsLoading() const { return !m_pendingQueryId.isEmpty(); }
    const Request& request() const { return m_request; }

    static QVariantMap resultToVariant(const Result& result);

signals:
    void sessionChanged();
    void phaseChanged();
    void lastErrorChanged();
    void lastResultChanged();
    void targetsChanged();
    void providersChanged();
    void finished(const QVariantMap& result);

private:
    struct ActiveJob
    {
        Job job;
        Target target;
        QPointer<Provider> provider;
        QString dedupeKey;
    };

    void setPhase(Phase phase);
    void setLastError(const QString& code);
    void onTargetsReady(Provider* provider, const QString& queryId,
                        const QVector<Target>& targets, const QString& errorCode);
    void onJobProgress(Provider* provider, const QString& jobId, JobState state, double progress);
    void onJobFinished(Provider* provider, const Result& result);
    void finishActive(Result result);
    void attach(Provider* provider);
    QString dedupeKey(const QString& providerId, const QString& targetId) const;

    ProviderRegistry* m_registry;
    Request m_request;
    Phase m_phase = Phase::Idle;
    QString m_lastError;
    QVariantMap m_lastResult;

    QString m_targetsProviderId;
    QString m_pendingQueryId;
    QVector<Target> m_targets;

    bool m_hasActive = false;
    ActiveJob m_active;
    QTimer* m_watchdog = nullptr;

    // Outcome of the last job per (file identity, provider, target) in this
    // process; Sent/Unconfirmed there makes a resend need confirmation.
    QHash<QString, Outcome> m_history;
    QVector<Provider*> m_attached;
};

} // namespace share
