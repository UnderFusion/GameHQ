#pragma once

#include "share/ShareProvider.h"
#include "telegram/TelegramAccount.h"

#include <QHash>
#include <QJsonArray>
#include <QSet>
#include <QTimer>

#include <functional>

namespace telegram
{
class TdRuntime;
}

namespace share
{

// Telegram Integrated (plan t13/t23/t24): sends the picked capture natively
// from GameHQ through the user's own Telegram account (TDLib). It is a Share
// provider only: no inbox, no incoming-message UI, no notifications.
//
// TDLib is a full account session, so accountAccess() is FullAccountSession
// and the Share UI shows the privacy notice before anyone connects.
class TelegramIntegratedProvider : public Provider
{
    Q_OBJECT
public:
    TelegramIntegratedProvider(telegram::Account* account, telegram::TdRuntime* runtime,
                               QObject* parent = nullptr);

    QString id() const override { return telegram::Account::providerId(); }
    QString displayName() const override;
    QString iconSource() const override { return QStringLiteral(""); }
    Capabilities capabilities() const override;
    AuthState authState() const override;
    Availability availability() const override;
    QString availabilityReason() const override;
    AccountAccess accountAccess() const override { return AccountAccess::FullAccountSession; }
    QString privacyNotice() const override;
    void disconnectAccount() override;
    int jobTimeoutMs() const override { return 10 * 60 * 1000; }

    void requestTargets(const QString& queryId, const Request& request,
                        const QString& query) override;
    void start(const Job& job, const Request& request, const Target& target) override;
    void cancel(const QString& jobId) override;

    // Chats and contacts as Share targets. Private chats and non-broadcast
    // groups only; groups where media is not allowed are dropped. Public for
    // tests, which feed TDLib-shaped objects straight in.
    QVector<Target> targetsFrom(const QVector<QJsonObject>& chats, const QVector<QJsonObject>& users,
                                MediaKind kind) const;

private:
    struct JobState_
    {
        bool active = false;
        bool queued = false;       // TDLib accepted the message; awaiting confirmation
        QString id;
        QString targetId;
        qint64 chatId = 0;
        qint64 messageId = 0;
    };

    void whenConnected(std::function<void()> onReady, std::function<void(const QString&)> onFail);
    bool whenConnectedForJob() const;
    void finishWait(const QString& failCode);
    void onAccountStateChanged();
    void loadTargets(const QString& queryId, const QString& query, MediaKind kind);
    void fetchChats(const QJsonArray& ids, std::function<void(QVector<QJsonObject>)> done);
    void onSendReply(const QJsonObject& reply);
    void onUpdate(const QJsonObject& update);
    void failJob(Outcome outcome, const QString& code);
    void finishJob(Outcome outcome, const QString& code);

    telegram::Account* m_account;
    telegram::TdRuntime* m_runtime;
    QString m_targetQuery;
    bool m_waiting = false;
    QTimer m_waitTimer;
    std::function<void()> m_onReady;
    std::function<void(const QString&)> m_onFail;
    JobState_ m_job;
    QSet<qint64> m_earlySucceeded;
    QHash<qint64, QString> m_earlyFailed;
};

} // namespace share
