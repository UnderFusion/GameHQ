#include "FakeTdTransport.h"
#include "share/providers/TelegramIntegratedProvider.h"
#include "telegram/TdRuntime.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

using namespace share;

Q_DECLARE_METATYPE(QVector<share::Target>)

// t24: Telegram Integrated targets and sending against a scripted TDLib.
// The contract under test: Sent only after TDLib confirms the outgoing
// message, no duplicate sends, only the picked file and target, and honest
// outcomes when the link drops or the user cancels.
class TestTelegramProvider : public QObject
{
    Q_OBJECT

    QString m_ns;
    QTemporaryDir m_dir;
    FakeTdTransport* m_fake = nullptr;
    std::unique_ptr<telegram::Account> m_account;
    std::unique_ptr<telegram::TdRuntime> m_runtime;
    std::unique_ptr<TelegramIntegratedProvider> m_provider;

    static QJsonObject privateChat(qint64 id, const QString& title, qint64 userId)
    {
        return { { "@type", "chat" }, { "id", id }, { "title", title },
                 { "type", QJsonObject{ { "@type", "chatTypePrivate" }, { "user_id", userId } } } };
    }
    static QJsonObject groupChat(qint64 id, const QString& title, bool photos = true, bool videos = true)
    {
        return { { "@type", "chat" }, { "id", id }, { "title", title },
                 { "type", QJsonObject{ { "@type", "chatTypeBasicGroup" }, { "basic_group_id", id } } },
                 { "permissions", QJsonObject{ { "can_send_basic_messages", true },
                                               { "can_send_photos", photos },
                                               { "can_send_videos", videos } } } };
    }
    static QJsonObject channelChat(qint64 id)
    {
        return { { "@type", "chat" }, { "id", id }, { "title", "News" },
                 { "type", QJsonObject{ { "@type", "chatTypeSupergroup" }, { "supergroup_id", id },
                                        { "is_channel", true } } } };
    }
    static QJsonObject regularUser(qint64 id, const QString& first)
    {
        return { { "@type", "user" }, { "id", id }, { "first_name", first }, { "last_name", "" },
                 { "type", QJsonObject{ { "@type", "userTypeRegular" } } } };
    }

    QString writeFile(const QString& name, qint64 size = 100)
    {
        const QString path = m_dir.filePath(name);
        QFile f(path);
        f.open(QIODevice::WriteOnly);
        f.resize(size);
        f.close();
        return path;
    }
    Request requestFor(const QString& path)
    {
        return Request::fromCapture(path, QStringLiteral("Game"));
    }
    static Target target(const QString& id)
    {
        Target t;
        t.id = id;
        t.providerId = QStringLiteral("telegram.integrated");
        return t;
    }
    static Job job(const QString& id = QStringLiteral("job1"))
    {
        Job j;
        j.id = id;
        return j;
    }
    static QJsonObject sendReply(qint64 id)
    {
        return QJsonObject{ { "@type", "message" }, { "id", id } };
    }

    telegram::Account::Config baseConfig()
    {
        return telegram::Account::Config{ share::SecretStore(m_ns), m_dir.filePath("sessions"), {},
                                          [] { return QStringLiteral("ready"); },
                                          QStringLiteral("0.7.9"), QStringLiteral("en"), 0 };
    }

    // A connected account with a resumable session; `respond` scripts chat traffic.
    void connectWith(std::function<QJsonObject(const QJsonObject&)> respond = {})
    {
        telegram::Account::Config cfg = baseConfig();
        cfg.transportFactory = [this, respond] {
            auto t = std::make_unique<FakeTdTransport>(FakeTdTransport::Script{ .savedSession = true });
            t->responder = respond;
            m_fake = t.get();
            return std::unique_ptr<telegram::TdTransport>(std::move(t));
        };
        m_account = std::make_unique<telegram::Account>(std::move(cfg));
        m_account->setCredentials(QStringLiteral("12345"),
                                  QStringLiteral("0123456789abcdef0123456789abcdef"));
        m_runtime = std::make_unique<telegram::TdRuntime>(QString(), QString(), QString());
        m_provider = std::make_unique<TelegramIntegratedProvider>(m_account.get(), m_runtime.get());
        QVERIFY(m_account->connectAccount());
        QTRY_COMPARE(m_account->stateName(), QStringLiteral("connected"));
    }

private slots:
    void initTestCase()
    {
        qRegisterMetaType<QVector<share::Target>>();
        qRegisterMetaType<share::Result>();
    }
    void init()
    {
        m_ns = QStringLiteral("GameHQ.Test.") + QUuid::createUuid().toString(QUuid::Id128).left(12);
    }
    void cleanup()
    {
        m_provider.reset();
        m_account.reset();
        share::SecretStore(m_ns).removeAll(telegram::Account::providerId());
        QDir(m_dir.filePath("sessions")).removeRecursively();
        m_fake = nullptr;
    }

    void mappingKeepsOnlyShareableDestinations()
    {
        connectWith();
        const QVector<QJsonObject> chats = {
            privateChat(1, "Alice", 100),
            groupChat(2, "Squad"),
            groupChat(3, "NoPhotos", false, true),
            channelChat(4),
            QJsonObject{ { "@type", "chat" }, { "id", 5 }, { "title", "Secret" },
                         { "type", QJsonObject{ { "@type", "chatTypeSecret" } } } },
        };
        QJsonObject bot = regularUser(300, "Bot");
        bot.insert("type", QJsonObject{ { "@type", "userTypeBot" } });
        const QVector<QJsonObject> users = { regularUser(100, "Alice"),   // already has a chat
                                             regularUser(200, "Bob"), bot };
        const QVector<Target> photo = m_provider->targetsFrom(chats, users, MediaKind::Image);
        QStringList ids;
        for (const Target& t : photo)
            ids << t.id;
        QCOMPARE(ids, (QStringList{ "c:1", "c:2", "u:200" }));   // NoPhotos, channel, secret, dup, bot dropped
        QCOMPARE(photo.at(0).kind, TargetKind::Contact);
        QCOMPARE(photo.at(1).kind, TargetKind::Group);
        QCOMPARE(photo.at(2).displayName, QStringLiteral("Bob"));

        const QVector<Target> video = m_provider->targetsFrom(chats, {}, MediaKind::Video);
        QStringList vids;
        for (const Target& t : video)
            vids << t.id;
        QCOMPARE(vids, (QStringList{ "c:1", "c:2", "c:3" }));   // videos are allowed in c:3
    }

    void defaultListFollowsTelegramOrdering()
    {
        connectWith([](const QJsonObject& r) -> QJsonObject {
            const QString t = r.value("@type").toString();
            if (t == "loadChats")
                return QJsonObject{ { "@type", "ok" } };
            if (t == "getChats")
                return QJsonObject{ { "@type", "chats" }, { "chat_ids", QJsonArray{ 2, 1 } } };
            if (t == "getChat")
                return r.value("chat_id").toVariant().toLongLong() == 1 ? privateChat(1, "Alice", 100)
                                                                        : groupChat(2, "Squad");
            return {};
        });
        QSignalSpy spy(m_provider.get(), &Provider::targetsReady);
        m_provider->requestTargets("q1", requestFor(writeFile("a.png")), QString());
        QTRY_COMPARE(spy.count(), 1);
        QCOMPARE(spy.first().at(2).toString(), QString());
        const auto targets = spy.first().at(1).value<QVector<Target>>();
        QCOMPARE(targets.size(), 2);
        QCOMPARE(targets.at(0).displayName, QStringLiteral("Squad"));   // Telegram's order is kept
        QCOMPARE(targets.at(1).displayName, QStringLiteral("Alice"));
    }

    void searchMergesChatsAndContacts()
    {
        connectWith([](const QJsonObject& r) -> QJsonObject {
            const QString t = r.value("@type").toString();
            if (t == "searchChats")
                return QJsonObject{ { "@type", "chats" }, { "chat_ids", QJsonArray{ 1 } } };
            if (t == "searchContacts")
                return QJsonObject{ { "@type", "users" }, { "user_ids", QJsonArray{ 100, 200 } } };
            if (t == "getChat")
                return privateChat(1, "Alice", 100);
            if (t == "getUser") {
                const qint64 id = r.value("user_id").toVariant().toLongLong();
                return regularUser(id, id == 100 ? "Alice" : "Bob");
            }
            return {};
        });
        QSignalSpy spy(m_provider.get(), &Provider::targetsReady);
        m_provider->requestTargets("q2", requestFor(writeFile("b.png")), "  al ");
        QTRY_COMPARE(spy.count(), 1);
        const auto targets = spy.first().at(1).value<QVector<Target>>();
        QStringList ids;
        for (const Target& t : targets)
            ids << t.id;
        QCOMPARE(ids, (QStringList{ "c:1", "u:200" }));
        QCOMPARE(m_fake->firstOfType("searchChats").value("query").toString(), QStringLiteral("al"));
    }

    void staleTargetQueryIsDropped()
    {
        connectWith([](const QJsonObject& r) -> QJsonObject {
            const QString t = r.value("@type").toString();
            if (t == "searchChats")
                return QJsonObject{ { "@type", "chats" }, { "chat_ids", QJsonArray{} } };
            if (t == "searchContacts")
                return QJsonObject{ { "@type", "users" }, { "user_ids", QJsonArray{} } };
            return {};
        });
        QSignalSpy spy(m_provider.get(), &Provider::targetsReady);
        const Request req = requestFor(writeFile("c.png"));
        m_provider->requestTargets("old", req, "a");
        m_provider->requestTargets("new", req, "ab");
        QTRY_VERIFY(spy.count() >= 1);
        QTest::qWait(30);
        for (const auto& call : spy)
            QCOMPARE(call.at(0).toString(), QStringLiteral("new"));
    }

    void noSessionMeansNotConnected()
    {
        m_account = std::make_unique<telegram::Account>(baseConfig());
        m_runtime = std::make_unique<telegram::TdRuntime>(QString(), QString(), QString());
        m_provider = std::make_unique<TelegramIntegratedProvider>(m_account.get(), m_runtime.get());
        QCOMPARE(m_provider->authState(), AuthState::Disconnected);
        QSignalSpy spy(m_provider.get(), &Provider::targetsReady);
        m_provider->requestTargets("q", requestFor(writeFile("d.png")), QString());
        QTRY_COMPARE(spy.count(), 1);
        QCOMPARE(spy.first().at(2).toString(), QStringLiteral("not_connected"));
    }

    void photoIsSentAndSentOnlyAfterConfirmation()
    {
        connectWith([](const QJsonObject& r) -> QJsonObject {
            return r.value("@type").toString() == "sendMessage" ? sendReply(77) : QJsonObject();
        });
        const QString path = writeFile("shot.png", 2048);
        QSignalSpy finished(m_provider.get(), &Provider::jobFinished);
        m_provider->start(job(), requestFor(path), target("c:42"));
        QTRY_VERIFY(m_fake->sentType("sendMessage"));
        const QJsonObject send = m_fake->firstOfType("sendMessage");
        QCOMPARE(send.value("chat_id").toVariant().toLongLong(), 42LL);
        const QJsonObject content = send.value("input_message_content").toObject();
        QCOMPARE(content.value("@type").toString(), QStringLiteral("inputMessagePhoto"));
        QCOMPARE(QDir::fromNativeSeparators(content.value("photo").toObject().value("path").toString()),
                 QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath()));

        QTest::qWait(50);
        QCOMPARE(finished.count(), 0);   // accepted by TDLib is not delivered
        m_fake->push(QJsonObject{ { "@type", "updateMessageSendSucceeded" }, { "old_message_id", 77 },
                                  { "message", QJsonObject{ { "id", 900 } } } });
        QTRY_COMPARE(finished.count(), 1);
        const Result r = finished.first().at(0).value<Result>();
        QCOMPARE(r.outcome, Outcome::Sent);
        QCOMPARE(r.jobId, QStringLiteral("job1"));
        QCOMPARE(m_fake->countOfType("sendMessage"), 1);   // never resent
    }

    void confirmationForAnotherMessageIsIgnored()
    {
        connectWith([](const QJsonObject& r) -> QJsonObject {
            return r.value("@type").toString() == "sendMessage" ? sendReply(77) : QJsonObject();
        });
        QSignalSpy finished(m_provider.get(), &Provider::jobFinished);
        m_provider->start(job(), requestFor(writeFile("s.png")), target("c:1"));
        QTRY_VERIFY(m_fake->sentType("sendMessage"));
        QTest::qWait(30);
        m_fake->push(QJsonObject{ { "@type", "updateMessageSendSucceeded" }, { "old_message_id", 5 } });
        QTest::qWait(50);
        QCOMPARE(finished.count(), 0);
    }

    void telegramRefusalIsFailedWithReason()
    {
        connectWith([](const QJsonObject& r) -> QJsonObject {
            return r.value("@type").toString() == "sendMessage" ? sendReply(9) : QJsonObject();
        });
        QSignalSpy finished(m_provider.get(), &Provider::jobFinished);
        m_provider->start(job(), requestFor(writeFile("f.png")), target("c:1"));
        QTRY_VERIFY(m_fake->sentType("sendMessage"));
        QTest::qWait(30);
        m_fake->push(QJsonObject{ { "@type", "updateMessageSendFailed" }, { "old_message_id", 9 },
                                  { "error", QJsonObject{ { "code", 429 },
                                                          { "message", "Too Many Requests: retry after 30" } } } });
        QTRY_COMPARE(finished.count(), 1);
        const Result r = finished.first().at(0).value<Result>();
        QCOMPARE(r.outcome, Outcome::Failed);
        QCOMPARE(r.errorCode, QStringLiteral("rate_limited"));
    }

    void immediateSendErrorIsFailed()
    {
        connectWith([](const QJsonObject& r) -> QJsonObject {
            if (r.value("@type").toString() == "sendMessage")
                return QJsonObject{ { "@type", "error" }, { "code", 400 },
                                    { "message", "CHAT_WRITE_FORBIDDEN" } };
            return {};
        });
        QSignalSpy finished(m_provider.get(), &Provider::jobFinished);
        m_provider->start(job(), requestFor(writeFile("g.png")), target("c:1"));
        QTRY_COMPARE(finished.count(), 1);
        const Result r = finished.first().at(0).value<Result>();
        QCOMPARE(r.outcome, Outcome::Failed);
        QCOMPARE(r.errorCode, QStringLiteral("not_allowed"));
    }

    void earlyConfirmationRaceStillSends()
    {
        // The confirmation update overtakes the reply to sendMessage.
        connectWith([this](const QJsonObject& r) -> QJsonObject {
            if (r.value("@type").toString() == "sendMessage") {
                m_fake->push(QJsonObject{ { "@type", "updateMessageSendSucceeded" },
                                          { "old_message_id", 31 } });
                return sendReply(31);
            }
            return {};
        });
        QSignalSpy finished(m_provider.get(), &Provider::jobFinished);
        m_provider->start(job(), requestFor(writeFile("h.png")), target("c:1"));
        QTRY_COMPARE(finished.count(), 1);
        QCOMPARE(finished.first().at(0).value<Result>().outcome, Outcome::Sent);
    }

    void largePngGoesAsDocumentAndClipAsVideo()
    {
        connectWith([](const QJsonObject& r) -> QJsonObject {
            return r.value("@type").toString() == "sendMessage" ? sendReply(1) : QJsonObject();
        });
        QSignalSpy finished(m_provider.get(), &Provider::jobFinished);
        m_provider->start(job("a"), requestFor(writeFile("big.png", 11LL * 1024 * 1024)), target("c:1"));
        QTRY_COMPARE(m_fake->countOfType("sendMessage"), 1);
        QCOMPARE(m_fake->firstOfType("sendMessage").value("input_message_content").toObject()
                     .value("@type").toString(), QStringLiteral("inputMessageDocument"));
        m_fake->push(QJsonObject{ { "@type", "updateMessageSendSucceeded" }, { "old_message_id", 1 } });
        QTRY_COMPARE(finished.count(), 1);

        m_fake->sent.clear();
        m_provider->start(job("b"), requestFor(writeFile("clip.mp4", 5000)), target("c:1"));
        QTRY_COMPARE(m_fake->countOfType("sendMessage"), 1);
        QCOMPARE(m_fake->firstOfType("sendMessage").value("input_message_content").toObject()
                     .value("@type").toString(), QStringLiteral("inputMessageVideo"));
    }

    void contactWithoutChatOpensPrivateChatFirst()
    {
        connectWith([](const QJsonObject& r) -> QJsonObject {
            const QString t = r.value("@type").toString();
            if (t == "createPrivateChat")
                return privateChat(555, "Bob", 200);
            if (t == "sendMessage")
                return sendReply(3);
            return {};
        });
        m_provider->start(job(), requestFor(writeFile("u.png")), target("u:200"));
        QTRY_VERIFY(m_fake->sentType("sendMessage"));
        QCOMPARE(m_fake->firstOfType("createPrivateChat").value("user_id").toVariant().toLongLong(), 200LL);
        QCOMPARE(m_fake->firstOfType("sendMessage").value("chat_id").toVariant().toLongLong(), 555LL);
    }

    void malformedTargetIsRefusedWithoutSending()
    {
        connectWith();
        QSignalSpy finished(m_provider.get(), &Provider::jobFinished);
        m_provider->start(job(), requestFor(writeFile("m.png")), target("evil"));
        QTRY_COMPARE(finished.count(), 1);
        QCOMPARE(finished.first().at(0).value<Result>().errorCode, QStringLiteral("unknown_target"));
        QVERIFY(!m_fake->sentType("sendMessage"));
    }

    void lostConnectionMidUploadIsUnconfirmed()
    {
        connectWith([](const QJsonObject& r) -> QJsonObject {
            return r.value("@type").toString() == "sendMessage" ? sendReply(8) : QJsonObject();
        });
        QSignalSpy finished(m_provider.get(), &Provider::jobFinished);
        m_provider->start(job(), requestFor(writeFile("l.png")), target("c:1"));
        QTRY_VERIFY(m_fake->sentType("sendMessage"));
        QTest::qWait(30);
        m_fake->push(QJsonObject{ { "@type", "updateAuthorizationState" },
                                  { "authorization_state",
                                    QJsonObject{ { "@type", "authorizationStateClosed" } } } });
        QTRY_COMPARE(finished.count(), 1);
        const Result r = finished.first().at(0).value<Result>();
        QCOMPARE(r.outcome, Outcome::Unconfirmed);   // never a guessed Failed or Sent
        QCOMPARE(r.errorCode, QStringLiteral("connection_lost"));
    }

    void cancelDeletesThePendingMessage()
    {
        connectWith([](const QJsonObject& r) -> QJsonObject {
            const QString t = r.value("@type").toString();
            if (t == "sendMessage")
                return sendReply(66);
            if (t == "deleteMessages")
                return QJsonObject{ { "@type", "ok" } };
            return {};
        });
        QSignalSpy finished(m_provider.get(), &Provider::jobFinished);
        m_provider->start(job(), requestFor(writeFile("x.png")), target("c:9"));
        QTRY_VERIFY(m_fake->sentType("sendMessage"));
        QTest::qWait(30);
        m_provider->cancel("job1");
        QTRY_COMPARE(finished.count(), 1);
        QCOMPARE(finished.first().at(0).value<Result>().outcome, Outcome::Cancelled);
        const QJsonObject del = m_fake->firstOfType("deleteMessages");
        QCOMPARE(del.value("chat_id").toVariant().toLongLong(), 9LL);
        QCOMPARE(del.value("message_ids").toArray().first().toVariant().toLongLong(), 66LL);
    }

    void cancelForAnotherJobIsIgnored()
    {
        connectWith([](const QJsonObject& r) -> QJsonObject {
            return r.value("@type").toString() == "sendMessage" ? sendReply(66) : QJsonObject();
        });
        QSignalSpy finished(m_provider.get(), &Provider::jobFinished);
        m_provider->start(job(), requestFor(writeFile("y.png")), target("c:9"));
        QTRY_VERIFY(m_fake->sentType("sendMessage"));
        m_provider->cancel("someone-else");
        QTest::qWait(50);
        QCOMPARE(finished.count(), 0);
        QVERIFY(!m_fake->sentType("deleteMessages"));
    }

    void disconnectRemovesTheLocalSession()
    {
        connectWith();
        QFile marker(m_account->sessionDirectory(true) + "/db.marker");
        QVERIFY(marker.open(QIODevice::WriteOnly));
        marker.close();
        QVERIFY(m_account->hasSavedSession());
        m_provider->disconnectAccount();
        QTRY_VERIFY(!m_account->hasSavedSession());
        QCOMPARE(m_provider->authState(), AuthState::Disconnected);
    }

    void accountAccessIsFullSessionAndDeclaredHonestly()
    {
        connectWith();
        QCOMPARE(m_provider->accountAccess(), AccountAccess::FullAccountSession);
        QVERIFY(m_provider->capabilities().testFlag(Capability::RequiresAccount));
        QVERIFY(m_provider->capabilities().testFlag(Capability::DirectSend));
        QVERIFY(!m_provider->privacyNotice().isEmpty());
    }
};

QTEST_MAIN(TestTelegramProvider)
#include "tst_telegramprovider.moc"
