#include "share/ShareService.h"
#include "share/external/ExternalProviderHost.h"
#include "share/external/ExternalProviderProtocol.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <QtEndian>

using namespace share;
using namespace share::external;

namespace
{
// A minimal third-party provider as a test would write it: it knows only the
// wire protocol from the specification, nothing of GameHQ's internals.
class Peer
{
public:
    QLocalSocket socket;
    FrameDecoder decoder;
    QList<QJsonObject> inbox;

    bool connectTo(const QString& name)
    {
        socket.connectToServer(name);
        return socket.waitForConnected(3000);
    }

    void sendRaw(const QByteArray& bytes)
    {
        socket.write(bytes);
        socket.flush();
    }

    void send(const QJsonObject& message)
    {
        QString error;
        sendRaw(encodeFrame(message, error));
    }

    // Pumps the event loop until a message of `type` arrives (or `ms` elapse).
    QJsonObject waitFor(const QString& type, int ms = 3000)
    {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < ms) {
            for (int i = 0; i < inbox.size(); ++i) {
                if (inbox.at(i).value("type").toString() == type)
                    return inbox.takeAt(i);
            }
            pump(20);
        }
        return {};
    }

    void pump(int ms)
    {
        // Data may already sit in the socket's buffer (it shares this event
        // loop), where waitForReadyRead would never see it as "new".
        if (socket.bytesAvailable() > 0 || socket.waitForReadyRead(ms)) {
            QList<QJsonObject> got;
            QString error;
            decoder.append(socket.readAll(), got, error);
            inbox.append(got);
        }
        QCoreApplication::processEvents();
    }

    bool waitClosed(int ms = 3000)
    {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < ms) {
            if (socket.state() != QLocalSocket::ConnectedState)
                return true;
            pump(20);
        }
        return socket.state() != QLocalSocket::ConnectedState;
    }

    static QJsonObject hello(const QString& id, const QStringList& caps, int pMin = 1, int pMax = 1,
                             const QJsonObject& extra = {})
    {
        QJsonObject provider{ { "id", id }, { "name", "Test Provider" }, { "version", "1.0" },
                              { "capabilities", QJsonArray::fromStringList(caps) } };
        for (auto it = extra.begin(); it != extra.end(); ++it)
            provider.insert(it.key(), it.value());
        return { { "type", "hello" }, { "requestId", "r1" }, { "protocolMin", pMin },
                 { "protocolMax", pMax }, { "provider", provider } };
    }
};
} // namespace

// Share Provider API v1 (t18). The peer here is a stand-in for an untrusted
// third-party process; GameHQ's side is the real host, registry and service.
class TestShareExternalProvider : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString m_pipe;

    QString capture(const QString& name, const QByteArray& content = "media-bytes")
    {
        const QString path = m_dir.filePath(name);
        QFile f(path);
        f.open(QIODevice::WriteOnly);
        f.write(content);
        return path;
    }

    // Connects, handshakes and returns the ack (empty on failure).
    QJsonObject register_(Peer& peer, const QString& id, const QStringList& caps,
                          const QJsonObject& extra = {})
    {
        if (!peer.connectTo(m_pipe))
            return {};
        peer.send(Peer::hello(id, caps, 1, 1, extra));
        return peer.waitFor("hello.ack");
    }

    struct Started
    {
        QJsonObject job;
        QString target;
    };

    Started startJob(Service& s, Peer& peer, const QString& providerId, const QString& file)
    {
        Started out;
        if (!s.open(file) || !s.requestTargets(providerId))
            return out;
        const QJsonObject req = peer.waitFor("targets.request");
        peer.send({ { "type", "targets.result" }, { "requestId", req.value("requestId") },
                    { "targets", QJsonArray{ QJsonObject{ { "id", "dest1" }, { "name", "Dest" } } } } });
        if (!QTest::qWaitFor([&] { return !s.targetsLoading(); }, 3000))
            return out;
        out.target = "dest1";
        if (s.share(providerId, "dest1").isEmpty())
            return out;
        out.job = peer.waitFor("job.start");
        return out;
    }

private slots:
    void init() { m_pipe = "GameHQ.Share.Provider.Test." + QUuid::createUuid().toString(QUuid::Id128); }

    // ── Handshake and negotiation ───────────────────────────────────────
    void compatibleHandshakeRegistersTheProvider()
    {
        Service s;
        ExternalProviderHost host(s.registry());
        QString err;
        QVERIFY(host.start(err, m_pipe));
        Peer peer;
        const QJsonObject ack = register_(peer, "ext.test", { "image", "video", "direct_send" });
        QCOMPARE(ack.value("protocolSelected").toInt(), 1);
        QCOMPARE(ack.value("providerId").toString(), QStringLiteral("ext.test"));
        QCOMPARE(ack.value("requestId").toString(), QStringLiteral("r1"));
        QVERIFY(ack.value("limits").toObject().value("maxFrameBytes").toInt() > 0);
        QVERIFY(s.registry()->find("ext.test"));
        QCOMPARE(host.providerCount(), 1);

        QVERIFY(s.open(capture("a.png")));
        const QVariantMap row = s.providers().first().toMap();
        QCOMPARE(row.value("id").toString(), QStringLiteral("ext.test"));
        QCOMPARE(row.value("available").toBool(), true);
        // Self-declared, so it is always presented as third-party.
        QVERIFY(row.value("privacy").toString().length() > 10);
    }

    void newerAndOlderProtocolRanges()
    {
        Service s;
        ExternalProviderHost host(s.registry());
        QString err;
        QVERIFY(host.start(err, m_pipe));

        Peer newer;   // understands 1..5: the highest common version is 1
        QVERIFY(newer.connectTo(m_pipe));
        newer.send(Peer::hello("ext.newer", { "image" }, 1, 5));
        QCOMPARE(newer.waitFor("hello.ack").value("protocolSelected").toInt(), 1);

        Peer tooNew;   // 2..3: no overlap
        QVERIFY(tooNew.connectTo(m_pipe));
        tooNew.send(Peer::hello("ext.toonew", { "image" }, 2, 3));
        const QJsonObject e = tooNew.waitFor("error");
        QCOMPARE(e.value("code").toString(), QStringLiteral("protocol_incompatible"));
        QCOMPARE(e.value("requestId").toString(), QStringLiteral("r1"));
        QVERIFY(tooNew.waitClosed());
        QVERIFY(!s.registry()->find("ext.toonew"));
        QVERIFY(s.registry()->find("ext.newer"));   // the good one is unaffected
    }

    void unknownAndReservedCapabilitiesAreIgnoredAndReported()
    {
        Service s;
        ExternalProviderHost host(s.registry());
        QString err;
        QVERIFY(host.start(err, m_pipe));
        Peer peer;
        const QJsonObject ack = register_(peer, "ext.caps",
                                          { "image", "teleport", "requires_account", "saved_targets", "image" });
        QCOMPARE(ack.value("capabilities").toArray().size(), 1);   // image, once
        const QJsonArray ignored = ack.value("ignoredCapabilities").toArray();
        QCOMPARE(ignored.size(), 3);
        // Reserved flags cannot be claimed: they would change how GameHQ treats the provider.
        Provider* p = s.registry()->find("ext.caps");
        QVERIFY(p);
        QVERIFY(!p->capabilities().testFlag(Capability::RequiresAccount));
        QVERIFY(!p->capabilities().testFlag(Capability::SavedTargets));
        QVERIFY(!p->capabilities().testFlag(Capability::Video));
    }

    void invalidManifestsAreRefused_data()
    {
        QTest::addColumn<QString>("id");
        QTest::addColumn<QStringList>("caps");
        QTest::addColumn<QString>("code");
        QTest::newRow("built-in namespace") << "telegram.desktop" << QStringList{ "image" } << "invalid_manifest";
        QTest::newRow("no prefix") << "myprovider" << QStringList{ "image" } << "invalid_manifest";
        QTest::newRow("bad characters") << "ext.bad id!" << QStringList{ "image" } << "invalid_manifest";
        QTest::newRow("no media") << "ext.nomedia" << QStringList{ "contacts", "target_search" } << "no_media_capability";
        QTest::newRow("both send modes") << "ext.both" << QStringList{ "image", "direct_send", "external_handoff" } << "invalid_capabilities";
    }

    void invalidManifestsAreRefused()
    {
        QFETCH(QString, id);
        QFETCH(QStringList, caps);
        QFETCH(QString, code);
        Service s;
        ExternalProviderHost host(s.registry());
        QString err;
        QVERIFY(host.start(err, m_pipe));
        Peer peer;
        QVERIFY(peer.connectTo(m_pipe));
        peer.send(Peer::hello(id, caps));
        QCOMPARE(peer.waitFor("error").value("code").toString(), code);
        QVERIFY(peer.waitClosed());
        QVERIFY(s.registry()->providers().isEmpty());
    }

    void displayTextIsSanitisedAndBounded()
    {
        Service s;
        ExternalProviderHost host(s.registry());
        QString err;
        QVERIFY(host.start(err, m_pipe));
        Peer peer;
        QJsonObject extra{ { "name", QString("Evil\u202E  \n\tName") + QString(200, 'x') } };
        QVERIFY(!register_(peer, "ext.text", { "image" }, extra).isEmpty());
        const QString name = s.registry()->find("ext.text")->displayName();
        QVERIFY(name.size() <= kMaxNameChars);
        QVERIFY(!name.contains(QChar(0x202E)) && !name.contains('\n') && !name.contains('\t'));
        QVERIFY(name.startsWith("Evil Name"));
    }

    void duplicateProviderIdentityIsRefusedAndTheRunningOneSurvives()
    {
        Service s;
        ExternalProviderHost host(s.registry());
        QString err;
        QVERIFY(host.start(err, m_pipe));
        Peer first, second;
        QVERIFY(!register_(first, "ext.dup", { "image" }).isEmpty());
        QVERIFY(second.connectTo(m_pipe));
        second.send(Peer::hello("ext.dup", { "video" }));
        QCOMPARE(second.waitFor("error").value("code").toString(), QStringLiteral("duplicate_provider"));
        QVERIFY(second.waitClosed());
        // The original registration is untouched (same capabilities, still up).
        Provider* p = s.registry()->find("ext.dup");
        QVERIFY(p);
        QVERIFY(p->capabilities().testFlag(Capability::Image));
        QVERIFY(!p->capabilities().testFlag(Capability::Video));
        QCOMPARE(first.socket.state(), QLocalSocket::ConnectedState);
    }

    // ── Hostile input ───────────────────────────────────────────────────
    void requestsBeforeTheHandshakeAreRefused()
    {
        Service s;
        ExternalProviderHost host(s.registry());
        QString err;
        QVERIFY(host.start(err, m_pipe));
        Peer peer;
        QVERIFY(peer.connectTo(m_pipe));
        peer.send({ { "type", "job.result" }, { "jobId", "x" }, { "requestId", "q9" } });
        const QJsonObject e = peer.waitFor("error");
        QCOMPARE(e.value("code").toString(), QStringLiteral("not_handshaken"));
        QCOMPARE(e.value("requestId").toString(), QStringLiteral("q9"));
        QVERIFY(peer.waitClosed());
        QVERIFY(s.registry()->providers().isEmpty());
    }

    void theFinalErrorSurvivesASlowReader()
    {
        // A provider that is busy (or simply polling) when it is rejected must
        // still learn why: closing the server end of a Windows pipe discards
        // unread data, so the host lets the error be read before it closes.
        Service s;
        ExternalProviderHost host(s.registry());
        QString err;
        QVERIFY(host.start(err, m_pipe));
        Peer peer;
        QVERIFY(peer.connectTo(m_pipe));
        peer.send(Peer::hello("ext.toonew", { "image" }, 2, 3));
        QTest::qWait(120);   // not reading yet: the host has already rejected us
        QCOMPARE(host.providerCount(), 0);
        QCOMPARE(host.connectionCount(), 0);   // registration state is already gone
        const QJsonObject e = peer.waitFor("error");
        QCOMPARE(e.value("code").toString(), QStringLiteral("protocol_incompatible"));
        QVERIFY(peer.waitClosed());
    }

    void aSilentPeerIsDroppedAtTheHandshakeTimeout()
    {
        Service s;
        ExternalProviderHost host(s.registry());
        host.setHandshakeTimeoutMs(300);
        QString err;
        QVERIFY(host.start(err, m_pipe));
        Peer peer;
        QVERIFY(peer.connectTo(m_pipe));
        QCOMPARE(peer.waitFor("error", 2000).value("code").toString(), QStringLiteral("handshake_timeout"));
        QVERIFY(peer.waitClosed());
        QTRY_COMPARE(host.connectionCount(), 0);
    }

    void malformedAndOversizedFramesDropTheConnectionNotTheApp_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        const auto frame = [](const QByteArray& payload) {
            QByteArray f(4, 0);
            qToLittleEndian<quint32>(quint32(payload.size()), f.data());
            return f + payload;
        };
        QTest::newRow("not json") << frame("this is not json");
        QTest::newRow("json array") << frame("[1,2,3]");
        QTest::newRow("no type") << frame("{\"a\":1}");
        QTest::newRow("invalid utf8") << frame(QByteArray("{\"type\":\"\xFF\xFE\"}"));
        QTest::newRow("zero length") << QByteArray(4, 0);
        QByteArray huge(4, 0);
        qToLittleEndian<quint32>(kMaxFrameBytes + 1, huge.data());
        QTest::newRow("oversized length") << huge;
        QTest::newRow("absurd length") << QByteArray("\xFF\xFF\xFF\x7F", 4);
    }

    void malformedAndOversizedFramesDropTheConnectionNotTheApp()
    {
        QFETCH(QByteArray, bytes);
        Service s;
        ExternalProviderHost host(s.registry());
        QString err;
        QVERIFY(host.start(err, m_pipe));
        Peer bad;
        QVERIFY(bad.connectTo(m_pipe));
        bad.sendRaw(bytes);
        QVERIFY(bad.waitClosed());
        QTRY_COMPARE(host.connectionCount(), 0);
        // The host is still fully functional.
        Peer good;
        QVERIFY(!register_(good, "ext.after", { "image" }).isEmpty());
    }

    void aFloodOfBytesCannotGrowTheInputQueueWithoutBound()
    {
        Service s;
        ExternalProviderHost host(s.registry());
        QString err;
        QVERIFY(host.start(err, m_pipe));
        Peer bad;
        QVERIFY(bad.connectTo(m_pipe));
        // A valid length prefix promising a full frame, then only filler.
        QByteArray head(4, 0);
        qToLittleEndian<quint32>(kMaxFrameBytes, head.data());
        bad.sendRaw(head);
        for (int i = 0; i < 12 && bad.socket.state() == QLocalSocket::ConnectedState; ++i) {
            bad.sendRaw(QByteArray(60 * 1024, 'a'));
            bad.pump(5);
        }
        // Whether it hit the invalid-JSON check or the queue limit, it is gone.
        QVERIFY(bad.waitClosed());
        QTRY_COMPARE(host.connectionCount(), 0);
    }

    void unknownMessageTypesAreViolationsAndRepeatOffendersAreDropped()
    {
        Service s;
        ExternalProviderHost host(s.registry());
        QString err;
        QVERIFY(host.start(err, m_pipe));
        Peer peer;
        QVERIFY(!register_(peer, "ext.rude", { "image" }).isEmpty());
        for (int i = 0; i < ExternalProviderHost::kMaxViolations - 1; ++i) {
            peer.send({ { "type", "shell.exec" }, { "cmd", "calc.exe" }, { "requestId", "z" } });
            QCOMPARE(peer.waitFor("error").value("code").toString(), QStringLiteral("unknown_type"));
        }
        QCOMPARE(peer.socket.state(), QLocalSocket::ConnectedState);   // still tolerated
        peer.send({ { "type", "shell.exec" } });
        QVERIFY(peer.waitClosed());
        QVERIFY(!s.registry()->find("ext.rude"));   // its registration went with it
    }

    void aSecondHelloIsAViolation()
    {
        Service s;
        ExternalProviderHost host(s.registry());
        QString err;
        QVERIFY(host.start(err, m_pipe));
        Peer peer;
        QVERIFY(!register_(peer, "ext.twice", { "image" }).isEmpty());
        peer.send(Peer::hello("ext.other", { "image" }));
        QCOMPARE(peer.waitFor("error").value("code").toString(), QStringLiteral("already_handshaken"));
        QVERIFY(!s.registry()->find("ext.other"));
    }

    void connectionCountIsCapped()
    {
        Service s;
        ExternalProviderHost host(s.registry());
        QString err;
        QVERIFY(host.start(err, m_pipe));
        QList<std::shared_ptr<Peer>> peers;
        for (int i = 0; i < ExternalProviderHost::kMaxConnections; ++i) {
            auto p = std::make_shared<Peer>();
            QVERIFY(p->connectTo(m_pipe));
            peers.append(p);
        }
        QTRY_COMPARE(host.connectionCount(), ExternalProviderHost::kMaxConnections);
        Peer extra;
        QVERIFY(extra.connectTo(m_pipe));
        QCOMPARE(extra.waitFor("error").value("code").toString(), QStringLiteral("too_many_providers"));
        QVERIFY(extra.waitClosed());
    }

    // ── Targets ─────────────────────────────────────────────────────────
    void targetListingSearchAndValidation()
    {
        Service s;
        ExternalProviderHost host(s.registry());
        QString err;
        QVERIFY(host.start(err, m_pipe));
        Peer peer;
        QVERIFY(!register_(peer, "ext.dest", { "image", "contacts", "target_search" }).isEmpty());
        QVERIFY(s.open(capture("t.png")));

        QVERIFY(s.requestTargets("ext.dest", "ann"));
        const QJsonObject req = peer.waitFor("targets.request");
        QCOMPARE(req.value("query").toString(), QStringLiteral("ann"));
        QCOMPARE(req.value("mediaKind").toString(), QStringLiteral("image"));

        QJsonArray targets;
        targets.append(QJsonObject{ { "id", "u1" }, { "name", "Ann" }, { "kind", "contact" }, { "subtitle", "friend" } });
        targets.append(QJsonObject{ { "id", "u1" }, { "name", "Duplicate id" } });                 // duplicate
        targets.append(QJsonObject{ { "id", "bad id with spaces" }, { "name", "Bad" } });          // bad id
        targets.append(QJsonObject{ { "id", "u2" }, { "name", "" } });                              // no name
        targets.append(QJsonObject{ { "id", "u3" }, { "name", "Weird" }, { "kind", "galaxy" } });   // bad kind
        targets.append(QJsonObject{ { "id", "u4" }, { "name", "Video only" }, { "media", QJsonArray{ "video" } } });
        targets.append(QString("not an object"));
        peer.send({ { "type", "targets.result" }, { "requestId", req.value("requestId") }, { "targets", targets } });

        QTRY_VERIFY(!s.targetsLoading());
        QCOMPARE(s.targets().size(), 1);   // u4 is video-only and this is an image
        QCOMPARE(s.targets().first().toMap().value("id").toString(), QStringLiteral("u1"));
        QCOMPARE(s.targets().first().toMap().value("providerId").toString(), QStringLiteral("ext.dest"));
    }

    void aProviderWithoutTargetSearchNeverSeesTheSearchText()
    {
        Service s;
        ExternalProviderHost host(s.registry());
        QString err;
        QVERIFY(host.start(err, m_pipe));
        Peer peer;
        QVERIFY(!register_(peer, "ext.nosearch", { "image" }).isEmpty());
        QVERIFY(s.open(capture("q.png")));
        QVERIFY(s.requestTargets("ext.nosearch", "secret words"));
        QCOMPARE(peer.waitFor("targets.request").value("query").toString(), QString());
    }

    void targetListsAreBounded()
    {
        Service s;
        ExternalProviderHost host(s.registry());
        QString err;
        QVERIFY(host.start(err, m_pipe));
        Peer peer;
        QVERIFY(!register_(peer, "ext.many", { "image" }).isEmpty());
        QVERIFY(s.open(capture("m.png")));
        QVERIFY(s.requestTargets("ext.many"));
        const QJsonObject req = peer.waitFor("targets.request");
        QJsonArray targets;
        for (int i = 0; i < 400; ++i)
            targets.append(QJsonObject{ { "id", QString("t%1").arg(i) }, { "name", QString("T%1").arg(i) } });
        peer.send({ { "type", "targets.result" }, { "requestId", req.value("requestId") }, { "targets", targets } });
        QTRY_VERIFY(!s.targetsLoading());
        QCOMPARE(s.targets().size(), kMaxTargets);
    }

    void aSilentProviderTimesOutTheTargetQuery()
    {
        Service s;
        ExternalProviderHost host(s.registry());
        host.setTargetsTimeoutMs(200);
        QString err;
        QVERIFY(host.start(err, m_pipe));
        Peer peer;
        QVERIFY(!register_(peer, "ext.slow", { "image" }).isEmpty());
        QVERIFY(s.open(capture("s.png")));
        QVERIFY(s.requestTargets("ext.slow"));
        QVERIFY(s.targetsLoading());
        QTRY_VERIFY_WITH_TIMEOUT(!s.targetsLoading(), 2000);
        QCOMPARE(s.lastError(), QStringLiteral("timeout"));
        QCOMPARE(s.targets().size(), 0);
        // A late answer changes nothing.
        const QJsonObject req = peer.waitFor("targets.request");
        peer.send({ { "type", "targets.result" }, { "requestId", req.value("requestId") },
                    { "targets", QJsonArray{ QJsonObject{ { "id", "late" }, { "name", "Late" } } } } });
        peer.pump(100);
        QCOMPARE(s.targets().size(), 0);
        QCOMPARE(peer.socket.state(), QLocalSocket::ConnectedState);   // and it is not a violation
    }

    // ── Jobs ────────────────────────────────────────────────────────────
    void anExplicitShareDeliversOnlyTheSelectedCaptureAndHonoursTheResult()
    {
        Service s;
        ExternalProviderHost host(s.registry());
        QString err;
        QVERIFY(host.start(err, m_pipe));
        Peer peer;
        QVERIFY(!register_(peer, "ext.send", { "image", "direct_send" }).isEmpty());
        const QString file = capture("clip.png", "PNGDATA");
        const Started st = startJob(s, peer, "ext.send", file);
        QVERIFY(!st.job.isEmpty());

        // The job carries exactly the chosen file, described by GameHQ.
        const QJsonObject f = st.job.value("file").toObject();
        QCOMPARE(QFileInfo(f.value("path").toString()).absoluteFilePath(), QFileInfo(file).absoluteFilePath());
        QCOMPARE(f.value("fileName").toString(), QStringLiteral("clip.png"));
        QCOMPARE(f.value("sizeBytes").toInt(), 7);
        QCOMPARE(f.value("mediaKind").toString(), QStringLiteral("image"));
        QCOMPARE(st.job.value("targetId").toString(), QStringLiteral("dest1"));
        QVERIFY(st.job.value("token").toString().size() >= 32);
        // No bytes travel in the control channel.
        QVERIFY(!QJsonDocument(st.job).toJson().contains("PNGDATA"));

        const QJsonObject ident{ { "jobId", st.job.value("jobId") }, { "token", st.job.value("token") } };
        auto msg = [&](const QString& type, QJsonObject extra) {
            extra.insert("type", type);
            for (auto it = ident.begin(); it != ident.end(); ++it)
                extra.insert(it.key(), it.value());
            return extra;
        };
        QSignalSpy progress(&s, &Service::phaseChanged);
        peer.send(msg("job.progress", { { "state", "transferring" }, { "progress", 0.5 } }));
        peer.pump(100);
        QCOMPARE(peer.socket.state(), QLocalSocket::ConnectedState);

        QSignalSpy done(&s, &Service::finished);
        peer.send(msg("job.result", { { "outcome", "sent" } }));
        QVERIFY(done.wait(3000));
        QCOMPARE(s.lastResult().value("outcome").toString(), QStringLiteral("sent"));

        // The token died with the job: replaying the result is a stale-job error.
        peer.send(msg("job.result", { { "outcome", "failed" } }));
        QCOMPARE(peer.waitFor("error").value("code").toString(), QStringLiteral("stale_job"));
        QCOMPARE(s.lastResult().value("outcome").toString(), QStringLiteral("sent"));
    }

    void aProviderCannotClaimSentWithoutDirectSend()
    {
        Service s;
        ExternalProviderHost host(s.registry());
        QString err;
        QVERIFY(host.start(err, m_pipe));
        Peer peer;
        QVERIFY(!register_(peer, "ext.handoff", { "image", "external_handoff" }).isEmpty());
        const Started st = startJob(s, peer, "ext.handoff", capture("h.png"));
        QVERIFY(!st.job.isEmpty());
        QSignalSpy done(&s, &Service::finished);
        peer.send({ { "type", "job.result" }, { "jobId", st.job.value("jobId") },
                    { "token", st.job.value("token") }, { "outcome", "sent" } });
        QVERIFY(done.wait(3000));
        QCOMPARE(s.lastResult().value("outcome").toString(), QStringLiteral("handed_off"));
    }

    void resultsForTheWrongJobOrTokenAreIgnoredAsStale()
    {
        Service s;
        ExternalProviderHost host(s.registry());
        QString err;
        QVERIFY(host.start(err, m_pipe));
        Peer peer;
        QVERIFY(!register_(peer, "ext.stale", { "image", "direct_send" }).isEmpty());
        const Started st = startJob(s, peer, "ext.stale", capture("st.png"));
        QVERIFY(!st.job.isEmpty());

        peer.send({ { "type", "job.result" }, { "jobId", st.job.value("jobId") },
                    { "token", "guessed-token" }, { "outcome", "sent" } });
        QCOMPARE(peer.waitFor("error").value("code").toString(), QStringLiteral("stale_job"));
        peer.send({ { "type", "job.result" }, { "jobId", "some-other-job" },
                    { "token", st.job.value("token") }, { "outcome", "sent" } });
        QCOMPARE(peer.waitFor("error").value("code").toString(), QStringLiteral("stale_job"));
        peer.send({ { "type", "job.progress" }, { "jobId", "nope" }, { "token", "nope" },
                    { "state", "transferring" } });
        QCOMPARE(peer.waitFor("error").value("code").toString(), QStringLiteral("stale_job"));
        QVERIFY(s.busy());   // none of that finished the real job
        QCOMPARE(s.lastResult().value("outcome").toString(), QString());
    }

    void thereIsNoWayToAskForAnotherFile()
    {
        Service s;
        ExternalProviderHost host(s.registry());
        QString err;
        QVERIFY(host.start(err, m_pipe));
        Peer peer;
        QVERIFY(!register_(peer, "ext.grab", { "image", "direct_send" }).isEmpty());
        const Started st = startJob(s, peer, "ext.grab", capture("only-this.png"));
        QVERIFY(!st.job.isEmpty());
        // Every conceivable "give me a file" request is simply not in the protocol.
        const QStringList attempts = { "file.open", "file.read", "fs.browse", "path.request" };
        for (const QString& type : attempts) {
            peer.send({ { "type", type }, { "jobId", st.job.value("jobId") }, { "token", st.job.value("token") },
                        { "path", "C:\\Windows\\System32\\config\\SAM" } });
            const QJsonObject e = peer.waitFor("error");
            QCOMPARE(e.value("code").toString(), QStringLiteral("unknown_type"));
            // and nothing came back that could be a path or file content
            QVERIFY(!QJsonDocument(e).toJson().contains("SAM"));
        }
        // Still connected and still the same job: nothing was granted or read.
        QCOMPARE(peer.socket.state(), QLocalSocket::ConnectedState);
        QCOMPARE(s.busy(), true);
    }

    void cancellationReachesTheProviderAndItsVerdictIsReported()
    {
        Service s;
        ExternalProviderHost host(s.registry());
        QString err;
        QVERIFY(host.start(err, m_pipe));
        Peer peer;
        QVERIFY(!register_(peer, "ext.cancel", { "image", "direct_send" }).isEmpty());
        const Started st = startJob(s, peer, "ext.cancel", capture("c.png"));
        QVERIFY(!st.job.isEmpty());
        peer.send({ { "type", "job.progress" }, { "jobId", st.job.value("jobId") },
                    { "token", st.job.value("token") }, { "state", "transferring" }, { "progress", 0.2 } });
        peer.pump(100);

        s.cancel();
        const QJsonObject cancel = peer.waitFor("job.cancel");
        QCOMPARE(cancel.value("jobId"), st.job.value("jobId"));
        QCOMPARE(cancel.value("token"), st.job.value("token"));
        QVERIFY(s.busy());   // data may be in flight: wait for the provider's word

        QSignalSpy done(&s, &Service::finished);
        peer.send({ { "type", "job.result" }, { "jobId", st.job.value("jobId") },
                    { "token", st.job.value("token") }, { "outcome", "cancelled" } });
        QVERIFY(done.wait(3000));
        QCOMPARE(s.lastResult().value("outcome").toString(), QStringLiteral("cancelled"));
    }

    void aSilentProviderEndsTheJobAsUnconfirmedAtTheTimeout()
    {
        Service s;
        ExternalProviderHost host(s.registry());
        QString err;
        QVERIFY(host.start(err, m_pipe));
        Peer peer;
        QVERIFY(!register_(peer, "ext.mute", { "image", "direct_send" }, { { "jobTimeoutMs", 1000 } }).isEmpty());
        const Started st = startJob(s, peer, "ext.mute", capture("mu.png"));
        QVERIFY(!st.job.isEmpty());
        QSignalSpy done(&s, &Service::finished);
        QVERIFY(done.wait(4000));
        QCOMPARE(s.lastResult().value("outcome").toString(), QStringLiteral("unconfirmed"));
        QCOMPARE(s.lastResult().value("errorCode").toString(), QStringLiteral("timeout"));
    }

    void aProviderCrashingMidJobIsUnconfirmedAndTheAppKeepsRunning()
    {
        Service s;
        ExternalProviderHost host(s.registry());
        QString err;
        QVERIFY(host.start(err, m_pipe));
        auto peer = std::make_unique<Peer>();
        QVERIFY(!register_(*peer, "ext.crash", { "image", "direct_send" }).isEmpty());
        const Started st = startJob(s, *peer, "ext.crash", capture("cr.png"));
        QVERIFY(!st.job.isEmpty());

        QSignalSpy done(&s, &Service::finished);
        peer->socket.abort();   // the process dies without a goodbye
        QVERIFY(done.wait(3000));
        QCOMPARE(s.lastResult().value("outcome").toString(), QStringLiteral("unconfirmed"));
        QCOMPARE(s.lastResult().value("errorCode").toString(), QStringLiteral("provider_disconnected"));
        QTRY_VERIFY(!s.registry()->find("ext.crash"));
        QCOMPARE(host.connectionCount(), 0);
        QVERIFY(!s.busy());

        // GameHQ is unharmed: a fresh provider registers and the service works.
        Peer next;
        QVERIFY(!register_(next, "ext.crash", { "image" }).isEmpty());   // identity is free again
    }

    void aCleanGoodbyeRemovesTheProvider()
    {
        Service s;
        ExternalProviderHost host(s.registry());
        QString err;
        QVERIFY(host.start(err, m_pipe));
        Peer peer;
        QVERIFY(!register_(peer, "ext.bye", { "image" }).isEmpty());
        peer.send({ { "type", "goodbye" } });
        QTRY_VERIFY(!s.registry()->find("ext.bye"));
        QVERIFY(peer.waitClosed());
    }

    void stoppingTheHostRemovesEveryProvider()
    {
        Service s;
        ExternalProviderHost host(s.registry());
        QString err;
        QVERIFY(host.start(err, m_pipe));
        Peer a, b;
        QVERIFY(!register_(a, "ext.a", { "image" }).isEmpty());
        QVERIFY(!register_(b, "ext.b", { "image" }).isEmpty());
        QCOMPARE(s.registry()->providers().size(), 2);
        host.stop();
        QVERIFY(s.registry()->providers().isEmpty());
        QVERIFY(a.waitClosed());
    }

    void withoutAHostThereIsNoListener()
    {
        // Feature off: nothing listens, so a provider simply cannot connect.
        Peer peer;
        QVERIFY(!peer.connectTo(m_pipe));
    }
};

QTEST_MAIN(TestShareExternalProvider)
#include "tst_shareexternalprovider.moc"
