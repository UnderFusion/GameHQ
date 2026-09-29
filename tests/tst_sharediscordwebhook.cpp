#include "share/ShareService.h"
#include "share/providers/DiscordWebhookProvider.h"
#include "share/providers/DiscordWebhookStore.h"

#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

using namespace share;

namespace
{
QStringList g_log;
void captureLog(QtMsgType, const QMessageLogContext&, const QString& message)
{
    g_log << message;
}

// A one-connection-at-a-time fake Discord: records the last request and
// answers with whatever the case configured.
class FakeDiscord : public QTcpServer
{
public:
    int status = 200;
    QByteArray body = R"({"id":"1234567890","type":0})";
    QByteArray extraHeaders;
    bool hang = false;         // read the whole request, never answer
    bool dropAfterHeaders = false;

    QByteArray requestHead;
    QByteArray requestBody;
    int requests = 0;

    FakeDiscord()
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            QTcpSocket* s = nextPendingConnection();
            auto* buffer = new QByteArray;
            connect(s, &QTcpSocket::readyRead, this, [this, s, buffer] {
                buffer->append(s->readAll());
                const int headEnd = buffer->indexOf("\r\n\r\n");
                if (headEnd < 0)
                    return;
                const QByteArray head = buffer->left(headEnd);
                const int at = head.toLower().indexOf("content-length:");
                const int length = at < 0 ? 0 : head.mid(at + 15).split('\r').first().trimmed().toInt();
                if (buffer->size() < headEnd + 4 + length)
                    return;
                requestHead = head;
                requestBody = buffer->mid(headEnd + 4, length);
                ++requests;
                buffer->clear();
                if (hang)
                    return;
                if (dropAfterHeaders) {
                    s->abort();
                    return;
                }
                QByteArray reply = "HTTP/1.1 " + QByteArray::number(status) + " X\r\n"
                                   "Content-Type: application/json\r\n" + extraHeaders
                                   + "Content-Length: " + QByteArray::number(body.size())
                                   + "\r\nConnection: close\r\n\r\n" + body;
                s->write(reply);
                s->disconnectFromHost();
            });
            connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
        });
    }
};
} // namespace

// Discord channel provider (t17): multipart upload, response-backed "sent",
// honest failure codes, secrets kept out of files, logs and results.
class TestShareDiscordWebhook : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString m_namespace;
    const QString m_token = QStringLiteral("AbCdEfGhIjKlMnOpQrStUvWxYz0123456789_-tokenpart");

    QString capture(const QString& name, const QByteArray& content = "media-bytes")
    {
        const QString path = m_dir.filePath(name);
        QFile f(path);
        f.open(QIODevice::WriteOnly);
        f.write(content);
        return path;
    }

    QString webhookUrl(const FakeDiscord& server) const
    {
        return QStringLiteral("http://127.0.0.1:%1/api/webhooks/123456789012345678/%2")
            .arg(server.serverPort())
            .arg(m_token);
    }

    DiscordWebhookStore makeStore()
    {
        return DiscordWebhookStore(m_dir.filePath("share/discord-webhooks.json"),
                                   SecretStore(m_namespace),
                                   [](const QString& url) { return url.startsWith("http://127.0.0.1:"); });
    }

private slots:
    void initTestCase()
    {
        m_namespace = QStringLiteral("GameHQ.Test.Webhook.")
                      + QUuid::createUuid().toString(QUuid::Id128);
        qInstallMessageHandler(captureLog);
    }

    void cleanupTestCase()
    {
        SecretStore(m_namespace).removeAll(QStringLiteral("discord.webhook"));
        qInstallMessageHandler(nullptr);
    }

    void cleanup()
    {
        DiscordWebhookStore store = makeStore();
        store.removeAll();
    }

    void recognisesOnlyDiscordWebhookUrls_data()
    {
        QTest::addColumn<QString>("url");
        QTest::addColumn<bool>("valid");
        const QString ok = QStringLiteral("https://discord.com/api/webhooks/123456789012345678/")
                           + QString(40, QLatin1Char('a'));
        QTest::newRow("discord.com") << ok << true;
        QTest::newRow("versioned") << QString(ok).replace("/api/", "/api/v10/") << true;
        QTest::newRow("discordapp") << QString(ok).replace("discord.com", "discordapp.com") << true;
        QTest::newRow("http") << QString(ok).replace("https", "http") << false;
        QTest::newRow("other host") << QString(ok).replace("discord.com", "evil.example") << false;
        QTest::newRow("lookalike host") << QString(ok).replace("discord.com", "discord.com.evil.io") << false;
        QTest::newRow("userinfo") << QString(ok).replace("https://", "https://a:b@") << false;
        QTest::newRow("port") << QString(ok).replace("discord.com", "discord.com:8443") << false;
        QTest::newRow("query") << ok + "?wait=true" << false;
        QTest::newRow("short token") << QStringLiteral("https://discord.com/api/webhooks/123456789012345678/abc") << false;
        QTest::newRow("empty") << QString() << false;
    }

    void recognisesOnlyDiscordWebhookUrls()
    {
        QFETCH(QString, url);
        QFETCH(bool, valid);
        QCOMPARE(DiscordWebhookStore::isDiscordWebhookUrl(url), valid);
    }

    void storeKeepsTheUrlOutOfTheMetadataFile()
    {
        FakeDiscord server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        DiscordWebhookStore store = makeStore();
        QString id;
        QCOMPARE(store.add("  Clips   channel ", webhookUrl(server), &id), QString());
        QCOMPARE(store.list().size(), 1);
        QCOMPARE(store.list().first().name, QStringLiteral("Clips channel"));
        QCOMPARE(store.webhookUrl(id), webhookUrl(server));

        QByteArray json;
        {
            QFile f(m_dir.filePath("share/discord-webhooks.json"));
            QVERIFY(f.open(QIODevice::ReadOnly));
            json = f.readAll();
        }   // closed: an open handle would block the store's atomic rewrite
        QVERIFY(!json.contains(m_token.toUtf8()));
        QVERIFY(!json.contains("127.0.0.1"));

        QCOMPARE(store.add("", webhookUrl(server)), QStringLiteral("invalid_name"));
        QCOMPARE(store.add("x", "https://example.com/hook"), QStringLiteral("invalid_secret"));

        QVERIFY(store.remove(id));
        QVERIFY(store.webhookUrl(id).isEmpty());   // the credential went too
        QVERIFY(!store.remove(id));
    }

    void noDestinationsMeansUnavailable()
    {
        DiscordWebhookStore store = makeStore();
        DiscordWebhookProvider p(&store);
        Service s;
        s.registry()->add(&p);
        QVERIFY(s.open(capture("a.png")));
        const QVariantMap row = s.providers().first().toMap();
        QCOMPARE(row.value("id").toString(), QStringLiteral("discord.webhook"));
        QCOMPARE(row.value("available").toBool(), false);
        QCOMPARE(row.value("access").toString(), QStringLiteral("share_token"));
        QVERIFY(!s.requestTargets("discord.webhook"));
    }

    void settingsCanAddAndRemoveDestinationsThroughTheService()
    {
        FakeDiscord server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        DiscordWebhookStore store = makeStore();
        DiscordWebhookProvider p(&store);
        Service s;
        s.registry()->add(&p);

        QCOMPARE(s.addSavedDestination("discord.webhook", "Friends", "nope"),
                 QStringLiteral("invalid_secret"));
        QCOMPARE(s.addSavedDestination("discord.webhook", "Friends", webhookUrl(server)), QString());
        const QVariantList providers = s.savedDestinationProviders();
        QCOMPARE(providers.size(), 1);
        const QVariantList dests = providers.first().toMap().value("destinations").toList();
        QCOMPARE(dests.size(), 1);
        QCOMPARE(dests.first().toMap().value("name").toString(), QStringLiteral("Friends"));
        // The secret is never handed back to QML.
        QVERIFY(!QString::fromUtf8(QJsonDocument::fromVariant(providers).toJson()).contains(m_token));
        QVERIFY(s.removeSavedDestination("discord.webhook", dests.first().toMap().value("id").toString()));
        QVERIFY(p.savedDestinations().isEmpty());
        QCOMPARE(s.addSavedDestination("telegram.desktop", "x", "y"), QStringLiteral("unknown_provider"));
    }

    void uploadsTheExactCaptureAndReportsSentFromTheResponse_data()
    {
        QTest::addColumn<QString>("fileName");
        QTest::addColumn<QByteArray>("content");
        QTest::newRow("screenshot") << QStringLiteral("2026-09-29_14-00-00.png") << QByteArray("PNGDATA\x01\x02");
        QTest::newRow("clip") << QStringLiteral("clip with spaces.mp4") << QByteArray(200000, 'v');
    }

    void uploadsTheExactCaptureAndReportsSentFromTheResponse()
    {
        QFETCH(QString, fileName);
        QFETCH(QByteArray, content);
        g_log.clear();
        FakeDiscord server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        DiscordWebhookStore store = makeStore();
        QVERIFY(store.add("Clips", webhookUrl(server)).isEmpty());
        DiscordWebhookProvider p(&store);
        Service s;
        s.registry()->add(&p);
        QVERIFY(s.open(capture(fileName, content)));
        QVERIFY(s.requestTargets("discord.webhook"));
        QCOMPARE(s.targets().size(), 1);
        const QString targetId = s.targets().first().toMap().value("id").toString();

        QSignalSpy done(&s, &Service::finished);
        QVERIFY(!s.share("discord.webhook", targetId).isEmpty());
        QVERIFY(done.wait(10000));

        QCOMPARE(s.lastResult().value("outcome").toString(), QStringLiteral("sent"));
        QVERIFY(server.requestHead.startsWith("POST /api/webhooks/123456789012345678/"));
        QVERIFY(server.requestHead.contains("?wait=true"));
        QVERIFY(server.requestHead.toLower().contains("multipart/form-data"));
        QVERIFY(server.requestBody.contains("name=\"payload_json\""));
        QVERIFY(server.requestBody.contains("\"parse\":[]"));   // no mentions
        QVERIFY(server.requestBody.contains("name=\"files[0]\"; filename=\"" + fileName.toUtf8() + "\""));
        QVERIFY(server.requestBody.contains(content));          // the exact bytes
        QCOMPARE(server.requests, 1);
        QVERIFY(store.list().first().lastUsedMs > 0);           // recent-first ordering

        // The credential never reaches a log line or the result.
        for (const QString& line : g_log)
            QVERIFY2(!line.contains(m_token) && !line.contains("127.0.0.1:"), qPrintable(line));
        QVERIFY(!QString::fromUtf8(QJsonDocument::fromVariant(s.lastResult()).toJson()).contains(m_token));
    }

    void honestOutcomeForEveryServerAnswer_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<QString>("outcome");
        QTest::addColumn<QString>("code");
        QTest::newRow("ok but no message id") << 200 << QByteArray("{}") << "unconfirmed" << "unexpected_response";
        QTest::newRow("204 no content") << 204 << QByteArray() << "unconfirmed" << "unexpected_response";
        QTest::newRow("deleted webhook") << 404 << QByteArray(R"({"code":10015})") << "failed" << "webhook_revoked";
        QTest::newRow("bad token") << 401 << QByteArray(R"({"code":50027})") << "failed" << "webhook_revoked";
        QTest::newRow("rate limited") << 429 << QByteArray(R"({"retry_after":3})") << "failed" << "rate_limited";
        QTest::newRow("payload too large") << 413 << QByteArray() << "failed" << "too_large";
        QTest::newRow("discord too large code") << 400 << QByteArray(R"({"code":40005})") << "failed" << "too_large";
        QTest::newRow("bad request") << 400 << QByteArray(R"({"message":"x"})") << "failed" << "rejected";
        QTest::newRow("server error") << 502 << QByteArray("<html>") << "unconfirmed" << "server_error";
    }

    void honestOutcomeForEveryServerAnswer()
    {
        QFETCH(int, status);
        QFETCH(QByteArray, body);
        QFETCH(QString, outcome);
        QFETCH(QString, code);
        FakeDiscord server;
        server.status = status;
        server.body = body;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        DiscordWebhookStore store = makeStore();
        QVERIFY(store.add("Clips", webhookUrl(server)).isEmpty());
        DiscordWebhookProvider p(&store);
        Service s;
        s.registry()->add(&p);
        QVERIFY(s.open(capture("b.png")));
        QVERIFY(s.requestTargets("discord.webhook"));
        QSignalSpy done(&s, &Service::finished);
        QVERIFY(!s.share("discord.webhook", s.targets().first().toMap().value("id").toString()).isEmpty());
        QVERIFY(done.wait(10000));
        QCOMPARE(s.lastResult().value("outcome").toString(), outcome);
        QCOMPARE(s.lastResult().value("errorCode").toString(), code);
        QCOMPARE(server.requests, 1);   // never retried
        QVERIFY(store.list().first().lastUsedMs == 0);   // only a real send counts as use
    }

    void aDroppedConnectionAfterTheUploadIsUnconfirmed()
    {
        FakeDiscord server;
        server.dropAfterHeaders = true;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        DiscordWebhookStore store = makeStore();
        QVERIFY(store.add("Clips", webhookUrl(server)).isEmpty());
        DiscordWebhookProvider p(&store);
        Service s;
        s.registry()->add(&p);
        QVERIFY(s.open(capture("c.png")));
        QVERIFY(s.requestTargets("discord.webhook"));
        QSignalSpy done(&s, &Service::finished);
        QVERIFY(!s.share("discord.webhook", s.targets().first().toMap().value("id").toString()).isEmpty());
        QVERIFY(done.wait(10000));
        QCOMPARE(s.lastResult().value("outcome").toString(), QStringLiteral("unconfirmed"));
        QCOMPARE(server.requests, 1);
    }

    void aRefusedConnectionIsAPlainFailure()
    {
        DiscordWebhookStore store = makeStore();
        // A closed port: listen, take the number, close.
        int port = 0;
        {
            QTcpServer probe;
            QVERIFY(probe.listen(QHostAddress::LocalHost));
            port = probe.serverPort();
        }
        QVERIFY(store.add("Gone", QStringLiteral("http://127.0.0.1:%1/api/webhooks/123456789012345678/%2")
                                      .arg(port).arg(m_token)).isEmpty());
        DiscordWebhookProvider p(&store);
        Service s;
        s.registry()->add(&p);
        QVERIFY(s.open(capture("d.png")));
        QVERIFY(s.requestTargets("discord.webhook"));
        QSignalSpy done(&s, &Service::finished);
        QVERIFY(!s.share("discord.webhook", s.targets().first().toMap().value("id").toString()).isEmpty());
        QVERIFY(done.wait(10000));
        QCOMPARE(s.lastResult().value("outcome").toString(), QStringLiteral("failed"));
        QCOMPARE(s.lastResult().value("errorCode").toString(), QStringLiteral("network_error"));
    }

    void oversizedMediaIsRefusedBeforeAnyUpload()
    {
        FakeDiscord server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        DiscordWebhookStore store = makeStore();
        QVERIFY(store.add("Clips", webhookUrl(server)).isEmpty());
        DiscordWebhookProvider p(&store);
        p.setMaxUploadBytes(10);
        Service s;
        s.registry()->add(&p);
        QVERIFY(s.open(capture("e.mp4", QByteArray(64, 'x'))));
        QVERIFY(s.requestTargets("discord.webhook"));
        QSignalSpy done(&s, &Service::finished);
        s.share("discord.webhook", s.targets().first().toMap().value("id").toString());
        if (done.isEmpty())
            QVERIFY(done.wait(5000));
        QCOMPARE(s.lastResult().value("outcome").toString(), QStringLiteral("failed"));
        QCOMPARE(s.lastResult().value("errorCode").toString(), QStringLiteral("too_large"));
        QCOMPARE(server.requests, 0);
    }

    void cancellingAfterTheWholeUploadIsUnconfirmedNotCancelled()
    {
        FakeDiscord server;
        server.hang = true;   // reads everything, never answers
        QVERIFY(server.listen(QHostAddress::LocalHost));
        DiscordWebhookStore store = makeStore();
        QVERIFY(store.add("Clips", webhookUrl(server)).isEmpty());
        DiscordWebhookProvider p(&store);
        Service s;
        s.registry()->add(&p);
        QVERIFY(s.open(capture("f.png")));
        QVERIFY(s.requestTargets("discord.webhook"));
        QSignalSpy done(&s, &Service::finished);
        QVERIFY(!s.share("discord.webhook", s.targets().first().toMap().value("id").toString()).isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(server.requests == 1, 5000);
        QTest::qWait(200);   // let the upload progress reach the provider
        s.cancel();
        QVERIFY(done.count() > 0 || done.wait(5000));
        // The server got everything; it may still have posted the message.
        QCOMPARE(s.lastResult().value("outcome").toString(), QStringLiteral("unconfirmed"));
    }

    void disconnectRemovesEveryDestinationAndSecret()
    {
        FakeDiscord server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        DiscordWebhookStore store = makeStore();
        QString id;
        QVERIFY(store.add("A", webhookUrl(server), &id).isEmpty());
        QVERIFY(store.add("B", webhookUrl(server)).isEmpty());
        DiscordWebhookProvider p(&store);
        Service s;
        s.registry()->add(&p);
        QVERIFY(s.disconnectProvider("discord.webhook"));
        QVERIFY(store.list().isEmpty());
        QVERIFY(store.webhookUrl(id).isEmpty());
        QVERIFY(SecretStore(m_namespace).names("discord.webhook").isEmpty());
    }
};

QTEST_MAIN(TestShareDiscordWebhook)
#include "tst_sharediscordwebhook.moc"
