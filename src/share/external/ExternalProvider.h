#pragma once

#include "share/ShareProvider.h"
#include "share/external/ExternalProviderProtocol.h"

#include <QJsonObject>

#include <functional>

class QTimer;

namespace share::external
{

// The in-process face of one connected out-of-process provider. It turns the
// Share `Provider` contract into protocol messages and treats everything the
// peer says as untrusted: every id, string and number is validated and
// bounded, results are only accepted for the current job's id AND token, and a
// lost connection ends the job as `unconfirmed` instead of guessing.
//
// It has no file access of its own: the only path that ever leaves GameHQ is
// the one in the `job.start` for the capture the user explicitly shared.
class ExternalProvider : public Provider
{
    Q_OBJECT
public:
    using Sender = std::function<bool(const QJsonObject& message)>;

    ExternalProvider(Manifest manifest, Sender send, QObject* parent = nullptr);

    QString id() const override { return m_manifest.id; }
    QString displayName() const override { return m_manifest.name; }
    QString iconSource() const override { return QStringLiteral(""); }   // Segoe Fluent: OpenInNewWindow
    Capabilities capabilities() const override { return m_manifest.capabilities; }
    Availability availability() const override
    {
        return m_lost ? Availability::Unsupported : Availability::Available;
    }
    QString availabilityReason() const override;
    AccountAccess accountAccess() const override { return m_manifest.access; }
    QString privacyNotice() const override;
    int jobTimeoutMs() const override { return m_manifest.jobTimeoutMs; }

    void requestTargets(const QString& queryId, const Request& request,
                        const QString& query) override;
    void start(const Job& job, const Request& request, const Target& target) override;
    void cancel(const QString& jobId) override;

    // A validated message from the handshaken peer. Returns "" or the
    // ErrorCode the peer earned (the host counts these as violations).
    QString handleMessage(const QString& type, const QJsonObject& message);
    // The connection is gone (clean close, crash or drop).
    void connectionLost();

    const Manifest& manifest() const { return m_manifest; }
    bool hasActiveJob() const { return !m_jobId.isEmpty(); }
    void setTargetsTimeoutMs(int ms);

private:
    QString onTargetsResult(const QJsonObject& message);
    QString onJobProgress(const QJsonObject& message);
    QString onJobResult(const QJsonObject& message);
    bool matchesActiveJob(const QJsonObject& message) const;
    void clearJob();

    Manifest m_manifest;
    Sender m_send;
    bool m_lost = false;

    QString m_pendingQuery;
    QTimer* m_targetsTimer = nullptr;

    QString m_jobId;
    QString m_jobToken;
    bool m_cancelSent = false;
};

} // namespace share::external
