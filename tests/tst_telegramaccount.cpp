#include "FakeTdTransport.h"
#include "telegram/TelegramAccount.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

using telegram::Account;

// t23: the Telegram Integrated auth/session lifecycle against a scripted
// TDLib. Secrets go to a throwaway Credential Manager namespace that each test
// wipes, and the session lives in a temp directory.
class TestTelegramAccount : public QObject
{
    Q_OBJECT

    static constexpr const char* kId = "12345";
    static constexpr const char* kHash = "0123456789abcdef0123456789abcdef";

    QString m_ns;
    QTemporaryDir m_dir;
    FakeTdTransport* m_fake = nullptr;   // owned by the account

    std::unique_ptr<Account> makeAccount(FakeTdTransport::Script script, int idleMs = 0,
                                         bool withFactory = true)
    {
        Account::Config cfg{ share::SecretStore(m_ns), m_dir.filePath("sessions"), {},
                             [] { return QStringLiteral("ready"); }, QStringLiteral("0.7.9"),
                             QStringLiteral("en"), idleMs };
        if (withFactory) {
            cfg.transportFactory = [this, script] {
                auto t = std::make_unique<FakeTdTransport>(script);
                m_fake = t.get();
                return std::unique_ptr<telegram::TdTransport>(std::move(t));
            };
        } else {
            cfg.runtimeStatus = [] { return QStringLiteral("not_installed"); };
        }
        return std::make_unique<Account>(std::move(cfg));
    }

private slots:
    void init()
    {
        m_ns = QStringLiteral("GameHQ.Test.") + QUuid::createUuid().toString(QUuid::Id128).left(12);
    }
    void cleanup()
    {
        share::SecretStore(m_ns).removeAll(Account::providerId());
        QDir(m_dir.filePath("sessions")).removeRecursively();
        m_fake = nullptr;
    }

    void credentialsAreValidatedAndStoredOutsideConfig()
    {
        auto a = makeAccount({});
        QVERIFY(!a->hasCredentials());
        QCOMPARE(a->setCredentials("abc", kHash), QStringLiteral("invalid_api_id"));
        QCOMPARE(a->setCredentials("0", kHash), QStringLiteral("invalid_api_id"));
        QCOMPARE(a->setCredentials(kId, "short"), QStringLiteral("invalid_api_hash"));
        QVERIFY(!a->hasCredentials());
        QCOMPARE(a->setCredentials(kId, kHash), QString());
        QVERIFY(a->hasCredentials());
        a->forgetCredentials();
        QVERIFY(!a->hasCredentials());
    }

    void connectNeedsCredentials()
    {
        auto a = makeAccount({});
        QVERIFY(!a->connectAccount());
        QCOMPARE(a->stateName(), QStringLiteral("error"));
        QCOMPARE(a->errorCode(), QStringLiteral("no_credentials"));
        QVERIFY(!m_fake);   // no client is created without credentials
    }

    void missingRuntimeIsReportedNotCrashed()
    {
        auto a = makeAccount({}, 0, false);
        a->setCredentials(kId, kHash);
        QVERIFY(!a->connectAccount());
        QCOMPARE(a->errorCode(), QStringLiteral("runtime_not_installed"));
    }

    void fullLoginWithTwoStepPassword()
    {
        auto a = makeAccount({ .needsPassword = true });
        a->setCredentials(kId, kHash);
        QVERIFY(a->connectAccount());
        QCOMPARE(a->stateName(), QStringLiteral("starting"));
        QTRY_COMPARE(a->stateName(), QStringLiteral("wait_phone"));

        // Parameters: confined directory, minimal persistence, encrypted DB.
        const QJsonObject p = m_fake->firstOfType("setTdlibParameters");
        QCOMPARE(p.value("api_id").toInt(), 12345);
        QVERIFY(p.value("database_directory").toString().startsWith(
            QDir::fromNativeSeparators(m_dir.filePath("sessions"))));
        QVERIFY(!p.value("use_message_database").toBool());
        QVERIFY(!p.value("use_file_database").toBool());
        QVERIFY(!p.value("use_secret_chats").toBool());
        QVERIFY(!p.value("database_encryption_key").toString().isEmpty());

        a->submitPhone("+48123456789");
        QTRY_COMPARE(a->stateName(), QStringLiteral("wait_code"));
        a->submitCode("1111");
        QTRY_COMPARE(a->stateName(), QStringLiteral("wait_password"));
        a->submitPassword("pw");
        QTRY_COMPARE(a->stateName(), QStringLiteral("connected"));
        QVERIFY(a->errorCode().isEmpty());
        // GameHQ never shows the user online.
        QVERIFY(m_fake->sentType("setOption"));
    }

    void wrongAnswersKeepThePromptAndSayWhy()
    {
        auto a = makeAccount({});
        a->setCredentials(kId, kHash);
        a->connectAccount();
        QTRY_COMPARE(a->stateName(), QStringLiteral("wait_phone"));
        a->submitPhone("+bad");
        QTRY_COMPARE(a->errorCode(), QStringLiteral("phone_invalid"));
        QCOMPARE(a->stateName(), QStringLiteral("wait_phone"));
        a->submitPhone("+48123456789");
        QTRY_COMPARE(a->stateName(), QStringLiteral("wait_code"));
        QVERIFY(a->errorCode().isEmpty());   // a new step clears the old reason
        a->submitCode("0000");
        QTRY_COMPARE(a->errorCode(), QStringLiteral("code_invalid"));
        QCOMPARE(a->stateName(), QStringLiteral("wait_code"));
        a->submitCode("9999");
        QTRY_COMPARE(a->errorCode(), QStringLiteral("rate_limited"));
        a->submitCode("1111");
        QTRY_COMPARE(a->stateName(), QStringLiteral("connected"));
    }

    void answersOutOfSequenceAreIgnored()
    {
        auto a = makeAccount({});
        a->setCredentials(kId, kHash);
        a->connectAccount();
        QTRY_COMPARE(a->stateName(), QStringLiteral("wait_phone"));
        a->submitCode("1111");        // not asked for a code yet
        a->submitPassword("pw");
        QVERIFY(!m_fake->sentType("checkAuthenticationCode"));
        QVERIFY(!m_fake->sentType("checkAuthenticationPassword"));
    }

    void savedSessionResumesWithoutPrompts()
    {
        auto a = makeAccount({ .savedSession = true });
        a->setCredentials(kId, kHash);
        a->connectAccount();
        QTRY_COMPARE(a->stateName(), QStringLiteral("connected"));
        QVERIFY(!m_fake->sentType("setAuthenticationPhoneNumber"));
    }

    void unsupportedAuthStepStopsCleanly()
    {
        auto a = makeAccount({ .unsupportedStep = true });
        a->setCredentials(kId, kHash);
        a->connectAccount();
        QTRY_COMPARE(a->stateName(), QStringLiteral("error"));
        QCOMPARE(a->errorCode(), QStringLiteral("unsupported_auth"));
        QTRY_VERIFY(a->transport() == nullptr);   // the client was closed and released
    }

    void startFailureIsAnError()
    {
        auto a = makeAccount({ .failStart = true });
        a->setCredentials(kId, kHash);
        QVERIFY(!a->connectAccount());
        QCOMPARE(a->errorCode(), QStringLiteral("start_failed"));
    }

    void idleClientIsReleasedButSessionKept()
    {
        auto a = makeAccount({ .savedSession = true }, 60);
        a->setCredentials(kId, kHash);
        a->connectAccount();
        QTRY_COMPARE(a->stateName(), QStringLiteral("connected"));
        QVERIFY(!a->sessionDirectory(true).isEmpty());
        QFile marker(a->sessionDirectory() + "/db.marker");
        QVERIFY(marker.open(QIODevice::WriteOnly));
        marker.close();
        QTRY_COMPARE(a->stateName(), QStringLiteral("disconnected"));
        QVERIFY(a->hasSavedSession());
        QVERIFY(a->hasCredentials());
    }

    void touchKeepsAnActiveClientAlive()
    {
        auto a = makeAccount({ .savedSession = true }, 150);
        a->setCredentials(kId, kHash);
        a->connectAccount();
        QTRY_COMPARE(a->stateName(), QStringLiteral("connected"));
        for (int i = 0; i < 4; ++i) {
            QTest::qWait(60);
            a->touch();
        }
        QCOMPARE(a->stateName(), QStringLiteral("connected"));
    }

    void disconnectSignsOutAndDeletesLocalSession()
    {
        auto a = makeAccount({ .savedSession = true });
        a->setCredentials(kId, kHash);
        a->connectAccount();
        QTRY_COMPARE(a->stateName(), QStringLiteral("connected"));
        const QString dir = a->sessionDirectory();
        QFile marker(dir + "/db.marker");
        QVERIFY(marker.open(QIODevice::WriteOnly));
        marker.close();
        FakeTdTransport* fake = m_fake;

        QSignalSpy closed(a.get(), &Account::clientClosed);
        a->disconnectAndRemoveSession();
        QCOMPARE(a->stateName(), QStringLiteral("logging_out"));
        QVERIFY(fake->sentType("logOut"));
        QTRY_COMPARE(closed.count(), 1);
        QCOMPARE(a->stateName(), QStringLiteral("disconnected"));
        QVERIFY(!QDir(dir).exists());
        QVERIFY(!a->hasSavedSession());
        QVERIFY(a->hasCredentials());   // removing the session is not forgetting the app credentials
        bool found = true;
        share::SecretStore(m_ns).read(Account::providerId(), "db-key", &found);
        QVERIFY(!found);
    }

    void disconnectWithoutAClientStillWipes()
    {
        auto a = makeAccount({ .savedSession = true }, 60);
        a->setCredentials(kId, kHash);
        a->connectAccount();
        QTRY_COMPARE(a->stateName(), QStringLiteral("connected"));
        QFile marker(a->sessionDirectory() + "/db.marker");
        QVERIFY(marker.open(QIODevice::WriteOnly));
        marker.close();
        QTRY_COMPARE(a->stateName(), QStringLiteral("disconnected"));   // idle release
        QVERIFY(a->hasSavedSession());
        a->disconnectAndRemoveSession(true);
        QVERIFY(!a->hasSavedSession());
        QVERIFY(!a->hasCredentials());   // forgetToo
    }

    void hungClientCannotBlockRemoval()
    {
        auto a = makeAccount({ .savedSession = true, .swallowClose = true });
        a->setCredentials(kId, kHash);
        a->connectAccount();
        QTRY_COMPARE(a->stateName(), QStringLiteral("connected"));
        QFile marker(a->sessionDirectory() + "/db.marker");
        QVERIFY(marker.open(QIODevice::WriteOnly));
        marker.close();
        a->disconnectAndRemoveSession();
        QTRY_VERIFY_WITH_TIMEOUT(!a->hasSavedSession(), 12000);
        QCOMPARE(a->stateName(), QStringLiteral("disconnected"));
    }

    void nonAuthUpdatesAreDroppedUnlessObserved()
    {
        auto a = makeAccount({ .savedSession = true });
        a->setCredentials(kId, kHash);
        a->connectAccount();
        QTRY_COMPARE(a->stateName(), QStringLiteral("connected"));
        // A message arriving must change nothing and go nowhere.
        m_fake->push(QJsonObject{ { "@type", "updateNewMessage" } });
        QTest::qWait(30);
        QCOMPARE(a->stateName(), QStringLiteral("connected"));

        int seen = 0;
        a->setUpdateObserver([&seen](const QJsonObject&) { ++seen; });
        m_fake->push(QJsonObject{ { "@type", "updateMessageSendSucceeded" } });
        QTRY_COMPARE(seen, 1);
    }
};

QTEST_MAIN(TestTelegramAccount)
#include "tst_telegramaccount.moc"
