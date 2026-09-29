#include "telegram/TelegramAccount.h"

#include "telegram/TdRuntime.h"

#include <QDebug>
#include <QDir>
#include <QJsonArray>
#include <QRandomGenerator>
#include <QRegularExpression>

namespace telegram
{

namespace
{
constexpr const char* kSecretApiId = "api-id";
constexpr const char* kSecretApiHash = "api-hash";
constexpr const char* kSecretDbKey = "db-key";
constexpr int kCloseGraceMs = 3000;
constexpr int kLogoutGraceMs = 8000;
} // namespace

Account::Account(Config config, QObject* parent)
    : QObject(parent)
    , m_cfg(std::move(config))
    , m_storage(m_cfg.sessionRoot)
{
    m_idle.setSingleShot(true);
    connect(&m_idle, &QTimer::timeout, this, [this] {
        // Nobody used the account for a while: stop the client (and with it any
        // background traffic). The session on disk stays; the next use resumes it.
        if (m_state == State::Connected && !m_closing) {
            qInfo() << "Telegram: closing idle client";
            closeClient();
        }
    });
}

Account::~Account()
{
    if (m_transport)
        m_transport->stop();
}

QString Account::stateName() const
{
    switch (m_state) {
    case State::Disconnected: return QStringLiteral("disconnected");
    case State::Starting:     return QStringLiteral("starting");
    case State::WaitPhone:    return QStringLiteral("wait_phone");
    case State::WaitCode:     return QStringLiteral("wait_code");
    case State::WaitPassword: return QStringLiteral("wait_password");
    case State::Connected:    return QStringLiteral("connected");
    case State::LoggingOut:   return QStringLiteral("logging_out");
    case State::Error:        return QStringLiteral("error");
    }
    return QStringLiteral("error");
}

QString Account::runtimeStatus() const
{
    return m_cfg.runtimeStatus ? m_cfg.runtimeStatus() : QStringLiteral("not_installed");
}

QString Account::sessionDirectory(bool create) const
{
    return m_storage.directoryFor(providerId(), create);
}

bool Account::hasSavedSession() const
{
    const QString dir = m_storage.directoryFor(providerId(), false);
    return !dir.isEmpty() && QDir(dir).exists()
           && !QDir(dir).entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty();
}

// ---- credentials ----------------------------------------------------------

bool Account::hasCredentials() const
{
    bool idFound = false, hashFound = false;
    m_cfg.secrets.read(providerId(), QString::fromLatin1(kSecretApiId), &idFound);
    m_cfg.secrets.read(providerId(), QString::fromLatin1(kSecretApiHash), &hashFound);
    return idFound && hashFound;
}

QString Account::setCredentials(const QString& apiId, const QString& apiHash)
{
    bool ok = false;
    const qlonglong id = apiId.trimmed().toLongLong(&ok);
    if (!ok || id <= 0 || id > 2147483647LL)
        return QStringLiteral("invalid_api_id");
    static const QRegularExpression kHash(QStringLiteral("^[0-9a-fA-F]{32}$"));
    const QString hash = apiHash.trimmed();
    if (!kHash.match(hash).hasMatch())
        return QStringLiteral("invalid_api_hash");
    if (!m_cfg.secrets.write(providerId(), QString::fromLatin1(kSecretApiId),
                             QByteArray::number(id))
        || !m_cfg.secrets.write(providerId(), QString::fromLatin1(kSecretApiHash),
                                hash.toLower().toLatin1()))
        return QStringLiteral("storage_failed");
    qInfo() << "Telegram: developer credentials saved";
    emit credentialsChanged();
    return {};
}

void Account::forgetCredentials()
{
    m_cfg.secrets.remove(providerId(), QString::fromLatin1(kSecretApiId));
    m_cfg.secrets.remove(providerId(), QString::fromLatin1(kSecretApiHash));
    qInfo() << "Telegram: developer credentials removed";
    emit credentialsChanged();
}

QByteArray Account::databaseKey(bool create)
{
    bool found = false;
    QByteArray key = m_cfg.secrets.read(providerId(), QString::fromLatin1(kSecretDbKey), &found);
    if (found && !key.isEmpty())
        return key;
    if (!create)
        return {};
    QByteArray raw(32, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32*>(raw.data()), raw.size() / 4);
    key = raw.toBase64();
    return m_cfg.secrets.write(providerId(), QString::fromLatin1(kSecretDbKey), key) ? key
                                                                                     : QByteArray();
}

// ---- lifecycle ------------------------------------------------------------

void Account::setState(State state, const QString& errorCode)
{
    if (m_state == state && m_errorCode == errorCode)
        return;
    m_state = state;
    m_errorCode = errorCode;
    qInfo() << "Telegram: account" << stateName()
            << (errorCode.isEmpty() ? QString() : QStringLiteral("code=") + errorCode);
    emit stateChanged();
}

void Account::fail(const QString& errorCode)
{
    setState(State::Error, errorCode);
}

bool Account::startClient()
{
    if (!m_cfg.transportFactory) {
        fail(QStringLiteral("runtime_") + runtimeStatus());
        return false;
    }
    m_transport = m_cfg.transportFactory();
    if (!m_transport) {
        fail(QStringLiteral("runtime_") + runtimeStatus());
        return false;
    }
    connect(m_transport.get(), &TdTransport::received, this, &Account::onReceived);
    if (!m_transport->start()) {
        m_transport.reset();
        fail(QStringLiteral("start_failed"));
        return false;
    }
    return true;
}

bool Account::connectAccount()
{
    if (m_state != State::Disconnected && m_state != State::Error)
        return m_state == State::Connected;
    if (!hasCredentials()) {
        fail(QStringLiteral("no_credentials"));
        return false;
    }
    setState(State::Starting);
    m_closing = false;
    if (!startClient())
        return false;
    touch();
    return true;
}

void Account::sendParameters()
{
    bool idFound = false, hashFound = false;
    const QByteArray id = m_cfg.secrets.read(providerId(), QString::fromLatin1(kSecretApiId), &idFound);
    const QByteArray hash = m_cfg.secrets.read(providerId(), QString::fromLatin1(kSecretApiHash), &hashFound);
    const QString dir = sessionDirectory(true);
    const QByteArray key = databaseKey(true);
    if (!idFound || !hashFound || dir.isEmpty() || key.isEmpty()) {
        fail(QStringLiteral("no_credentials"));
        closeClient();
        return;
    }
    QJsonObject p;
    p.insert(QStringLiteral("@type"), QStringLiteral("setTdlibParameters"));
    p.insert(QStringLiteral("use_test_dc"), false);
    p.insert(QStringLiteral("database_directory"), dir + QStringLiteral("/db"));
    p.insert(QStringLiteral("files_directory"), dir + QStringLiteral("/files"));
    p.insert(QStringLiteral("database_encryption_key"), QString::fromLatin1(key));
    // Keep as little as possible on disk: no message history and no file
    // cache. The chat info cache is what lets recipient search work offline.
    p.insert(QStringLiteral("use_file_database"), false);
    p.insert(QStringLiteral("use_chat_info_database"), true);
    p.insert(QStringLiteral("use_message_database"), false);
    p.insert(QStringLiteral("use_secret_chats"), false);
    p.insert(QStringLiteral("api_id"), id.toInt());
    p.insert(QStringLiteral("api_hash"), QString::fromLatin1(hash));
    p.insert(QStringLiteral("system_language_code"), m_cfg.languageCode);
    p.insert(QStringLiteral("device_model"), QStringLiteral("GameHQ"));
    p.insert(QStringLiteral("system_version"), QStringLiteral("Windows"));
    p.insert(QStringLiteral("application_version"), m_cfg.applicationVersion);
    request(p, [this](const QJsonObject& reply) {
        if (reply.value(QLatin1String("@type")).toString() == QLatin1String("error"))
            onError(reply);
    });
}

void Account::submitPhone(const QString& phoneNumber)
{
    if (m_state != State::WaitPhone || !m_transport)
        return;
    m_lastAuthStep = QStringLiteral("phone");
    QJsonObject o{ { QStringLiteral("@type"), QStringLiteral("setAuthenticationPhoneNumber") },
                   { QStringLiteral("phone_number"), phoneNumber.trimmed() } };
    request(o, [this](const QJsonObject& r) {
        if (r.value(QLatin1String("@type")).toString() == QLatin1String("error"))
            onError(r);
    });
}

void Account::submitCode(const QString& code)
{
    if (m_state != State::WaitCode || !m_transport)
        return;
    m_lastAuthStep = QStringLiteral("code");
    QJsonObject o{ { QStringLiteral("@type"), QStringLiteral("checkAuthenticationCode") },
                   { QStringLiteral("code"), code.trimmed() } };
    request(o, [this](const QJsonObject& r) {
        if (r.value(QLatin1String("@type")).toString() == QLatin1String("error"))
            onError(r);
    });
}

void Account::submitPassword(const QString& password)
{
    if (m_state != State::WaitPassword || !m_transport)
        return;
    m_lastAuthStep = QStringLiteral("password");
    QJsonObject o{ { QStringLiteral("@type"), QStringLiteral("checkAuthenticationPassword") },
                   { QStringLiteral("password"), password } };
    request(o, [this](const QJsonObject& r) {
        if (r.value(QLatin1String("@type")).toString() == QLatin1String("error"))
            onError(r);
    });
}

void Account::cancelLogin()
{
    if (!m_transport)
        return;
    if (m_state == State::Connected)
        return;   // an authorized session is left alone; use Disconnect
    closeClient();
}

void Account::disconnectAndRemoveSession(bool forgetToo)
{
    m_removeAfterClose = true;
    m_forgetAfterClose = forgetToo;
    if (!m_transport) {
        finishClose();
        return;
    }
    if (m_state == State::Connected) {
        // Ends the session on Telegram's side too; TDLib closes itself after.
        setState(State::LoggingOut);
        m_closing = true;
        m_transport->send(QJsonObject{ { QStringLiteral("@type"), QStringLiteral("logOut") } });
        QTimer::singleShot(kLogoutGraceMs, this, [this] {
            if (m_transport)
                finishClose();
        });
        return;
    }
    closeClient();
}

void Account::closeClient()
{
    if (!m_transport) {
        finishClose();
        return;
    }
    if (m_closing)
        return;
    m_closing = true;
    m_idle.stop();
    m_transport->send(QJsonObject{ { QStringLiteral("@type"), QStringLiteral("close") } });
    QTimer::singleShot(kCloseGraceMs, this, [this] {
        if (m_transport)
            finishClose();
    });
}

void Account::finishClose()
{
    m_idle.stop();
    if (m_transport) {
        m_transport->stop();
        m_transport.release()->deleteLater();
    }
    m_pending.clear();
    m_closing = false;
    if (m_removeAfterClose) {
        m_storage.removeAll(providerId());
        m_cfg.secrets.remove(providerId(), QString::fromLatin1(kSecretDbKey));
        if (m_forgetAfterClose)
            forgetCredentials();
        qInfo() << "Telegram: local session removed";
    }
    m_removeAfterClose = false;
    m_forgetAfterClose = false;
    const bool wasError = m_state == State::Error;
    if (!wasError)
        setState(State::Disconnected);
    emit clientClosed();
    emit stateChanged();
}

void Account::touch()
{
    if (m_state == State::Connected && m_cfg.idleCloseMs > 0)
        m_idle.start(m_cfg.idleCloseMs);
}

// ---- TDLib traffic --------------------------------------------------------

quint64 Account::request(const QJsonObject& body, ReplyHandler handler)
{
    if (!m_transport)
        return 0;
    const quint64 id = m_nextRequest++;
    QJsonObject o = body;
    o.insert(QStringLiteral("@extra"), QString::number(id));
    if (handler)
        m_pending.insert(id, std::move(handler));
    m_transport->send(o);
    return id;
}

void Account::onReceived(const QJsonObject& object)
{
    const QString type = object.value(QLatin1String("@type")).toString();
    const QString extra = object.value(QLatin1String("@extra")).toString();
    if (!extra.isEmpty()) {
        bool ok = false;
        const quint64 id = extra.toULongLong(&ok);
        if (ok && m_pending.contains(id)) {
            ReplyHandler handler = m_pending.take(id);
            handler(object);
            return;
        }
    }
    if (type == QLatin1String("updateAuthorizationState")) {
        onAuthorizationState(object.value(QLatin1String("authorization_state")).toObject());
        return;
    }
    if (type == QLatin1String("error")) {
        onError(object);
        return;
    }
    // Everything else (chats, messages, files, presence...) is not GameHQ's
    // business unless a send in progress asked for it.
    if (m_observer)
        m_observer(object);
}

void Account::onAuthorizationState(const QJsonObject& state)
{
    const QString type = state.value(QLatin1String("@type")).toString();
    if (type == QLatin1String("authorizationStateWaitTdlibParameters")) {
        sendParameters();
    } else if (type == QLatin1String("authorizationStateWaitPhoneNumber")) {
        setState(State::WaitPhone, m_lastAuthStep == QLatin1String("phone") ? m_errorCode : QString());
    } else if (type == QLatin1String("authorizationStateWaitCode")) {
        setState(State::WaitCode, m_lastAuthStep == QLatin1String("code") ? m_errorCode : QString());
    } else if (type == QLatin1String("authorizationStateWaitPassword")) {
        setState(State::WaitPassword,
                 m_lastAuthStep == QLatin1String("password") ? m_errorCode : QString());
    } else if (type == QLatin1String("authorizationStateReady")) {
        m_lastAuthStep.clear();
        setState(State::Connected);
        // GameHQ must not show the user as online on Telegram.
        request(QJsonObject{ { QStringLiteral("@type"), QStringLiteral("setOption") },
                             { QStringLiteral("name"), QStringLiteral("online") },
                             { QStringLiteral("value"),
                               QJsonObject{ { QStringLiteral("@type"), QStringLiteral("optionValueBoolean") },
                                            { QStringLiteral("value"), false } } } },
                nullptr);
        touch();
    } else if (type == QLatin1String("authorizationStateLoggingOut")) {
        m_closing = true;
        setState(State::LoggingOut);
    } else if (type == QLatin1String("authorizationStateClosing")) {
        m_closing = true;
    } else if (type == QLatin1String("authorizationStateClosed")) {
        finishClose();
    } else {
        // E-mail login, QR confirmation, registration, premium purchase...
        // Not something Share will grow into: stop cleanly and say so.
        fail(QStringLiteral("unsupported_auth"));
        closeClient();
    }
}

QString Account::mapTdError(const QString& message, int code)
{
    const QString m = message.toUpper();
    if (m.startsWith(QLatin1String("PHONE_NUMBER_INVALID")))    return QStringLiteral("phone_invalid");
    if (m.startsWith(QLatin1String("PHONE_CODE_EXPIRED")))      return QStringLiteral("code_expired");
    if (m.startsWith(QLatin1String("PHONE_CODE_")))             return QStringLiteral("code_invalid");
    if (m.startsWith(QLatin1String("PASSWORD_HASH_INVALID")))   return QStringLiteral("password_invalid");
    if (m.startsWith(QLatin1String("API_ID_")))                 return QStringLiteral("credentials_rejected");
    if (code == 429 || m.startsWith(QLatin1String("FLOOD")))   return QStringLiteral("rate_limited");
    return QStringLiteral("auth_failed");
}

void Account::onError(const QJsonObject& error)
{
    // Only the code is kept: the message can echo user input.
    const QString mapped = mapTdError(error.value(QLatin1String("message")).toString(),
                                      error.value(QLatin1String("code")).toInt());
    if (m_state == State::Starting || m_state == State::Connected) {
        fail(mapped);
        return;
    }
    // A wrong answer during login leaves TDLib waiting in the same step: keep
    // that prompt and only attach the reason.
    State prompt = m_state;
    if (m_lastAuthStep == QLatin1String("phone")) prompt = State::WaitPhone;
    else if (m_lastAuthStep == QLatin1String("code")) prompt = State::WaitCode;
    else if (m_lastAuthStep == QLatin1String("password")) prompt = State::WaitPassword;
    m_errorCode = mapped;
    if (m_state == prompt) {
        emit stateChanged();
    } else {
        setState(prompt, mapped);
    }
}

} // namespace telegram
