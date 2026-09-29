#pragma once

#include "share/ShareSecurity.h"
#include "telegram/TdTransport.h"

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>
#include <memory>

namespace telegram
{

// GameHQ's Telegram Integrated account session (plan t23): the TDLib
// authorization state machine and its secure lifecycle, and nothing else.
// GameHQ uses the account to Share only. This class deliberately has no
// notion of messages, inbox, unread counts or notifications: every update
// that is not about authorization or an answer to a request GameHQ made is
// dropped on arrival.
//
// Secrets: api_id/api_hash and the random database key live in the Windows
// Credential Manager through share::SecretStore; the TDLib session directory
// is confined under one root; neither the phone number, the login code, the
// password nor any of those secrets is ever logged, stored or kept in a
// property.
class Account : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString state READ stateName NOTIFY stateChanged)
    Q_PROPERTY(QString errorCode READ errorCode NOTIFY stateChanged)
    Q_PROPERTY(bool hasCredentials READ hasCredentials NOTIFY credentialsChanged)
    Q_PROPERTY(bool hasSavedSession READ hasSavedSession NOTIFY stateChanged)
    Q_PROPERTY(QString runtimeStatus READ runtimeStatus NOTIFY stateChanged)

public:
    enum class State {
        Disconnected,   // no client running (a saved session may still exist)
        Starting,       // client created, waiting for TDLib
        WaitPhone,      // needs the phone number
        WaitCode,       // needs the login code Telegram sent
        WaitPassword,   // needs the two-step verification password
        Connected,      // authorized and ready
        LoggingOut,     // logOut / close in flight
        Error,          // see errorCode()
    };
    Q_ENUM(State)

    struct Config
    {
        share::SecretStore secrets;
        QString sessionRoot;                 // confined parent of the TDLib directory
        TdTransportFactory transportFactory; // null => runtime unavailable
        std::function<QString()> runtimeStatus;   // stable code for the UI
        QString applicationVersion;
        QString languageCode = QStringLiteral("en");
        int idleCloseMs = 5 * 60 * 1000;     // release the client after this much quiet
    };

    static QString providerId() { return QStringLiteral("telegram.integrated"); }

    explicit Account(Config config, QObject* parent = nullptr);
    ~Account() override;

    State state() const { return m_state; }
    QString stateName() const;
    // Stable identifiers: "runtime_<status>", "no_credentials", "start_failed",
    // "phone_invalid", "code_invalid", "code_expired", "password_invalid",
    // "rate_limited", "unsupported_auth", "auth_failed", "network".
    QString errorCode() const { return m_errorCode; }
    QString runtimeStatus() const;

    // --- credentials (the developer api_id/api_hash of this GameHQ build) ---
    bool hasCredentials() const;
    // Returns "" on success or "invalid_api_id" / "invalid_api_hash" / "storage_failed".
    Q_INVOKABLE QString setCredentials(const QString& apiId, const QString& apiHash);
    // Forget the credentials only; the session stays until it is removed.
    Q_INVOKABLE void forgetCredentials();

    // --- lifecycle ---
    // Starts the client. With a saved session this reaches Connected without
    // any prompt; otherwise it walks WaitPhone -> WaitCode [-> WaitPassword].
    Q_INVOKABLE bool connectAccount();
    Q_INVOKABLE void submitPhone(const QString& phoneNumber);
    Q_INVOKABLE void submitCode(const QString& code);
    Q_INVOKABLE void submitPassword(const QString& password);
    // Abandon an unfinished login (or stop a running client) without touching
    // a saved session.
    Q_INVOKABLE void cancelLogin();
    // Sign out on Telegram's side, close the client and delete the local
    // session. Credentials stay unless `forgetToo`.
    Q_INVOKABLE void disconnectAndRemoveSession(bool forgetToo = false);
    bool hasSavedSession() const;

    // Restarts the idle timer; providers call this on every use.
    void touch();
    // The transport while a client is running (t24 sends through it), else null.
    TdTransport* transport() const { return m_transport.get(); }
    // Requests answered through a request id: t24 registers a handler here.
    using ReplyHandler = std::function<void(const QJsonObject& reply)>;
    quint64 request(const QJsonObject& body, ReplyHandler handler);
    // Every non-auth update goes to this observer (t24 tracks upload/message
    // confirmation); null => dropped.
    void setUpdateObserver(std::function<void(const QJsonObject& update)> observer)
    {
        m_observer = std::move(observer);
    }

    QString sessionDirectory(bool create = false) const;

signals:
    void stateChanged();
    void credentialsChanged();
    // Emitted once a client that was running has fully closed.
    void clientClosed();

private:
    void onReceived(const QJsonObject& object);
    void onAuthorizationState(const QJsonObject& state);
    void sendParameters();
    void setState(State state, const QString& errorCode = {});
    void fail(const QString& errorCode);
    void onError(const QJsonObject& error);
    void closeClient();
    void finishClose();
    bool startClient();
    QByteArray databaseKey(bool create);
    static QString mapTdError(const QString& message, int code);

    Config m_cfg;
    share::SessionStorage m_storage;
    std::unique_ptr<TdTransport> m_transport;
    State m_state = State::Disconnected;
    QString m_errorCode;
    QTimer m_idle;
    bool m_removeAfterClose = false;
    bool m_forgetAfterClose = false;
    bool m_closing = false;
    quint64 m_nextRequest = 1;
    QHash<quint64, ReplyHandler> m_pending;
    QString m_lastAuthStep;   // "phone" | "code" | "password": which answer is in flight
    std::function<void(const QJsonObject&)> m_observer;
};

} // namespace telegram
