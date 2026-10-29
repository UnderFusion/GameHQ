#include "updates/UpdateService.h"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

// End to end from an HTTP answer to the state the update banner and About bind
// to. 0.7.9 and 0.8.0 left an available update hidden for up to a day, and a
// 304 on an hourly re-check used to clear the offer it was confirming.
namespace
{
struct Response
{
    int status = 200;
    QByteArray reason = "OK";
    QList<QPair<QByteArray, QByteArray>> headers;
    QByteArray body;
    int delayMs = 0;
};

// Serves queued responses in order and records each request's headers.
class FixtureServer : public QTcpServer
{
public:
    QList<Response> queue;
    QList<QByteArray> requests;

    FixtureServer()
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    QByteArray &buffer = m_buffers[socket];
                    buffer += socket->readAll();
                    if (!buffer.contains("\r\n\r\n"))
                        return;
                    requests.append(buffer);
                    const Response r = queue.isEmpty() ? Response{500, "Empty", {}, {}}
                                                       : queue.takeFirst();
                    QByteArray out = "HTTP/1.1 " + QByteArray::number(r.status) + ' ' + r.reason + "\r\n";
                    for (const auto &h : r.headers)
                        out += h.first + ": " + h.second + "\r\n";
                    out += "Content-Length: " + QByteArray::number(r.body.size())
                        + "\r\nConnection: close\r\n\r\n" + r.body;
                    QTimer::singleShot(r.delayMs, socket, [socket, out] {
                        socket->write(out);
                        socket->disconnectFromHost();
                    });
                });
                connect(socket, &QTcpSocket::disconnected, socket, [this, socket] {
                    m_buffers.remove(socket);
                    socket->deleteLater();
                });
            }
        });
    }

    QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(serverPort()); }

private:
    QHash<QTcpSocket *, QByteArray> m_buffers;
};

QJsonObject release(const QString &tag, bool prerelease = false)
{
    const QString version = tag.mid(1).section(QLatin1Char('-'), 0, 0);
    const QString zip = QStringLiteral("GameHQ-%1-win64-update.zip").arg(version);
    QJsonArray assets;
    for (const QString &name : {zip, zip + QStringLiteral(".sha256"),
                                QStringLiteral("gamehq-release.json"),
                                QStringLiteral("gamehq-release.sig")}) {
        assets.append(QJsonObject{{QStringLiteral("name"), name},
                                  {QStringLiteral("browser_download_url"),
                                   QStringLiteral("http://127.0.0.1:1/") + name},
                                  {QStringLiteral("size"), 1024}});
    }
    return {{QStringLiteral("tag_name"), tag},
            {QStringLiteral("name"), QStringLiteral("GameHQ ") + tag.mid(1)},
            {QStringLiteral("draft"), false},
            {QStringLiteral("prerelease"), prerelease},
            {QStringLiteral("html_url"), QStringLiteral("http://127.0.0.1:1/") + tag},
            {QStringLiteral("published_at"), QStringLiteral("2026-10-26T12:00:00Z")},
            {QStringLiteral("assets"), assets}};
}

Response releases(const QJsonArray &list, const QByteArray &etag)
{
    return {200, "OK", {{"ETag", etag}, {"Content-Type", "application/json"}},
            QJsonDocument(list).toJson(QJsonDocument::Compact)};
}

QJsonArray publishedWith(const QJsonObject &latest)
{
    // The real page also carries betas and the Playnite plugin's releases.
    return {latest, release(QStringLiteral("v0.8.0-beta4"), true),
            QJsonObject{{QStringLiteral("tag_name"), QStringLiteral("playnite-v0.4.13")},
                        {QStringLiteral("prerelease"), true}},
            release(QStringLiteral("v0.7.9"))};
}

constexpr int kRequestGuardMs = 5100; // UpdateService refuses re-checks within 5 s
}

class TestUpdateDiscovery : public QObject
{
    Q_OBJECT

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<FixtureServer> m_server;

    std::unique_ptr<UpdateService> service(const QString &installed)
    {
        auto s = std::make_unique<UpdateService>(
            QStringLiteral("fixture-owner"), QStringLiteral("fixture-repo"), installed,
            m_dir->filePath(QStringLiteral("app/.update/staging")),
            m_dir->filePath(QStringLiteral("data/update-trust.json")));
        s->setReleaseApiBase(m_server->base());
        return s;
    }

    static bool settle(UpdateService &s)
    {
        return QTest::qWaitFor([&s] { return s.state() != UpdateService::State::Checking; }, 5000);
    }

private slots:
    void init()
    {
        m_dir = std::make_unique<QTemporaryDir>();
        QVERIFY(m_dir->isValid());
        m_server = std::make_unique<FixtureServer>();
        QVERIFY(m_server->listen(QHostAddress::LocalHost));
    }

    void cleanup()
    {
        m_server.reset();
        m_dir.reset();
    }

    void aNewStableIsOfferedToEarlierInstalls_data()
    {
        QTest::addColumn<QString>("installed");
        QTest::addColumn<bool>("offered");
        QTest::newRow("0.7.9") << QStringLiteral("0.7.9") << true;
        QTest::newRow("0.8.0") << QStringLiteral("0.8.0") << true;
        QTest::newRow("0.8.0-beta4") << QStringLiteral("0.8.0-beta4") << true;
        QTest::newRow("0.8.1") << QStringLiteral("0.8.1") << false;
    }

    void aNewStableIsOfferedToEarlierInstalls()
    {
        QFETCH(QString, installed);
        QFETCH(bool, offered);
        m_server->queue.append(releases(publishedWith(release(QStringLiteral("v0.8.1"))), "\"e1\""));
        auto s = service(installed);
        s->checkNow();
        QVERIFY(settle(*s));
        QCOMPARE(m_server->requests.size(), 1);
        if (offered) {
            QCOMPARE(s->stateName(), QStringLiteral("UpdateAvailable"));
            QCOMPARE(s->latestVersion(), QStringLiteral("0.8.1"));
        } else {
            QCOMPARE(s->stateName(), QStringLiteral("UpToDate"));
            QVERIFY(s->latestVersion().isEmpty());
        }
        QVERIFY(s->lastChecked().isValid());
        QVERIFY(!s->busy());
    }

    void notModifiedKeepsTheOffer()
    {
        m_server->queue.append(releases(publishedWith(release(QStringLiteral("v0.8.1"))), "\"e1\""));
        m_server->queue.append({304, "Not Modified", {{"ETag", "\"e1\""}}, {}});
        auto s = service(QStringLiteral("0.7.9"));
        s->checkNow();
        QVERIFY(settle(*s));
        QCOMPARE(s->stateName(), QStringLiteral("UpdateAvailable"));

        QTest::qWait(kRequestGuardMs);
        s->checkNow();
        QVERIFY(settle(*s));
        QCOMPARE(m_server->requests.size(), 2);
        QVERIFY(m_server->requests.at(1).toLower().contains("if-none-match: \"e1\""));
        QCOMPARE(s->stateName(), QStringLiteral("UpdateAvailable"));
        QCOMPARE(s->latestVersion(), QStringLiteral("0.8.1"));
    }

    void aReleasePublishedAfterThePreviousCheckIsFound()
    {
        m_server->queue.append(releases(publishedWith(release(QStringLiteral("v0.8.0"))), "\"e1\""));
        m_server->queue.append(releases(publishedWith(release(QStringLiteral("v0.8.1"))), "\"e2\""));
        auto s = service(QStringLiteral("0.7.9"));
        s->checkNow();
        QVERIFY(settle(*s));
        QCOMPARE(s->latestVersion(), QStringLiteral("0.8.0"));
        // Skipping 0.8.0 must not hide the release that follows it.
        s->skipVersion();
        QCOMPARE(s->stateName(), QStringLiteral("UpToDate"));

        QTest::qWait(kRequestGuardMs);
        s->checkNow();
        QVERIFY(settle(*s));
        QCOMPARE(s->stateName(), QStringLiteral("UpdateAvailable"));
        QCOMPARE(s->latestVersion(), QStringLiteral("0.8.1"));
    }

    void aRateLimitKeepsTheOfferAndStopsFurtherRequests()
    {
        m_server->queue.append(releases(publishedWith(release(QStringLiteral("v0.8.1"))), "\"e1\""));
        const QByteArray reset = QByteArray::number(QDateTime::currentSecsSinceEpoch() + 600);
        m_server->queue.append({403, "Forbidden",
                                {{"x-ratelimit-remaining", "0"}, {"x-ratelimit-reset", reset}}, {}});
        auto s = service(QStringLiteral("0.8.0"));
        s->checkNow();
        QVERIFY(settle(*s));

        QTest::qWait(kRequestGuardMs);
        s->checkNow();
        QVERIFY(settle(*s));
        QCOMPARE(s->stateName(), QStringLiteral("UpdateAvailable"));
        QCOMPARE(s->latestVersion(), QStringLiteral("0.8.1"));
        QVERIFY(s->nextAllowedCheck().isValid());

        // Inside the cooldown nothing reaches the network.
        QTest::qWait(kRequestGuardMs);
        s->checkNow();
        QCOMPARE(m_server->requests.size(), 2);
        QVERIFY(!s->errorText().isEmpty());
        QCOMPARE(s->stateName(), QStringLiteral("UpdateAvailable"));
    }

    void aFailedCheckIsNeverReportedAsUpToDate()
    {
        m_server->queue.append({502, "Bad Gateway", {}, {}});
        auto s = service(QStringLiteral("0.7.9"));
        s->checkNow();
        QVERIFY(settle(*s));
        QCOMPARE(s->stateName(), QStringLiteral("Failed"));
        QVERIFY(s->failedDuringCheck());
        // No completed check is recorded, so the next automatic wake retries.
        QVERIFY(!s->lastChecked().isValid());
        QVERIFY(!s->nextAllowedCheck().isValid());
    }

    void overlappingChecksSendOneRequest()
    {
        m_server->queue.append(releases(publishedWith(release(QStringLiteral("v0.8.1"))), "\"e1\""));
        auto s = service(QStringLiteral("0.7.9"));
        s->checkNow();
        QVERIFY(s->busy());
        s->checkNow();
        QVERIFY(settle(*s));
        s->checkNow();   // immediately after: refused by the request guard
        QTest::qWait(200);
        QCOMPARE(m_server->requests.size(), 1);
        QCOMPARE(s->stateName(), QStringLiteral("UpdateAvailable"));
    }

    void publishedStableFeedOffersTheHotfix()
    {
        // Release acceptance replays the actual downloaded GitHub response
        // through UpdateService without contacting GitHub from a native test.
        const QString path = qEnvironmentVariable("GAMEHQ_PUBLIC_RELEASES_FIXTURE");
        if (path.isEmpty())
            QSKIP("Set GAMEHQ_PUBLIC_RELEASES_FIXTURE for public release acceptance");
        const QString expected = qEnvironmentVariable("GAMEHQ_EXPECTED_STABLE_VERSION");
        QVERIFY(!expected.isEmpty());
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray body = file.readAll();
        QVERIFY(QJsonDocument::fromJson(body).isArray());
        for (const QString &installed : {QStringLiteral("0.8.0"),
                                         QStringLiteral("0.8.1-beta1"), expected}) {
            m_server->queue.append({200, "OK", {{"Content-Type", "application/json"}}, body});
            auto s = service(installed);
            s->checkNow();
            QVERIFY(settle(*s));
            if (installed == expected) {
                QCOMPARE(s->stateName(), QStringLiteral("UpToDate"));
            } else {
                QCOMPARE(s->stateName(), QStringLiteral("UpdateAvailable"));
                QCOMPARE(s->latestVersion(), expected);
            }
        }
    }

    void optingInRefreshesAfterTheRequestGuard()
    {
        const QJsonArray page{release(QStringLiteral("v0.8.1-beta1"), true),
                              release(QStringLiteral("v0.8.0"))};
        m_server->queue.append(releases(page, "\"stable\""));
        auto s = service(QStringLiteral("0.8.0"));
        s->checkNow();
        QVERIFY(settle(*s));
        QCOMPARE(s->stateName(), QStringLiteral("UpToDate"));
        m_server->queue.append(releases(page, "\"beta\""));
        s->setIncludeBetas(true, true);
        s->setIncludeBetas(true, true); // persisted notifications do not duplicate requests
        QCOMPARE(m_server->requests.size(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(m_server->requests.size(), 2, 7000);
        QTRY_COMPARE(s->stateName(), QStringLiteral("UpdateAvailable"));
        QCOMPARE(s->latestVersion(), QStringLiteral("0.8.1-beta1"));
        QVERIFY(!m_server->requests.last().contains("If-None-Match"));
    }

    void optingOutRechecksWithoutDowngrading_data()
    {
        QTest::addColumn<QString>("installed");
        QTest::addColumn<QString>("stable");
        QTest::addColumn<bool>("offered");
        QTest::newRow("installed-beta-older-stable") << QStringLiteral("0.8.1-beta1")
            << QStringLiteral("v0.8.0") << false;
        QTest::newRow("installed-beta-matching-stable") << QStringLiteral("0.8.1-beta1")
            << QStringLiteral("v0.8.1") << true;
        QTest::newRow("installed-stable") << QStringLiteral("0.8.0")
            << QStringLiteral("v0.8.1") << true;
    }

    void optingOutRechecksWithoutDowngrading()
    {
        QFETCH(QString, installed);
        QFETCH(QString, stable);
        QFETCH(bool, offered);
        m_server->queue.append(releases({release(QStringLiteral("v0.8.2-beta1"), true),
                                          release(stable)}, "\"beta\""));
        auto s = service(installed);
        s->setIncludeBetas(true);
        s->checkNow();
        QVERIFY(settle(*s));
        QCOMPARE(s->latestVersion(), QStringLiteral("0.8.2-beta1"));
        m_server->queue.append(releases({release(QStringLiteral("v0.8.2-beta1"), true),
                                          release(stable)}, "\"stable\""));
        s->setIncludeBetas(false, true);
        QVERIFY(s->latestVersion().isEmpty()); // stale beta offer disappears immediately
        QTRY_COMPARE_WITH_TIMEOUT(m_server->requests.size(), 2, 7000);
        QTRY_VERIFY(s->state() != UpdateService::State::Checking);
        QCOMPARE(s->stateName(), offered ? QStringLiteral("UpdateAvailable") : QStringLiteral("UpToDate"));
        QCOMPARE(s->latestVersion(), offered ? stable.mid(1) : QString());
        QCOMPARE(s->waitingForStable(), !offered);
        QVERIFY(!m_server->requests.last().contains("If-None-Match"));
    }

    void switchingDuringDiscoveryCoalescesAndDiscardsTheOldAnswer()
    {
        Response old = releases({release(QStringLiteral("v0.8.2-beta1"), true)}, "\"old\"");
        old.delayMs = 200;
        m_server->queue.append(old);
        auto s = service(QStringLiteral("0.8.0"));
        s->setIncludeBetas(true);
        s->checkNow();
        QTRY_COMPARE(m_server->requests.size(), 1);
        m_server->queue.append(releases({release(QStringLiteral("v0.8.1"))}, "\"final\""));
        s->setIncludeBetas(false, true);
        s->setIncludeBetas(true, true);
        s->setIncludeBetas(false, true);
        QTest::qWait(300);
        QVERIFY(s->latestVersion().isEmpty());
        QVERIFY(s->cachedEtag().isEmpty());
        QCOMPARE(m_server->requests.size(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(m_server->requests.size(), 2, 7000);
        QTRY_COMPARE(s->latestVersion(), QStringLiteral("0.8.1"));
        QTest::qWait(100);
        QCOMPARE(m_server->requests.size(), 2);
    }

    void channelChangeRespectsGitHubCooldown()
    {
        auto s = service(QStringLiteral("0.8.0"));
        s->primeNextAllowedCheck(QDateTime::currentDateTimeUtc().addMSecs(1200));
        m_server->queue.append(releases({release(QStringLiteral("v0.8.1-beta1"), true)}, "\"b\""));
        s->setIncludeBetas(true, true);
        QTest::qWait(150);
        QCOMPARE(m_server->requests.size(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(m_server->requests.size(), 1, 3000);
        QTRY_COMPARE(s->latestVersion(), QStringLiteral("0.8.1-beta1"));
    }
};

QTEST_GUILESS_MAIN(TestUpdateDiscovery)
#include "tst_updatediscovery.moc"
