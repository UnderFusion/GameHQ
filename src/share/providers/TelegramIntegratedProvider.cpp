#include "share/providers/TelegramIntegratedProvider.h"

#include "localization/NativeText.h"
#include "telegram/TdRuntime.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QMetaObject>
#include <QSet>
#include <QTimer>

#include <memory>

namespace share
{

namespace
{
constexpr int kDefaultListSize = 20;
constexpr int kSearchChats = 15;
constexpr int kSearchContacts = 10;
constexpr int kResumeTimeoutMs = 20000;
// Telegram accepts photos up to 10 MB; anything bigger goes as a file so a
// large PNG screenshot still arrives instead of failing.
constexpr qint64 kPhotoLimitBytes = 10LL * 1024 * 1024;
// Regular (non-Premium) upload limit for one file.
constexpr qint64 kFileLimitBytes = 2000LL * 1024 * 1024;

QString chatTargetId(qint64 chatId) { return QStringLiteral("c:") + QString::number(chatId); }
QString userTargetId(qint64 userId) { return QStringLiteral("u:") + QString::number(userId); }

QJsonObject typed(const char* type, QJsonObject fields = {})
{
    fields.insert(QStringLiteral("@type"), QString::fromLatin1(type));
    return fields;
}

QString sendErrorCode(int code, const QString& message)
{
    const QString m = message.toUpper();
    if (code == 429 || m.startsWith(QLatin1String("FLOOD")) || m.contains(QLatin1String("SLOWMODE")))
        return QStringLiteral("rate_limited");
    if (m.contains(QLatin1String("WRITE_FORBIDDEN")) || m.contains(QLatin1String("FORBIDDEN"))
        || m.contains(QLatin1String("BANNED")) || m.contains(QLatin1String("RIGHTS")))
        return QStringLiteral("not_allowed");
    if (m.contains(QLatin1String("TOO_BIG")) || m.contains(QLatin1String("TOO_LARGE")))
        return QStringLiteral("too_large");
    return QStringLiteral("send_failed");
}

// Collects N asynchronous answers and calls `done` once, in arrival order.
struct Gather
{
    int remaining = 0;
    QVector<QJsonObject> items;
    std::function<void(QVector<QJsonObject>)> done;
    void arrive(const QJsonObject& item)
    {
        if (!item.isEmpty() && item.value(QLatin1String("@type")).toString() != QLatin1String("error"))
            items.append(item);
        if (--remaining == 0 && done)
            done(items);
    }
};
} // namespace

TelegramIntegratedProvider::TelegramIntegratedProvider(telegram::Account* account,
                                                       telegram::TdRuntime* runtime,
                                                       QObject* parent)
    : Provider(parent)
    , m_account(account)
    , m_runtime(runtime)
{
    m_waitTimer.setSingleShot(true);
    connect(&m_waitTimer, &QTimer::timeout, this, [this] { finishWait(QStringLiteral("network_error")); });
    connect(m_account, &telegram::Account::stateChanged, this, [this] {
        onAccountStateChanged();
        emit stateChanged();
    });
    m_account->setUpdateObserver([this](const QJsonObject& u) { onUpdate(u); });
}

QString TelegramIntegratedProvider::displayName() const
{
    return NativeText::get(
        //: Share destination that sends the capture from GameHQ through the user's connected Telegram account.
        //% "Telegram account"
        QT_TRID_NOOP("gamehq.share.telegram_integrated.name"), "Telegram account");
}

Capabilities TelegramIntegratedProvider::capabilities() const
{
    return Capability::Image | Capability::Video | Capability::Contacts | Capability::Groups
           | Capability::DirectSend | Capability::TargetSearch | Capability::RequiresAccount;
}

AuthState TelegramIntegratedProvider::authState() const
{
    using S = telegram::Account::State;
    switch (m_account->state()) {
    case S::Connected:
        return AuthState::Connected;
    case S::Starting:
    case S::WaitPhone:
    case S::WaitCode:
    case S::WaitPassword:
    case S::LoggingOut:
        return AuthState::Connecting;
    case S::Error:
        return AuthState::Error;
    case S::Disconnected:
        // An idle client is released to stop background traffic; the saved
        // session resumes on the next Share, so this still counts as connected.
        return m_account->hasSavedSession() ? AuthState::Connected : AuthState::Disconnected;
    }
    return AuthState::Disconnected;
}

Availability TelegramIntegratedProvider::availability() const
{
    switch (m_runtime ? m_runtime->quickStatus() : telegram::TdRuntime::Status::NotInstalled) {
    case telegram::TdRuntime::Status::NotLoaded:
    case telegram::TdRuntime::Status::Ready:
        return Availability::Available;
    case telegram::TdRuntime::Status::NotInstalled:
    case telegram::TdRuntime::Status::Unpinned:
        return Availability::NotInstalled;
    default:
        return Availability::Unsupported;
    }
}

QString TelegramIntegratedProvider::availabilityReason() const
{
    if (availability() == Availability::Available)
        return {};
    if (availability() == Availability::NotInstalled)
        return NativeText::get(
            //: Telegram account sharing needs an optional component that is not part of this GameHQ install.
            //% "The optional Telegram component is not installed."
            QT_TRID_NOOP("gamehq.share.telegram_integrated.no_runtime"),
            "The optional Telegram component is not installed.");
    return NativeText::get(
        //: The optional Telegram component was found but is not the exact version GameHQ expects.
        //% "The optional Telegram component is not the version GameHQ expects."
        QT_TRID_NOOP("gamehq.share.telegram_integrated.bad_runtime"),
        "The optional Telegram component is not the version GameHQ expects.");
}

QString TelegramIntegratedProvider::privacyNotice() const
{
    return NativeText::get(
        //: Shown before connecting Telegram inside GameHQ. It is a full account sign-in, but GameHQ only sends the capture the user picks.
        //% "Signs in to your Telegram account inside GameHQ. GameHQ only sends the capture you pick and never shows your chats or messages."
        QT_TRID_NOOP("gamehq.share.telegram_integrated.privacy"),
        "Signs in to your Telegram account inside GameHQ. GameHQ only sends the capture you pick and never shows your chats or messages.");
}

void TelegramIntegratedProvider::disconnectAccount()
{
    finishWait(QStringLiteral("not_connected"));
    m_account->disconnectAndRemoveSession(false);
}

// ---- getting a connected client -------------------------------------------

void TelegramIntegratedProvider::whenConnected(std::function<void()> onReady,
                                               std::function<void(const QString&)> onFail)
{
    using S = telegram::Account::State;
    finishWait(QStringLiteral("superseded"));
    if (m_account->state() == S::Connected) {
        m_account->touch();
        onReady();
        return;
    }
    const bool canResume = (m_account->state() == S::Disconnected || m_account->state() == S::Error)
                           && m_account->hasCredentials() && m_account->hasSavedSession();
    if (!canResume && m_account->state() != S::Starting) {
        onFail(QStringLiteral("not_connected"));
        return;
    }
    m_onReady = std::move(onReady);
    m_onFail = std::move(onFail);
    m_waiting = true;
    m_waitTimer.start(kResumeTimeoutMs);
    if (canResume && !m_account->connectAccount())
        finishWait(QStringLiteral("not_connected"));
}

void TelegramIntegratedProvider::onAccountStateChanged()
{
    using S = telegram::Account::State;
    if (m_waiting) {
        switch (m_account->state()) {
        case S::Connected:
            finishWait({});
            break;
        case S::WaitPhone:
        case S::WaitCode:
        case S::WaitPassword:   // the saved session is gone: needs a fresh login in Settings
        case S::Error:
            finishWait(QStringLiteral("not_connected"));
            break;
        default:
            break;
        }
    }
    if (m_job.active && m_account->state() != S::Connected)
        failJob(Outcome::Unconfirmed, QStringLiteral("connection_lost"));
}

void TelegramIntegratedProvider::finishWait(const QString& failCode)
{
    if (!m_waiting)
        return;
    m_waiting = false;
    m_waitTimer.stop();
    auto ready = std::move(m_onReady);
    auto fail = std::move(m_onFail);
    m_onReady = nullptr;
    m_onFail = nullptr;
    if (failCode.isEmpty()) {
        if (ready)
            ready();
    } else if (fail && failCode != QLatin1String("superseded")) {
        fail(failCode);
    }
}

// ---- targets --------------------------------------------------------------

void TelegramIntegratedProvider::requestTargets(const QString& queryId, const Request& request,
                                                const QString& query)
{
    m_targetQuery = queryId;
    const MediaKind kind = request.mediaKind();
    whenConnected(
        [this, queryId, query, kind] { loadTargets(queryId, query.trimmed(), kind); },
        [this, queryId](const QString& code) {
            if (m_targetQuery == queryId)
                emit targetsReady(queryId, {}, code);
        });
}

void TelegramIntegratedProvider::loadTargets(const QString& queryId, const QString& query,
                                             MediaKind kind)
{
    auto finish = [this, queryId, kind](QVector<QJsonObject> chats, QVector<QJsonObject> users) {
        if (m_targetQuery != queryId)
            return;   // a newer query took over
        emit targetsReady(queryId, targetsFrom(chats, users, kind), {});
    };

    if (query.isEmpty()) {
        m_account->request(typed("loadChats", { { "chat_list", typed("chatListMain") },
                                                 { "limit", kDefaultListSize } }),
                           [this, finish](const QJsonObject&) {
            // "loadChats" answers with an error (404) once everything is loaded;
            // either way the local list is what we want.
            m_account->request(typed("getChats", { { "chat_list", typed("chatListMain") },
                                                    { "limit", kDefaultListSize } }),
                               [this, finish](const QJsonObject& r) {
                fetchChats(r.value(QLatin1String("chat_ids")).toArray(),
                           [finish](QVector<QJsonObject> chats) { finish(chats, {}); });
            });
        });
        return;
    }

    auto chatsOut = std::make_shared<QVector<QJsonObject>>();
    auto usersOut = std::make_shared<QVector<QJsonObject>>();
    auto both = std::make_shared<Gather>();
    both->remaining = 2;
    both->done = [chatsOut, usersOut, finish](QVector<QJsonObject>) { finish(*chatsOut, *usersOut); };

    m_account->request(typed("searchChats", { { "query", query }, { "limit", kSearchChats } }),
                       [this, both, chatsOut](const QJsonObject& r) {
        fetchChats(r.value(QLatin1String("chat_ids")).toArray(),
                   [both, chatsOut](QVector<QJsonObject> chats) {
            *chatsOut = chats;
            both->arrive({});
        });
    });
    m_account->request(typed("searchContacts", { { "query", query }, { "limit", kSearchContacts } }),
                       [this, both, usersOut](const QJsonObject& r) {
        const QJsonArray ids = r.value(QLatin1String("user_ids")).toArray();
        if (ids.isEmpty()) {
            both->arrive({});
            return;
        }
        auto g = std::make_shared<Gather>();
        g->remaining = ids.size();
        g->done = [both, usersOut](QVector<QJsonObject> users) {
            *usersOut = users;
            both->arrive({});
        };
        for (const QJsonValue& id : ids)
            m_account->request(typed("getUser", { { "user_id", id.toVariant().toLongLong() } }),
                               [g](const QJsonObject& u) { g->arrive(u); });
    });
}

void TelegramIntegratedProvider::fetchChats(const QJsonArray& ids,
                                            std::function<void(QVector<QJsonObject>)> done)
{
    if (ids.isEmpty()) {
        done({});
        return;
    }
    auto g = std::make_shared<Gather>();
    g->remaining = ids.size();
    g->done = std::move(done);
    // Answers arrive in any order; the caller wants Telegram's own ordering.
    QVector<qint64> order;
    for (const QJsonValue& id : ids)
        order.append(id.toVariant().toLongLong());
    auto original = g->done;
    g->done = [order, original](QVector<QJsonObject> got) {
        QVector<QJsonObject> sorted;
        for (qint64 id : order)
            for (const QJsonObject& c : got)
                if (c.value(QLatin1String("id")).toVariant().toLongLong() == id) {
                    sorted.append(c);
                    break;
                }
        original(sorted);
    };
    for (qint64 id : order)
        m_account->request(typed("getChat", { { "chat_id", id } }),
                           [g](const QJsonObject& c) { g->arrive(c); });
}

QVector<Target> TelegramIntegratedProvider::targetsFrom(const QVector<QJsonObject>& chats,
                                                        const QVector<QJsonObject>& users,
                                                        MediaKind kind) const
{
    QVector<Target> out;
    QSet<qint64> privateUsersSeen;
    const auto mediaAllowed = [kind](const QJsonObject& permissions) {
        if (!permissions.value(QLatin1String("can_send_basic_messages")).toBool(true))
            return false;
        return kind == MediaKind::Image
                   ? permissions.value(QLatin1String("can_send_photos")).toBool(true)
                   : permissions.value(QLatin1String("can_send_videos")).toBool(true);
    };
    for (const QJsonObject& chat : chats) {
        const QJsonObject type = chat.value(QLatin1String("type")).toObject();
        const QString typeName = type.value(QLatin1String("@type")).toString();
        Target t;
        t.providerId = id();
        t.id = chatTargetId(chat.value(QLatin1String("id")).toVariant().toLongLong());
        t.displayName = chat.value(QLatin1String("title")).toString();
        if (t.displayName.isEmpty())
            continue;
        t.capabilities = Capability::Image | Capability::Video;
        if (typeName == QLatin1String("chatTypePrivate")) {
            t.kind = TargetKind::Contact;
            privateUsersSeen.insert(type.value(QLatin1String("user_id")).toVariant().toLongLong());
        } else if (typeName == QLatin1String("chatTypeBasicGroup")
                   || (typeName == QLatin1String("chatTypeSupergroup")
                       && !type.value(QLatin1String("is_channel")).toBool())) {
            t.kind = TargetKind::Group;
            if (!mediaAllowed(chat.value(QLatin1String("permissions")).toObject()))
                continue;
        } else {
            continue;   // secret chats and broadcast channels are not Share destinations
        }
        out.append(t);
    }
    for (const QJsonObject& user : users) {
        const qint64 uid = user.value(QLatin1String("id")).toVariant().toLongLong();
        const QString userType = user.value(QLatin1String("type")).toObject()
                                     .value(QLatin1String("@type")).toString();
        if (privateUsersSeen.contains(uid) || userType != QLatin1String("userTypeRegular"))
            continue;
        Target t;
        t.providerId = id();
        t.id = userTargetId(uid);
        t.kind = TargetKind::Contact;
        t.displayName = (user.value(QLatin1String("first_name")).toString() + QLatin1Char(' ')
                         + user.value(QLatin1String("last_name")).toString()).trimmed();
        if (t.displayName.isEmpty())
            continue;
        t.capabilities = Capability::Image | Capability::Video;
        out.append(t);
    }
    return out;
}

// ---- sending --------------------------------------------------------------

void TelegramIntegratedProvider::start(const Job& job, const Request& request, const Target& target)
{
    m_job = {};
    m_job.id = job.id;
    m_job.targetId = target.id;
    m_job.active = true;

    QFileInfo info(request.filePath());
    if (!info.isFile() || info.size() <= 0) {
        failJob(Outcome::Failed, QStringLiteral("capture_missing"));
        return;
    }
    if (info.size() > kFileLimitBytes) {
        failJob(Outcome::Failed, QStringLiteral("too_large"));
        return;
    }
    const QString path = QDir::toNativeSeparators(info.absoluteFilePath());
    const bool asPhoto = request.mediaKind() == MediaKind::Image && info.size() <= kPhotoLimitBytes;
    const QJsonObject file = typed("inputFileLocal", { { "path", path } });
    QJsonObject content;
    if (request.mediaKind() == MediaKind::Video)
        content = typed("inputMessageVideo", { { "video", file }, { "supports_streaming", true } });
    else if (asPhoto)
        content = typed("inputMessagePhoto", { { "photo", file } });
    else
        content = typed("inputMessageDocument",
                        { { "document", file }, { "disable_content_type_detection", true } });

    // Ids are only ever "c:<chat>" or "u:<user>" as issued by targetsFrom().
    const QString tid = target.id;
    const qint64 numeric = tid.mid(2).toLongLong();
    if (!whenConnectedForJob()) {
        failJob(Outcome::Failed, QStringLiteral("not_connected"));
        return;
    }
    const auto send = [this, content](qint64 chatId) {
        if (!m_job.active)
            return;   // cancelled while the private chat was being opened
        m_job.chatId = chatId;
        emit jobProgress(m_job.id, JobState::Preparing, -1.0);
        m_account->request(typed("sendMessage", { { "chat_id", chatId },
                                                   { "input_message_content", content } }),
                           [this](const QJsonObject& r) { onSendReply(r); });
    };
    if (tid.startsWith(QLatin1String("c:")) && numeric != 0) {
        send(numeric);
    } else if (tid.startsWith(QLatin1String("u:")) && numeric != 0) {
        m_account->request(typed("createPrivateChat", { { "user_id", numeric }, { "force", false } }),
                           [this, send](const QJsonObject& r) {
            if (r.value(QLatin1String("@type")).toString() == QLatin1String("error")) {
                failJob(Outcome::Failed, QStringLiteral("not_allowed"));
                return;
            }
            send(r.value(QLatin1String("id")).toVariant().toLongLong());
        });
    } else {
        failJob(Outcome::Failed, QStringLiteral("unknown_target"));
    }
}

bool TelegramIntegratedProvider::whenConnectedForJob() const
{
    return m_account->state() == telegram::Account::State::Connected;
}

void TelegramIntegratedProvider::onSendReply(const QJsonObject& reply)
{
    if (!m_job.active)
        return;
    if (reply.value(QLatin1String("@type")).toString() == QLatin1String("error")) {
        failJob(Outcome::Failed, sendErrorCode(reply.value(QLatin1String("code")).toInt(),
                                               reply.value(QLatin1String("message")).toString()));
        return;
    }
    m_job.messageId = reply.value(QLatin1String("id")).toVariant().toLongLong();
    m_job.queued = true;
    emit jobProgress(m_job.id, JobState::Transferring, -1.0);
    // The confirmation may have raced ahead of this reply.
    if (m_earlySucceeded.contains(m_job.messageId)) {
        m_earlySucceeded.remove(m_job.messageId);
        finishJob(Outcome::Sent, {});
    } else if (m_earlyFailed.contains(m_job.messageId)) {
        failJob(Outcome::Failed, m_earlyFailed.take(m_job.messageId));
    }
}

void TelegramIntegratedProvider::onUpdate(const QJsonObject& update)
{
    m_account->touch();   // an active upload must not look idle
    const QString type = update.value(QLatin1String("@type")).toString();
    const qint64 oldId = update.value(QLatin1String("old_message_id")).toVariant().toLongLong();
    if (type == QLatin1String("updateMessageSendSucceeded")) {
        if (m_job.active && m_job.queued && oldId == m_job.messageId)
            finishJob(Outcome::Sent, {});   // Telegram confirmed the outgoing message
        else if (m_job.active)
            m_earlySucceeded.insert(oldId);
    } else if (type == QLatin1String("updateMessageSendFailed")) {
        const QJsonObject err = update.value(QLatin1String("error")).toObject();
        const QString code = sendErrorCode(err.value(QLatin1String("code")).toInt(),
                                           err.value(QLatin1String("message")).toString());
        if (m_job.active && m_job.queued && oldId == m_job.messageId)
            failJob(Outcome::Failed, code);
        else if (m_job.active)
            m_earlyFailed.insert(oldId, code);
    }
    // Everything else (messages, chats, presence, files) is deliberately ignored.
}

void TelegramIntegratedProvider::cancel(const QString& jobId)
{
    if (!m_job.active || m_job.id != jobId)
        return;
    if (m_job.queued && m_job.chatId != 0) {
        // Best effort: remove the not-yet-delivered message. Only an accepted
        // deletion counts as cancelled; otherwise the real outcome decides.
        m_account->request(typed("deleteMessages", { { "chat_id", m_job.chatId },
                                                      { "message_ids", QJsonArray{ m_job.messageId } },
                                                      { "revoke", true } }),
                           [this, jobId](const QJsonObject& r) {
            if (m_job.active && m_job.id == jobId
                && r.value(QLatin1String("@type")).toString() != QLatin1String("error"))
                finishJob(Outcome::Cancelled, QStringLiteral("cancelled"));
        });
    } else if (!m_job.queued) {
        finishJob(Outcome::Cancelled, QStringLiteral("cancelled"));   // nothing left GameHQ yet
    }
}

void TelegramIntegratedProvider::failJob(Outcome outcome, const QString& code)
{
    finishJob(outcome, code);
}

void TelegramIntegratedProvider::finishJob(Outcome outcome, const QString& code)
{
    if (!m_job.active)
        return;
    Result r;
    r.jobId = m_job.id;
    r.providerId = id();
    r.targetId = m_job.targetId;
    r.outcome = outcome;
    r.errorCode = code;
    m_job = {};
    m_earlySucceeded.clear();
    m_earlyFailed.clear();
    qInfo() << "Telegram: share ended" << outcomeName(outcome) << code;
    emit jobFinished(r);
}

} // namespace share
