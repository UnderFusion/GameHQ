#pragma once

#include "telegram/TdTransport.h"

#include <QJsonObject>
#include <QMetaObject>
#include <QStringList>
#include <QTimer>

#include <functional>

// A scripted stand-in for TDLib used by the Telegram tests. It answers the
// authorization requests the way TDLib does (states as updates, failures as
// "error" objects carrying the request's @extra) so the account logic runs
// end to end without a runtime or an account.
class FakeTdTransport : public telegram::TdTransport
{
    Q_OBJECT
public:
    struct Script
    {
        bool savedSession = false;     // setTdlibParameters -> Ready directly
        bool needsPassword = false;    // correct code -> WaitPassword
        bool unsupportedStep = false;  // setTdlibParameters -> WaitEmailAddress
        bool failStart = false;
        bool swallowClose = false;     // never answers close/logOut
    };

    explicit FakeTdTransport(Script script, QObject* parent = nullptr)
        : TdTransport(parent), m_script(script) {}

    bool start() override
    {
        if (m_script.failStart)
            return false;
        started = true;
        state(QStringLiteral("authorizationStateWaitTdlibParameters"));
        return true;
    }

    void stop() override { stopped = true; }

    void send(const QJsonObject& r) override
    {
        sent.append(r);
        const QString type = r.value("@type").toString();
        const QString extra = r.value("@extra").toString();
        if (responder) {
            QJsonObject answer = responder(r);
            if (!answer.isEmpty()) {
                answer.insert("@extra", extra);
                later(answer);
                return;
            }
        }
        if (type == "setTdlibParameters") {
            reply(extra);
            if (m_script.unsupportedStep)
                state(QStringLiteral("authorizationStateWaitEmailAddress"));
            else
                state(m_script.savedSession ? QStringLiteral("authorizationStateReady")
                                            : QStringLiteral("authorizationStateWaitPhoneNumber"));
        } else if (type == "setAuthenticationPhoneNumber") {
            if (r.value("phone_number").toString() == "+bad")
                error(extra, 400, QStringLiteral("PHONE_NUMBER_INVALID"));
            else {
                reply(extra);
                state(QStringLiteral("authorizationStateWaitCode"));
            }
        } else if (type == "checkAuthenticationCode") {
            const QString code = r.value("code").toString();
            if (code == "0000")
                error(extra, 400, QStringLiteral("PHONE_CODE_INVALID"));
            else if (code == "9999")
                error(extra, 429, QStringLiteral("FLOOD_WAIT_30"));
            else {
                reply(extra);
                state(m_script.needsPassword ? QStringLiteral("authorizationStateWaitPassword")
                                             : QStringLiteral("authorizationStateReady"));
            }
        } else if (type == "checkAuthenticationPassword") {
            if (r.value("password").toString() != "pw")
                error(extra, 400, QStringLiteral("PASSWORD_HASH_INVALID"));
            else {
                reply(extra);
                state(QStringLiteral("authorizationStateReady"));
            }
        } else if (type == "logOut" && !m_script.swallowClose) {
            state(QStringLiteral("authorizationStateLoggingOut"));
            state(QStringLiteral("authorizationStateClosed"));
        } else if (type == "close" && !m_script.swallowClose) {
            state(QStringLiteral("authorizationStateClosing"));
            state(QStringLiteral("authorizationStateClosed"));
        }
    }

    // Push an arbitrary update at the account.
    void push(const QJsonObject& o) { later(o); }

    // Optional scripted answers for non-auth requests: return the reply object
    // (its @extra is filled in) or an empty object to fall through.
    std::function<QJsonObject(const QJsonObject&)> responder;

    QVector<QJsonObject> sent;
    bool started = false;
    bool stopped = false;

    bool sentType(const QString& type) const
    {
        for (const QJsonObject& o : sent)
            if (o.value("@type").toString() == type)
                return true;
        return false;
    }
    int countOfType(const QString& type) const
    {
        int n = 0;
        for (const QJsonObject& o : sent)
            if (o.value("@type").toString() == type)
                ++n;
        return n;
    }
    QJsonObject firstOfType(const QString& type) const
    {
        for (const QJsonObject& o : sent)
            if (o.value("@type").toString() == type)
                return o;
        return {};
    }

private:
    void later(const QJsonObject& o)
    {
        QMetaObject::invokeMethod(this, [this, o] { emit received(o); }, Qt::QueuedConnection);
    }
    void state(const QString& type)
    {
        later(QJsonObject{ { "@type", "updateAuthorizationState" },
                           { "authorization_state", QJsonObject{ { "@type", type } } } });
    }
    void reply(const QString& extra)
    {
        later(QJsonObject{ { "@type", "ok" }, { "@extra", extra } });
    }
    void error(const QString& extra, int code, const QString& message)
    {
        later(QJsonObject{ { "@type", "error" }, { "@extra", extra },
                           { "code", code }, { "message", message } });
    }

    Script m_script;
};
