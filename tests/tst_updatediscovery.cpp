#include "updates/UpdateService.h"
#include "updates/ReleaseCatalog.h"

#include <QDateTime>
#include <QDir>
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

QJsonObject release(const QString &tag, bool prerelease = false,
                    const QString &assetBase = QStringLiteral("http://127.0.0.1:1"))
{
    const QString version = tag.mid(1).section(QLatin1Char('-'), 0, 0);
    const QString zip = QStringLiteral("GameHQ-%1-win64-update.zip").arg(version);
    QJsonArray assets;
    for (const QString &name : {zip, zip + QStringLiteral(".sha256"),
                                QStringLiteral("gamehq-release.json"),
                                QStringLiteral("gamehq-release.sig")}) {
        assets.append(QJsonObject{{QStringLiteral("name"), name},
                                  {QStringLiteral("browser_download_url"),
                                   assetBase + QLatin1Char('/') + name},
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

    // ---- Command/state guards (A03) -------------------------------------
    // A real UpdateService driven into every operation state. Downloading uses
    // the real downloader against an HTTPS origin that accepts and never
    // answers; later states start from the test seam's verified package.
    std::unique_ptr<QTcpServer> m_silent;

    QString silentHttpsBase()
    {
        if (!m_silent) {
            m_silent = std::make_unique<QTcpServer>();
            if (!m_silent->listen(QHostAddress::LocalHost))
                return {};
            QObject::connect(m_silent.get(), &QTcpServer::newConnection, m_silent.get(), [this] {
                while (QTcpSocket *socket = m_silent->nextPendingConnection())
                    socket->setParent(m_silent.get());
            });
        }
        return QStringLiteral("https://127.0.0.1:%1").arg(m_silent->serverPort());
    }

    // Enough of a packaged install for UpdatePreflight to accept it.
    bool fakePackagedInstall()
    {
        const QString root = m_dir->filePath(QStringLiteral("app"));
        for (const QString &rel : {QStringLiteral("GameHQ.exe"), QStringLiteral("app/GameHQ.exe"),
                                   QStringLiteral("GameHQUpdater.exe")}) {
            const QString path = QDir(root).filePath(rel);
            QDir().mkpath(QFileInfo(path).absolutePath());
            QFile f(path);
            if (!f.open(QIODevice::WriteOnly) || f.write("MZ") != 2)
                return false;
        }
        return true;
    }

    // An offer restored from the discovery cache: no request and no 5 s request
    // guard, so every later checkNow() is a real attempt.
    std::unique_ptr<UpdateService> offering(const QJsonObject &rel, bool betas = false)
    {
        const auto info = ReleaseCatalog::selectBest(QJsonArray{rel}, betas);
        if (!info)
            return {};
        const QString cache = m_dir->filePath(QStringLiteral("data/release-notes-cache/discovery.json"));
        QDir().mkpath(QFileInfo(cache).absolutePath());
        QFile f(cache);
        const QByteArray bytes = ReleaseCatalog::cacheSnapshot(*info);
        if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size())
            return {};
        f.close();
        auto s = service(QStringLiteral("0.7.9"));
        s->setIncludeBetas(betas);
        s->restoreCachedRelease();
        return s;
    }

    static VerifiedUpdate fakeVerified(const QString &version)
    {
        VerifiedUpdate v;
        v.packagePath = QStringLiteral("staged.zip");
        v.packageSha256 = QByteArray(32, char(0x11));
        v.version = version;
        v.manifestPath = QStringLiteral("gamehq-release.json");
        v.signaturePath = QStringLiteral("gamehq-release.sig");
        v.manifestSha256 = QString(64, QLatin1Char('a'));
        v.signature = QStringLiteral("c2ln");
        v.keyId = QStringLiteral("gamehq-test-2026-01");
        v.releaseSequence = 7;
        v.artifactName = QStringLiteral("GameHQ-%1-win64-update.zip").arg(version);
        v.artifactSize = 1024;
        v.artifactSha256 = QString(64, QLatin1Char('b'));
        return v;
    }

    // Every discovery-side command a user, the channel setting or the cooldown
    // can issue. None of them may move an active operation or send a request.
    void probeDiscoveryCommands(UpdateService &s, UpdateService::State expected)
    {
        const qsizetype before = m_server->requests.size();
        QSignalSpy skipped(&s, &UpdateService::versionSkipped);
        s.checkNow();
        QCOMPARE(s.state(), expected);
        s.skipVersion();
        QCOMPARE(s.state(), expected);
        QCOMPARE(skipped.size(), 0);
        s.downloadUpdate();
        QCOMPARE(s.state(), expected);
        s.setIncludeBetas(!s.includeBetas(), true);
        s.setIncludeBetas(!s.includeBetas(), true);
        QCOMPARE(s.state(), expected);
        // Inside a GitHub cooldown checkNow() used to fall back to the offer.
        const QDateTime cooldown = s.nextAllowedCheck();
        s.primeNextAllowedCheck(QDateTime::currentDateTimeUtc().addSecs(600));
        s.checkNow();
        QCOMPARE(s.state(), expected);
        s.primeNextAllowedCheck(cooldown);
        QTest::qWait(150);   // a deferred channel check must not fire either
        QCOMPARE(s.state(), expected);
        QCOMPARE(m_server->requests.size(), before);
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
        m_silent.reset();
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

    void discoveryCannotInterruptADownload()
    {
        QVERIFY(fakePackagedInstall());
        auto s = offering(release(QStringLiteral("v0.8.1"), false, silentHttpsBase()));
        QVERIFY(s);
        QCOMPARE(s->stateName(), QStringLiteral("UpdateAvailable"));
        s->downloadUpdate();
        QCOMPARE(s->stateName(), QStringLiteral("Downloading"));
        probeDiscoveryCommands(*s, UpdateService::State::Downloading);
        if (QTest::currentTestFailed())
            return;
        // The one-click upgrade still applies to the running download.
        s->downloadAndInstall();
        QCOMPARE(s->stateName(), QStringLiteral("Downloading"));
        s->cancelDownload();
        QCOMPARE(s->stateName(), QStringLiteral("UpdateAvailable"));
        s->cancelDownload();   // nothing to cancel any more
        QCOMPARE(s->stateName(), QStringLiteral("UpdateAvailable"));
    }

    void discoveryCannotDiscardAVerifiedPackage()
    {
        QVERIFY(fakePackagedInstall());
        auto s = offering(release(QStringLiteral("v0.8.1")));
        QVERIFY(s);
        s->deliverVerifiedDownloadForTesting(fakeVerified(QStringLiteral("0.8.1")));
        QCOMPARE(s->stateName(), QStringLiteral("ReadyToInstall"));
        probeDiscoveryCommands(*s, UpdateService::State::ReadyToInstall);
        if (QTest::currentTestFailed())
            return;
        s->cancelDownload();
        QCOMPARE(s->stateName(), QStringLiteral("ReadyToInstall"));
        // The install command itself stays available.
        QSignalSpy prepare(s.get(), &UpdateService::prepareForUpdateRequested);
        s->installAndRestart();
        QCOMPARE(s->stateName(), QStringLiteral("PreparingForUpdate"));
        QCOMPARE(prepare.size(), 1);
    }

    void discoveryCannotInterruptPreparationOrInstallation()
    {
        QVERIFY(fakePackagedInstall());
        auto s = offering(release(QStringLiteral("v0.8.1")));
        QVERIFY(s);
        QSignalSpy approved(s.get(), &UpdateService::installApproved);
        s->deliverVerifiedDownloadForTesting(fakeVerified(QStringLiteral("0.8.1")));
        s->installAndRestart();
        QCOMPARE(s->stateName(), QStringLiteral("PreparingForUpdate"));
        probeDiscoveryCommands(*s, UpdateService::State::PreparingForUpdate);
        if (QTest::currentTestFailed())
            return;
        s->downloadAndInstall();
        QCOMPARE(s->stateName(), QStringLiteral("PreparingForUpdate"));

        Response fresh = releases({release(QStringLiteral("v0.8.1"))}, "\"fresh\"");
        fresh.delayMs = 400;
        m_server->queue.append(fresh);
        s->markQuiescent();
        QCOMPARE(s->stateName(), QStringLiteral("Quiescent"));
        QTRY_COMPARE(m_server->requests.size(), 1);
        // Revalidation is in flight: nothing may start a second request.
        s->checkNow();
        s->skipVersion();
        s->downloadUpdate();
        QCOMPARE(s->stateName(), QStringLiteral("Quiescent"));
        QTRY_COMPARE_WITH_TIMEOUT(approved.size(), 1, 5000);
        QCOMPARE(m_server->requests.size(), 1);

        // Approved but not yet handed off, then handed off.
        probeDiscoveryCommands(*s, UpdateService::State::Quiescent);
        if (QTest::currentTestFailed())
            return;
        s->markInstalling();
        QCOMPARE(s->stateName(), QStringLiteral("Installing"));
        probeDiscoveryCommands(*s, UpdateService::State::Installing);
        if (QTest::currentTestFailed())
            return;
        s->cancelDownload();
        s->installAndRestart();
        QCOMPARE(s->stateName(), QStringLiteral("Installing"));
        QCOMPARE(approved.size(), 1);
    }

    void aChannelChangeDuringRevalidationKeepsTheAcceptedRelease()
    {
        // Installing a beta, the user turns betas off while the final check is
        // on the wire. The download was accepted on the beta channel and must
        // be revalidated on it, not reported as withdrawn.
        QVERIFY(fakePackagedInstall());
        const QJsonObject beta = release(QStringLiteral("v0.8.1-beta1"), true);
        auto s = offering(beta, true);
        QVERIFY(s);
        QCOMPARE(s->latestVersion(), QStringLiteral("0.8.1-beta1"));
        QSignalSpy approved(s.get(), &UpdateService::installApproved);
        s->deliverVerifiedDownloadForTesting(fakeVerified(QStringLiteral("0.8.1")));
        s->installAndRestart();
        Response fresh = releases({beta, release(QStringLiteral("v0.7.9"))}, "\"fresh\"");
        fresh.delayMs = 300;
        m_server->queue.append(fresh);
        s->markQuiescent();
        QTRY_COMPARE(m_server->requests.size(), 1);
        s->setIncludeBetas(false, true);
        QTRY_COMPARE_WITH_TIMEOUT(approved.size(), 1, 5000);
        QCOMPARE(s->stateName(), QStringLiteral("Quiescent"));
        QVERIFY(s->errorText().isEmpty());
    }

    void aCancelledPreparationIgnoresTheLateRevalidation()
    {
        QVERIFY(fakePackagedInstall());
        auto s = offering(release(QStringLiteral("v0.8.1")));
        QVERIFY(s);
        QSignalSpy approved(s.get(), &UpdateService::installApproved);
        s->deliverVerifiedDownloadForTesting(fakeVerified(QStringLiteral("0.8.1")));
        s->installAndRestart();
        Response fresh = releases({release(QStringLiteral("v0.8.1"))}, "\"fresh\"");
        fresh.delayMs = 300;
        m_server->queue.append(fresh);
        s->markQuiescent();
        QTRY_COMPARE(m_server->requests.size(), 1);
        s->cancelPreparation(QStringLiteral("capture could not stop"));
        QCOMPARE(s->stateName(), QStringLiteral("ReadyToInstall"));
        QTest::qWait(700);
        QCOMPARE(approved.size(), 0);   // never install after a cancellation
        QCOMPARE(s->stateName(), QStringLiteral("ReadyToInstall"));
        QCOMPARE(s->errorText(), QStringLiteral("capture could not stop"));
        QCOMPARE(m_server->requests.size(), 1);
        // A fresh attempt runs its own revalidation.
        m_server->queue.append(releases({release(QStringLiteral("v0.8.1"))}, "\"again\""));
        s->installAndRestart();
        s->markQuiescent();
        QTRY_COMPARE_WITH_TIMEOUT(approved.size(), 1, 5000);
        QCOMPARE(m_server->requests.size(), 2);
    }

    void idleStatesStillAcceptDiscoveryAndRetry()
    {
        // Guards must not lock out the legitimate paths: a failed download is
        // retried, an idle service checked and an offer skipped.
        QVERIFY(fakePackagedInstall());
        auto s = offering(release(QStringLiteral("v0.8.1")));
        QVERIFY(s);
        s->downloadUpdate();   // http asset URLs fail validation at once
        QCOMPARE(s->stateName(), QStringLiteral("Failed"));
        QVERIFY(!s->failedDuringCheck());
        s->downloadUpdate();
        QCOMPARE(s->stateName(), QStringLiteral("Failed"));
        m_server->queue.append(releases({release(QStringLiteral("v0.8.1"))}, "\"e\""));
        s->checkNow();
        QVERIFY(settle(*s));
        QCOMPARE(m_server->requests.size(), 1);
        QCOMPARE(s->stateName(), QStringLiteral("UpdateAvailable"));
        QSignalSpy skipped(s.get(), &UpdateService::versionSkipped);
        s->skipVersion();
        QCOMPARE(skipped.size(), 1);
        QCOMPARE(s->stateName(), QStringLiteral("UpToDate"));
    }
};

QTEST_GUILESS_MAIN(TestUpdateDiscovery)
#include "tst_updatediscovery.moc"
