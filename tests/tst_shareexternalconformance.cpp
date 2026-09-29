#include "share/ShareService.h"
#include "share/external/ExternalProviderHost.h"

#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

using namespace share;
using namespace share::external;

#ifndef GAMEHQ_SHARE_SAMPLE_PATH
#error "GAMEHQ_SHARE_SAMPLE_PATH must point at integrations/share-provider/sample_provider.py"
#endif

namespace
{
// A provider running as its own OS process. It knows the public wire protocol
// and nothing of this test binary.
struct ProviderProcess
{
    QProcess process;

    ~ProviderProcess()
    {
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished(3000);
        }
    }
    QString output() { return QString::fromUtf8(process.readAllStandardError()); }
};
} // namespace

// Share Provider API v1 conformance (t19): the sample provider is launched as a
// separate process and talks to the real host over the real local pipe, so this
// covers what an in-process peer cannot: process start-up and connect timing,
// real OS reads and writes, clean exit, a crash mid-job, reconnecting after the
// host restarts, and cancellation crossing the process boundary.
//
// Any provider can be checked the same way: set GAMEHQ_SHARE_PYTHON to pick the
// interpreter. The suite skips (does not fail) when no Python is available.
class TestShareExternalConformance : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString m_python;
    QString m_pipe;
    QString m_dest;

    QString capture(const QString& name, qsizetype size = 300 * 1024)
    {
        const QString path = m_dir.filePath(name);
        QFile f(path);
        f.open(QIODevice::WriteOnly);
        QByteArray data(size, 0);
        for (qsizetype i = 0; i < size; ++i)
            data[i] = char((i * 31 + 7) & 0xFF);   // not compressible into a coincidence
        f.write(data);
        return path;
    }

    static QByteArray read(const QString& path)
    {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }

    void launch(ProviderProcess& p, const QStringList& extra)
    {
        QStringList args{ QStringLiteral(GAMEHQ_SHARE_SAMPLE_PATH), "--pipe", m_pipe,
                          "--dest-dir", m_dest };
        args += extra;
        p.process.setProcessChannelMode(QProcess::SeparateChannels);
#ifdef Q_OS_WIN
        // A console Python must not flash a window during a test run.
        p.process.setCreateProcessArgumentsModifier(
            [](QProcess::CreateProcessArguments* args) { args->flags |= CREATE_NO_WINDOW; });
#endif
        p.process.start(m_python, args);
        QVERIFY2(p.process.waitForStarted(10000), "could not start the sample provider");
    }

    // Waits for the process to end while keeping the host (this thread) responsive.
    bool waitExited(ProviderProcess& p, int ms)
    {
        const bool ended = QTest::qWaitFor([&] { return p.process.state() == QProcess::NotRunning; }, ms);
        if (ended)
            p.process.waitForFinished(100);   // collect the exit code
        return ended;
    }

    // Registered means the process started, connected and completed the handshake.
    bool waitRegistered(Service& s, const QString& id, int ms = 15000)
    {
        return QTest::qWaitFor([&] { return s.registry()->find(id) != nullptr; }, ms);
    }

    // Opens a capture and shares it to `targetId`; returns once the job started.
    bool startShare(Service& s, const QString& id, const QString& file, const QString& targetId)
    {
        if (!s.open(file) || !s.requestTargets(id))
            return false;
        if (!QTest::qWaitFor([&] { return !s.targetsLoading(); }, 10000))
            return false;
        return !s.share(id, targetId).isEmpty();
    }

private slots:
    void initTestCase()
    {
        m_python = qEnvironmentVariable("GAMEHQ_SHARE_PYTHON");
        if (m_python.isEmpty())
            m_python = QStandardPaths::findExecutable("python");
        if (m_python.isEmpty())
            m_python = QStandardPaths::findExecutable("py");
        if (m_python.isEmpty())
            QSKIP("Python 3 is not available; the out-of-process conformance suite needs it.");
        QVERIFY(QFileInfo::exists(GAMEHQ_SHARE_SAMPLE_PATH));
    }

    void init()
    {
        m_pipe = "GameHQ.Share.Provider.Conformance." + QUuid::createUuid().toString(QUuid::Id128);
        m_dest = m_dir.filePath("dest-" + QUuid::createUuid().toString(QUuid::Id128).left(8));
    }

    // ── The whole useful lifecycle, across a real process boundary ─────
    void fullLifecycleAndCleanExit()
    {
        Service s;
        ExternalProviderHost host(s.registry());
        QString err;
        QVERIFY(host.start(err, m_pipe));
        ProviderProcess sample;
        launch(sample, { "--exit-after-jobs", "1" });
        QVERIFY2(waitRegistered(s, "ext.sample.folder"), qPrintable(sample.output()));

        // Manifest, as the user would see it in Share.
        const QString file = capture("shot.png");
        const QString neighbour = capture("neighbour.png", 2048);   // must never be touched
        QVERIFY(s.open(file));
        const QVariantMap row = s.providers().first().toMap();
        QCOMPARE(row.value("id").toString(), QStringLiteral("ext.sample.folder"));
        QCOMPARE(row.value("name").toString(), QStringLiteral("Sample folder"));
        QCOMPARE(row.value("access").toString(), QStringLiteral("none"));
        QVERIFY(row.value("capabilities").toStringList().contains("target-search"));

        // Target listing and search.
        QVERIFY(s.requestTargets("ext.sample.folder"));
        QTRY_VERIFY_WITH_TIMEOUT(!s.targetsLoading(), 10000);
        QCOMPARE(s.targets().size(), 2);
        QVERIFY(s.requestTargets("ext.sample.folder", "arch"));
        QTRY_VERIFY_WITH_TIMEOUT(!s.targetsLoading(), 10000);
        QCOMPARE(s.targets().size(), 1);
        QCOMPARE(s.targets().first().toMap().value("id").toString(), QStringLiteral("archive"));

        // Share, watching progress arrive from the other process.
        QList<double> progress;
        Provider* provider = s.registry()->find("ext.sample.folder");
        connect(provider, &Provider::jobProgress, this,
                [&](const QString&, JobState state, double p) {
                    if (state == JobState::Transferring)
                        progress.append(p);
                });
        QSignalSpy done(&s, &Service::finished);
        QVERIFY(!s.share("ext.sample.folder", "archive").isEmpty());
        QVERIFY2(done.wait(15000), qPrintable(sample.output()));
        QCOMPARE(s.lastResult().value("outcome").toString(), QStringLiteral("copied"));
        QVERIFY(!progress.isEmpty());
        QVERIFY(progress.last() > 0.99 && progress.last() <= 1.0);

        // The exact capture arrived; nothing else from its folder did.
        QCOMPARE(read(m_dest + "/archive/shot.png"), read(file));
        int copiedFiles = 0;
        QDirIterator it(m_dest, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            ++copiedFiles;
            QVERIFY2(it.fileName() != QFileInfo(neighbour).fileName(),
                     "the sample copied a file it was never given");
        }
        QCOMPARE(copiedFiles, 1);

        // A clean goodbye: the process exits 0 and the destination disappears.
        QVERIFY2(waitExited(sample, 10000), "sample did not exit");
        QCOMPARE(sample.process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(sample.process.exitCode(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(!s.registry()->find("ext.sample.folder"), 5000);
        QCOMPARE(host.connectionCount(), 0);
    }

    void resultAndGoodbyeArrivingInOneWrite()
    {
        Service s;
        ExternalProviderHost host(s.registry());
        QString err;
        QVERIFY(host.start(err, m_pipe));
        ProviderProcess sample;
        launch(sample, { "--exit-after-jobs", "1", "--coalesce-goodbye" });
        QVERIFY2(waitRegistered(s, "ext.sample.folder"), qPrintable(sample.output()));
        QSignalSpy done(&s, &Service::finished);
        QVERIFY(startShare(s, "ext.sample.folder", capture("c.png", 4096), "inbox"));
        QVERIFY2(done.wait(15000), qPrintable(sample.output()));
        // Two frames in one OS read: the job result must be honoured before the goodbye.
        QCOMPARE(s.lastResult().value("outcome").toString(), QStringLiteral("copied"));
        QTRY_VERIFY_WITH_TIMEOUT(!s.registry()->find("ext.sample.folder"), 5000);
        QVERIFY(waitExited(sample, 10000));
        QCOMPARE(sample.process.exitCode(), 0);
    }

    // ── Behaviour only a real process can prove ─────────────────────────
    void cancellationCrossesTheProcessBoundary()
    {
        Service s;
        ExternalProviderHost host(s.registry());
        QString err;
        QVERIFY(host.start(err, m_pipe));
        ProviderProcess sample;
        launch(sample, { "--job-chunk-delay-ms", "100" });   // 4 MiB at 64 KiB per 100 ms: ~6 s
        QVERIFY2(waitRegistered(s, "ext.sample.folder"), qPrintable(sample.output()));

        bool progressSeen = false;
        connect(s.registry()->find("ext.sample.folder"), &Provider::jobProgress, this,
                [&](const QString&, JobState state, double p) {
                    if (state == JobState::Transferring && p > 0.0)
                        progressSeen = true;
                });
        QVERIFY(startShare(s, "ext.sample.folder", capture("big.mp4", 4 * 1024 * 1024), "inbox"));
        QTRY_VERIFY_WITH_TIMEOUT(progressSeen, 10000);

        QSignalSpy done(&s, &Service::finished);
        s.cancel();
        QVERIFY(s.busy());   // GameHQ waits for the provider's own verdict
        QVERIFY2(done.wait(10000), qPrintable(sample.output()));
        QCOMPARE(s.lastResult().value("outcome").toString(), QStringLiteral("cancelled"));
        // The provider stopped and cleaned up its half-written copy.
        QVERIFY(!QFileInfo::exists(m_dest + "/inbox/big.mp4"));
        // ...and is still a working destination.
        QVERIFY(s.registry()->find("ext.sample.folder"));
        QCOMPARE(sample.process.state(), QProcess::Running);
    }

    void aProviderCrashMidJobIsUnconfirmedAndGameHQCarriesOn()
    {
        Service s;
        ExternalProviderHost host(s.registry());
        QString err;
        QVERIFY(host.start(err, m_pipe));
        ProviderProcess crashing;
        launch(crashing, { "--crash-on-job" });
        QVERIFY2(waitRegistered(s, "ext.sample.folder"), qPrintable(crashing.output()));

        QSignalSpy done(&s, &Service::finished);
        QVERIFY(startShare(s, "ext.sample.folder", capture("x.png", 4096), "inbox"));
        QVERIFY2(done.wait(15000), qPrintable(crashing.output()));
        QCOMPARE(s.lastResult().value("outcome").toString(), QStringLiteral("unconfirmed"));
        QCOMPARE(s.lastResult().value("errorCode").toString(), QStringLiteral("provider_disconnected"));
        QVERIFY(waitExited(crashing, 10000));
        QCOMPARE(crashing.process.exitCode(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(!s.registry()->find("ext.sample.folder"), 5000);
        QVERIFY(!s.busy());

        // The identity is free again and a healthy provider works normally.
        ProviderProcess healthy;
        launch(healthy, {});
        QVERIFY2(waitRegistered(s, "ext.sample.folder"), qPrintable(healthy.output()));
        QSignalSpy done2(&s, &Service::finished);
        QVERIFY(startShare(s, "ext.sample.folder", capture("y.png", 4096), "inbox"));
        QVERIFY2(done2.wait(15000), qPrintable(healthy.output()));
        QCOMPARE(s.lastResult().value("outcome").toString(), QStringLiteral("copied"));
    }

    void aSilentProviderTimesOutAsUnconfirmed()
    {
        Service s;
        ExternalProviderHost host(s.registry());
        QString err;
        QVERIFY(host.start(err, m_pipe));
        ProviderProcess sample;
        launch(sample, { "--silent-job", "--job-timeout-ms", "1000" });
        QVERIFY2(waitRegistered(s, "ext.sample.folder"), qPrintable(sample.output()));
        QSignalSpy done(&s, &Service::finished);
        QVERIFY(startShare(s, "ext.sample.folder", capture("z.png", 4096), "inbox"));
        QVERIFY(done.wait(10000));
        QCOMPARE(s.lastResult().value("outcome").toString(), QStringLiteral("unconfirmed"));
        QCOMPARE(s.lastResult().value("errorCode").toString(), QStringLiteral("timeout"));
    }

    void aLateHelloMissesTheHandshakeWindow()
    {
        Service s;
        ExternalProviderHost host(s.registry());
        host.setHandshakeTimeoutMs(300);
        QString err;
        QVERIFY(host.start(err, m_pipe));
        ProviderProcess sample;
        launch(sample, { "--hello-delay-ms", "1500" });
        QVERIFY2(waitExited(sample, 15000), "sample should give up");
        QCOMPARE(sample.process.exitCode(), 5);   // connection lost
        QVERIFY(s.registry()->providers().isEmpty());
        QTRY_COMPARE(host.connectionCount(), 0);
    }

    void aProviderReconnectsAfterGameHQRestarts()
    {
        Service s;
        ExternalProviderHost host(s.registry());
        QString err;
        QVERIFY(host.start(err, m_pipe));
        ProviderProcess sample;
        launch(sample, { "--reconnect" });
        QVERIFY2(waitRegistered(s, "ext.sample.folder"), qPrintable(sample.output()));

        host.stop();   // GameHQ goes away
        QVERIFY(!s.registry()->find("ext.sample.folder"));
        QTest::qWait(500);
        QCOMPARE(sample.process.state(), QProcess::Running);   // the provider stays up

        QVERIFY(host.start(err, m_pipe));   // GameHQ comes back
        QVERIFY2(waitRegistered(s, "ext.sample.folder", 20000), qPrintable(sample.output()));

        QSignalSpy done(&s, &Service::finished);
        QVERIFY(startShare(s, "ext.sample.folder", capture("r.png", 4096), "inbox"));
        QVERIFY2(done.wait(15000), qPrintable(sample.output()));
        QCOMPARE(s.lastResult().value("outcome").toString(), QStringLiteral("copied"));
    }

    void withoutReconnectAProviderExitsWhenGameHQIsNotListening()
    {
        ProviderProcess sample;
        launch(sample, { "--connect-timeout", "0.5" });
        QVERIFY(waitExited(sample, 10000));
        QCOMPARE(sample.process.exitCode(), 4);   // "are Share add-ons enabled?"
    }

    // ── Framing across real OS reads and writes ─────────────────────────
    void framesSplitIntoTinyWritesAreReassembled()
    {
        Service s;
        ExternalProviderHost host(s.registry());
        QString err;
        QVERIFY(host.start(err, m_pipe));
        ProviderProcess sample;
        launch(sample, { "--fragment-writes" });   // every frame arrives in 1-7 byte pieces
        QVERIFY2(waitRegistered(s, "ext.sample.folder"), qPrintable(sample.output()));
        QSignalSpy done(&s, &Service::finished);
        QVERIFY(startShare(s, "ext.sample.folder", capture("f.png", 8192), "archive"));
        QVERIFY2(done.wait(20000), qPrintable(sample.output()));
        QCOMPARE(s.lastResult().value("outcome").toString(), QStringLiteral("copied"));
    }

    void aLargeFrameSpanningManyReadsIsHandledAndBounded()
    {
        Service s;
        ExternalProviderHost host(s.registry());
        QString err;
        QVERIFY(host.start(err, m_pipe));
        ProviderProcess sample;
        launch(sample, { "--many-targets", "400" });   // a ~25 KB targets.result
        QVERIFY2(waitRegistered(s, "ext.sample.folder"), qPrintable(sample.output()));
        QVERIFY(s.open(capture("l.png", 4096)));
        QVERIFY(s.requestTargets("ext.sample.folder"));
        QTRY_VERIFY_WITH_TIMEOUT(!s.targetsLoading(), 10000);
        QCOMPARE(s.targets().size(), kMaxTargets);   // bounded by GameHQ, not by the provider
        QCOMPARE(sample.process.state(), QProcess::Running);
    }

    // ── Version and forward-compatibility rules ─────────────────────────
    void compatibilityMatrix_data()
    {
        QTest::addColumn<QStringList>("args");
        QTest::addColumn<bool>("registered");
        QTest::addColumn<int>("exitCode");   // -1: still running
        QTest::newRow("current v1")                << QStringList{ "--protocol-min", "1", "--protocol-max", "1" } << true << -1;
        QTest::newRow("range that includes v1")    << QStringList{ "--protocol-min", "1", "--protocol-max", "3" } << true << -1;
        QTest::newRow("newer provider, overlaps")  << QStringList{ "--protocol-min", "1", "--protocol-max", "9" } << true << -1;
        QTest::newRow("only newer versions")       << QStringList{ "--protocol-min", "2", "--protocol-max", "3" } << false << 3;
        QTest::newRow("inverted range")            << QStringList{ "--protocol-min", "3", "--protocol-max", "1" } << false << 2;
        QTest::newRow("unknown capabilities")      << QStringList{ "--extra-capability", "teleport", "--extra-capability", "requires_account" } << true << -1;
        QTest::newRow("unknown optional fields")   << QStringList{ "--extra-fields" } << true << -1;
        QTest::newRow("unknown message type")      << QStringList{ "--send-unknown-type" } << true << -1;
    }

    void compatibilityMatrix()
    {
        QFETCH(QStringList, args);
        QFETCH(bool, registered);
        QFETCH(int, exitCode);
        Service s;
        ExternalProviderHost host(s.registry());
        QString err;
        QVERIFY(host.start(err, m_pipe));
        ProviderProcess sample;
        launch(sample, args);

        if (!registered) {
            QVERIFY2(waitExited(sample, 15000), qPrintable(sample.output()));
            QCOMPARE(sample.process.exitCode(), exitCode);
            QVERIFY(s.registry()->providers().isEmpty());
            return;
        }
        QVERIFY2(waitRegistered(s, "ext.sample.folder"), qPrintable(sample.output()));
        Provider* p = s.registry()->find("ext.sample.folder");
        // Reserved and unknown capabilities never widen what the provider is.
        QVERIFY(!p->capabilities().testFlag(Capability::RequiresAccount));
        QVERIFY(p->capabilities().testFlag(Capability::Image));

        // Whatever was ignored, the provider still works end to end.
        QSignalSpy done(&s, &Service::finished);
        QVERIFY(startShare(s, "ext.sample.folder", capture("m.png", 4096), "inbox"));
        QVERIFY2(done.wait(15000), qPrintable(sample.output()));
        QCOMPARE(s.lastResult().value("outcome").toString(), QStringLiteral("copied"));
        QCOMPARE(sample.process.state(), QProcess::Running);   // a stray message did not drop it
    }

    // ── The sample obeys its own rules ──────────────────────────────────
    void theSampleNeverBrowsesBesideTheCapture()
    {
        // The selected path is the only thing a provider is given. Nothing in
        // the sample may enumerate, glob or walk a directory.
        const QString source = QString::fromUtf8(read(GAMEHQ_SHARE_SAMPLE_PATH));
        QVERIFY(!source.isEmpty());
        for (const char* forbidden : { "listdir", "scandir", "os.walk", "glob", "iterdir", "dirname" })
            QVERIFY2(!source.contains(QLatin1String(forbidden)), forbidden);
        // Dependency-free: standard library only.
        QVERIFY(!source.contains("import requests"));
        QVERIFY(!source.contains("pip install"));
    }
};

QTEST_MAIN(TestShareExternalConformance)
#include "tst_shareexternalconformance.moc"
