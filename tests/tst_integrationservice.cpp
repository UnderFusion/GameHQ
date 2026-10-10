#include <QtTest>

#include "integration/IntegrationProtocol.h"
#include "integration/IntegrationService.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QtEndian>

class IntegrationServiceTest : public QObject
{
    Q_OBJECT
private slots:
    void handshakeActionsAndStatus();
    void incompatibleHandshakeCloses();
    void lifecycleSyncAndDisconnectExpiry();
    void externalIdentityRequiresProcessOrSafeGamePath();
    void unknownStartedProcessIdKeepsTheSession();
    void xboxShellFallbackStaysNarrow();
    void overlappingPlayniteReconnectKeepsLiveState();
    void handlerDisconnectDuringBufferedFramesIsSafe();
};

namespace
{
void sendObject(QLocalSocket &socket, const QJsonObject &object)
{
    QString error;
    const QByteArray frame = integration::encodeFrame(object, error);
    QVERIFY2(!frame.isEmpty(), qPrintable(error));
    QCOMPARE(socket.write(frame), frame.size());
    socket.flush();
}

bool takeObject(QLocalSocket &socket, QJsonObject &object)
{
    if (socket.bytesAvailable() < 4 && !socket.waitForReadyRead(10))
        return false;
    const QByteArray prefix = socket.peek(4);
    const quint32 size = qFromLittleEndian<quint32>(prefix.constData());
    if (size == 0 || size > integration::MaximumPayloadBytes)
        return false;
    if (socket.bytesAvailable() < 4 + size)
        socket.waitForReadyRead(10);
    if (socket.bytesAvailable() < 4 + size)
        return false;
    socket.read(4);
    QJsonParseError error;
    const QByteArray payload = socket.read(size);
    const QJsonDocument document = QJsonDocument::fromJson(payload, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return false;
    }
    object = document.object();
    return true;
}

// A real GameHQ.exe instance already listening on the production pipe name
// would otherwise make these tests bind (or connect) to the wrong server.
QString testServerName()
{
    return QStringLiteral("GameHQ.Local.Test.%1").arg(QCoreApplication::applicationPid());
}

QJsonObject hello(int minimum = 1, int maximum = 1,
                  const QString &client = QStringLiteral("GameHQ.Test"))
{
    return {
        { QStringLiteral("type"), QStringLiteral("hello") },
        { QStringLiteral("client"), client },
        { QStringLiteral("clientVersion"), QStringLiteral("1.2.3") },
        { QStringLiteral("protocolMin"), minimum },
        { QStringLiteral("protocolMax"), maximum },
        { QStringLiteral("requestId"), QStringLiteral("hello-1") }
    };
}
}

void IntegrationServiceTest::handshakeActionsAndStatus()
{
    IntegrationService service(QStringLiteral("9.8.7"));
    QString error;
    const QString name = testServerName();
    QVERIFY2(service.start(error, name), qPrintable(error));
    QLocalSocket client;
    client.connectToServer(name);
    QVERIFY(client.waitForConnected(1000));

    sendObject(client, { { QStringLiteral("type"), QStringLiteral("status.request") },
                         { QStringLiteral("requestId"), QStringLiteral("before") } });
    QJsonObject reply;
    QTRY_VERIFY_WITH_TIMEOUT(client.bytesAvailable() >= 4, 1000);
    QVERIFY(takeObject(client, reply));
    QCOMPARE(reply.value(QStringLiteral("code")).toString(), QStringLiteral("not_handshaken"));

    sendObject(client, hello());
    QTRY_VERIFY_WITH_TIMEOUT(client.bytesAvailable() >= 4, 1000);
    QVERIFY(takeObject(client, reply));
    QCOMPARE(reply.value(QStringLiteral("type")).toString(), QStringLiteral("hello.ack"));
    QCOMPARE(reply.value(QStringLiteral("appVersion")).toString(), QStringLiteral("9.8.7"));
    QCOMPARE(reply.value(QStringLiteral("protocolSelected")).toInt(), 1);
    QCOMPARE(reply.value(QStringLiteral("requestId")).toString(), QStringLiteral("hello-1"));

    bool activated = false;
    connect(&service, &IntegrationService::activateRequested,
            this, [&activated] { activated = true; });
    sendObject(client, { { QStringLiteral("type"), QStringLiteral("app.activate") },
                         { QStringLiteral("requestId"), QStringLiteral("activate-1") } });
    QTRY_VERIFY_WITH_TIMEOUT(activated, 1000);
    QTRY_VERIFY_WITH_TIMEOUT(client.bytesAvailable() >= 4, 1000);
    QVERIFY(takeObject(client, reply));
    QCOMPARE(reply.value(QStringLiteral("type")).toString(), QStringLiteral("ack"));
    QCOMPARE(reply.value(QStringLiteral("requestId")).toString(), QStringLiteral("activate-1"));

    sendObject(client, { { QStringLiteral("type"), QStringLiteral("status.request") } });
    QTRY_VERIFY_WITH_TIMEOUT(client.bytesAvailable() >= 4, 1000);
    QVERIFY(takeObject(client, reply));
    QCOMPARE(reply.value(QStringLiteral("type")).toString(), QStringLiteral("status.response"));
    QCOMPARE(reply.value(QStringLiteral("externalGameCount")).toInt(), 0);
}

void IntegrationServiceTest::incompatibleHandshakeCloses()
{
    IntegrationService service(QStringLiteral("9.8.7"));
    QString error;
    const QString name = testServerName();
    QVERIFY2(service.start(error, name), qPrintable(error));
    QLocalSocket client;
    client.connectToServer(name);
    QVERIFY(client.waitForConnected(1000));
    sendObject(client, hello(2, 3));
    QJsonObject reply;
    QTRY_VERIFY_WITH_TIMEOUT(client.bytesAvailable() >= 4, 1000);
    QVERIFY(takeObject(client, reply));
    QCOMPARE(reply.value(QStringLiteral("code")).toString(),
             QStringLiteral("protocol_incompatible"));
    QTRY_COMPARE_WITH_TIMEOUT(client.state(), QLocalSocket::UnconnectedState, 1000);
}

void IntegrationServiceTest::lifecycleSyncAndDisconnectExpiry()
{
    IntegrationService service(QStringLiteral("9.8.7"), nullptr, 50);
    QString error;
    const QString name = testServerName();
    QVERIFY2(service.start(error, name), qPrintable(error));
    QLocalSocket client;
    client.connectToServer(name);
    QVERIFY(client.waitForConnected(1000));
    sendObject(client, hello(1, 1, QStringLiteral("GameHQ.Playnite")));
    QJsonObject reply;
    QTRY_VERIFY_WITH_TIMEOUT(client.bytesAvailable() >= 4, 1000);
    QVERIFY(takeObject(client, reply));

    sendObject(client, {
        { QStringLiteral("type"), QStringLiteral("playnite.game.started") },
        { QStringLiteral("sessionId"), QStringLiteral("session-a") },
        { QStringLiteral("playniteGameId"), QStringLiteral("game-a") },
        { QStringLiteral("name"), QStringLiteral("Example") },
        { QStringLiteral("startedProcessId"), 1234 }
    });
    QTRY_COMPARE_WITH_TIMEOUT(service.externalContext()->sessionCount(), 1, 1000);
    QTRY_VERIFY_WITH_TIMEOUT(client.bytesAvailable() >= 4, 1000);
    QVERIFY(takeObject(client, reply));

    sendObject(client, {
        { QStringLiteral("type"), QStringLiteral("playnite.state.sync") },
        { QStringLiteral("games"), QJsonArray{} }
    });
    QTRY_COMPARE_WITH_TIMEOUT(service.externalContext()->sessionCount(), 0, 1000);
    QTRY_VERIFY_WITH_TIMEOUT(client.bytesAvailable() >= 4, 1000);
    QVERIFY(takeObject(client, reply));

    sendObject(client, {
        { QStringLiteral("type"), QStringLiteral("playnite.game.started") },
        { QStringLiteral("sessionId"), QStringLiteral("session-b") },
        { QStringLiteral("name"), QStringLiteral("Example Two") }
    });
    QTRY_COMPARE_WITH_TIMEOUT(service.externalContext()->sessionCount(), 1, 1000);
    client.abort();
    QTRY_COMPARE_WITH_TIMEOUT(service.externalContext()->sessionCount(), 0, 1000);
}

// Playnite reports PID 0 when it cannot identify the started process; the
// session must survive (and authorize nothing by PID), not reject the message.
void IntegrationServiceTest::unknownStartedProcessIdKeepsTheSession()
{
    integration::ExternalGameContext context;
    QString error;
    QVERIFY2(context.upsert(QStringLiteral("GameHQ.Playnite"), {
        { QStringLiteral("sessionId"), QStringLiteral("session-zero") },
        { QStringLiteral("name"), QStringLiteral("Local Exe Game") },
        { QStringLiteral("installDirectory"), QStringLiteral("C:/Games/LocalExe") },
        { QStringLiteral("startedProcessId"), 0 }
    }, QStringLiteral("started"), error), qPrintable(error));
    QCOMPARE(context.sessionCount(), 1);
    QVERIFY(context.launchedSessions().isEmpty());
    auto noDescendant = [](quint32, quint32) { return false; };
    QCOMPARE(context.matchForeground(0, QString(), false, noDescendant).confidence,
             integration::MatchConfidence::None);
    QCOMPARE(context.matchForeground(42, QStringLiteral("C:/Games/LocalExe/game.exe"),
                                     true, noDescendant).confidence,
             integration::MatchConfidence::InstallDirectory);

    QVERIFY2(context.replaceSource(QStringLiteral("GameHQ.Playnite"), QJsonArray{
        QJsonObject{ { QStringLiteral("sessionId"), QStringLiteral("s1") },
                     { QStringLiteral("startedProcessId"), 0 } },
        QJsonObject{ { QStringLiteral("sessionId"), QStringLiteral("s2") },
                     { QStringLiteral("startedProcessId"), 77 } } }, error),
             qPrintable(error));
    QCOMPARE(context.sessionCount(), 2);
    QVERIFY(!context.upsert(QStringLiteral("GameHQ.Playnite"), {
        { QStringLiteral("sessionId"), QStringLiteral("bad") },
        { QStringLiteral("startedProcessId"), -1 } }, QStringLiteral("started"), error));
}

void IntegrationServiceTest::externalIdentityRequiresProcessOrSafeGamePath()
{
    integration::ExternalGameContext context;
    QString error;
    QVERIFY(context.upsert(QStringLiteral("GameHQ.Playnite"), {
        { QStringLiteral("sessionId"), QStringLiteral("session-a") },
        { QStringLiteral("playniteGameId"), QStringLiteral("game-a") },
        { QStringLiteral("name"), QStringLiteral("Trusted Title") },
        { QStringLiteral("installDirectory"), QStringLiteral("C:/Games/Trusted") },
        { QStringLiteral("startedProcessId"), 55 }
    }, QStringLiteral("started"), error));

    auto noDescendant = [](quint32, quint32) { return false; };
    const auto exact = context.matchForeground(
        55, QStringLiteral("C:/Unrelated/browser.exe"), false, noDescendant);
    QCOMPARE(exact.confidence, integration::MatchConfidence::ExactProcess);
    QVERIFY(exact.authorizesWindowedCapture());

    const auto descendant = context.matchForeground(
        77, QStringLiteral("C:/Unrelated/child.exe"), false,
        [](quint32 child, quint32 ancestor) { return child == 77 && ancestor == 55; });
    QCOMPARE(descendant.confidence, integration::MatchConfidence::DescendantProcess);
    QVERIFY(descendant.authorizesWindowedCapture());

    const auto nameOnly = context.matchForeground(
        99, QStringLiteral("C:/Unrelated/browser.exe"), false, noDescendant);
    QCOMPARE(nameOnly.confidence, integration::MatchConfidence::None);

    const auto pathWithoutGameGate = context.matchForeground(
        99, QStringLiteral("C:/Games/Trusted/game.exe"), false, noDescendant);
    QCOMPARE(pathWithoutGameGate.confidence, integration::MatchConfidence::None);
    const auto safePath = context.matchForeground(
        99, QStringLiteral("C:/Games/Trusted/game.exe"), true, noDescendant);
    QCOMPARE(safePath.confidence, integration::MatchConfidence::InstallDirectory);
    QVERIFY(!safePath.authorizesWindowedCapture());

    // relativeFilePath() returns an absolute path for another drive; that must
    // never count as "inside" the install directory.
    const auto crossDrive = context.matchForeground(
        99, QStringLiteral("D:/Other/game.exe"), true, noDescendant);
    QCOMPARE(crossDrive.confidence, integration::MatchConfidence::None);
}

void IntegrationServiceTest::xboxShellFallbackStaysNarrow()
{
    // Only Xbox mode surfaces may hand the capture gate to a Playnite game.
    QVERIFY(integration::isXboxShellSurface(QStringLiteral("XboxPcApp.exe"), {}));
    QVERIFY(integration::isXboxShellSurface(QStringLiteral("GameBar.exe"), {}));
    QVERIFY(integration::isXboxShellSurface(QStringLiteral("ApplicationFrameHost.exe"),
                                            QStringLiteral("XBOX")));
    QVERIFY(!integration::isXboxShellSurface(QStringLiteral("ApplicationFrameHost.exe"),
                                             QStringLiteral("Calculator")));
    QVERIFY(!integration::isXboxShellSurface(QStringLiteral("explorer.exe"), {}));
    QVERIFY(!integration::isXboxShellSurface(QStringLiteral("brave.exe"),
                                             QStringLiteral("Xbox")));

    // Only started sessions with a launched process id can be verified.
    integration::ExternalGameContext context;
    QString error;
    QVERIFY(context.upsert(QStringLiteral("GameHQ.Playnite"), {
        { QStringLiteral("sessionId"), QStringLiteral("launched") },
        { QStringLiteral("startedProcessId"), 55 }
    }, QStringLiteral("started"), error));
    QVERIFY(context.upsert(QStringLiteral("GameHQ.Playnite"), {
        { QStringLiteral("sessionId"), QStringLiteral("no-pid") }
    }, QStringLiteral("started"), error));
    QVERIFY(context.upsert(QStringLiteral("GameHQ.Playnite"), {
        { QStringLiteral("sessionId"), QStringLiteral("starting") },
        { QStringLiteral("startedProcessId"), 77 }
    }, QStringLiteral("starting"), error));
    const QList<integration::ExternalGameSession> launched = context.launchedSessions();
    QCOMPARE(launched.size(), 1);
    QCOMPARE(launched.first().sessionId, QStringLiteral("launched"));
}

void IntegrationServiceTest::overlappingPlayniteReconnectKeepsLiveState()
{
    // Playnite reconnects before its old socket reports the disconnect. The
    // stale disconnect must not expire the replacement's state.
    IntegrationService service(QStringLiteral("9.8.7"), nullptr, 50);
    QString error;
    const QString name = testServerName();
    QVERIFY2(service.start(error, name), qPrintable(error));

    QLocalSocket oldClient;
    oldClient.connectToServer(name);
    QVERIFY(oldClient.waitForConnected(1000));
    sendObject(oldClient, hello(1, 1, QStringLiteral("GameHQ.Playnite")));
    QJsonObject reply;
    QTRY_VERIFY_WITH_TIMEOUT(oldClient.bytesAvailable() >= 4, 1000);
    QVERIFY(takeObject(oldClient, reply));

    // The replacement connects and publishes its session while the old socket
    // is still open.
    QLocalSocket newClient;
    newClient.connectToServer(name);
    QVERIFY(newClient.waitForConnected(1000));
    sendObject(newClient, hello(1, 1, QStringLiteral("GameHQ.Playnite")));
    QTRY_VERIFY_WITH_TIMEOUT(newClient.bytesAvailable() >= 4, 1000);
    QVERIFY(takeObject(newClient, reply));
    sendObject(newClient, {
        { QStringLiteral("type"), QStringLiteral("playnite.game.started") },
        { QStringLiteral("sessionId"), QStringLiteral("session-new") },
        { QStringLiteral("name"), QStringLiteral("Live Title") },
        { QStringLiteral("startedProcessId"), 4321 }
    });
    QTRY_COMPARE_WITH_TIMEOUT(service.externalContext()->sessionCount(), 1, 1000);

    // Only now does the superseded socket drop.
    oldClient.abort();
    QTest::qWait(200); // well past the 50 ms grace used by this fixture
    QCOMPARE(service.externalContext()->sessionCount(), 1);

    // When the last Playnite client leaves, the state does expire.
    newClient.abort();
    QTRY_COMPARE_WITH_TIMEOUT(service.externalContext()->sessionCount(), 0, 1000);
}

void IntegrationServiceTest::handlerDisconnectDuringBufferedFramesIsSafe()
{
    // Two frames arrive in one read and the first one makes the service drop
    // the client. Dispatching the second must not touch the erased entry.
    IntegrationService service(QStringLiteral("9.8.7"), nullptr, 50);
    QString error;
    const QString name = testServerName();
    QVERIFY2(service.start(error, name), qPrintable(error));
    QLocalSocket client;
    client.connectToServer(name);
    QVERIFY(client.waitForConnected(1000));
    sendObject(client, hello(1, 1, QStringLiteral("GameHQ.Playnite")));
    QJsonObject reply;
    QTRY_VERIFY_WITH_TIMEOUT(client.bytesAvailable() >= 4, 1000);
    QVERIFY(takeObject(client, reply));

    // A malformed lifecycle message is a fatal protocol error: the handler
    // disconnects the client while readClient still has a frame to deliver.
    QString encodeError;
    const QByteArray fatal = integration::encodeFrame({
        { QStringLiteral("type"), QStringLiteral("playnite.game.started") },
        { QStringLiteral("sessionId"), QJsonValue() }
    }, encodeError);
    QVERIFY(!fatal.isEmpty());
    const QByteArray trailing = integration::encodeFrame({
        { QStringLiteral("type"), QStringLiteral("status.request") },
        { QStringLiteral("requestId"), QStringLiteral("after-disconnect") }
    }, encodeError);
    QVERIFY(!trailing.isEmpty());

    const QByteArray both = fatal + trailing;
    QCOMPARE(client.write(both), both.size());
    client.flush();

    // The service must survive and the session state must stay empty.
    QTest::qWait(200);
    QCOMPARE(service.externalContext()->sessionCount(), 0);

    // The server is still healthy for a brand new client.
    QLocalSocket fresh;
    fresh.connectToServer(name);
    QVERIFY(fresh.waitForConnected(1000));
    sendObject(fresh, hello(1, 1, QStringLiteral("GameHQ.Test")));
    QTRY_VERIFY_WITH_TIMEOUT(fresh.bytesAvailable() >= 4, 1000);
    QVERIFY(takeObject(fresh, reply));
    QCOMPARE(reply.value(QStringLiteral("type")).toString(), QStringLiteral("hello.ack"));
}

QTEST_GUILESS_MAIN(IntegrationServiceTest)
#include "tst_integrationservice.moc"
