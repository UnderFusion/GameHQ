#include "share/ShareProvider.h"
#include "share/ShareProviderRegistry.h"
#include "share/ShareService.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

using namespace share;

// Share Platform core (t9): the rules every provider inherits. The fake
// provider is deliberately misbehaving where a real one might be (late
// results, Sent without direct-send, silence) so the service's guarantees are
// what is under test: right file, right target, no false Sent, no duplicate.
class FakeProvider : public Provider
{
    Q_OBJECT
public:
    FakeProvider(QString id, Capabilities caps, QObject* parent = nullptr)
        : Provider(parent), m_id(std::move(id)), m_caps(caps) {}

    QString id() const override { return m_id; }
    QString displayName() const override { return m_id; }
    Capabilities capabilities() const override { return m_caps; }
    AuthState authState() const override { return auth; }
    Availability availability() const override { return availabilityValue; }
    int jobTimeoutMs() const override { return timeoutMs; }

    void requestTargets(const QString& queryId, const Request&, const QString&) override
    {
        lastQueryId = queryId;
        if (answerTargets)
            emit targetsReady(queryId, targets, {});
    }
    void start(const Job& job, const Request& request, const Target& target) override
    {
        startedJob = job;
        startedFile = request.filePath();
        startedTarget = target.id;
        ++starts;
        if (finishWith) {
            Result r;
            r.jobId = job.id;
            r.outcome = *finishWith;
            r.errorCode = errorCode;
            emit jobFinished(r);
        }
    }
    void cancel(const QString& jobId) override { cancelled.append(jobId); }

    void finish(Outcome outcome, const QString& jobId = QString())
    {
        Result r;
        r.jobId = jobId.isEmpty() ? startedJob.id : jobId;
        r.outcome = outcome;
        emit jobFinished(r);
    }

    QString m_id;
    Capabilities m_caps;
    AuthState auth = AuthState::NotRequired;
    Availability availabilityValue = Availability::Available;
    int timeoutMs = 120000;
    bool answerTargets = true;
    std::optional<Outcome> finishWith;
    QString errorCode;
    QVector<Target> targets;
    QString lastQueryId;
    Job startedJob;
    QString startedFile;
    QString startedTarget;
    int starts = 0;
    QStringList cancelled;
};

class TestShareCore : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;

    QString writeFile(const QString& name, const QByteArray& bytes = "data")
    {
        const QString path = m_dir.filePath(name);
        QFile f(path);
        f.open(QIODevice::WriteOnly);
        f.write(bytes);
        return path;
    }

    static Target target(const QString& id, Capabilities caps = {})
    {
        Target t;
        t.id = id;
        t.kind = TargetKind::Contact;
        t.displayName = id;
        t.capabilities = caps;
        return t;
    }

    // Opens a session for `file` and loads the provider's targets.
    static void ready(Service& s, FakeProvider& p, const QString& file)
    {
        QVERIFY(s.open(file));
        QVERIFY(s.requestTargets(p.id()));
    }

private slots:
    void requestValidation()
    {
        Request::Error e;
        QVERIFY(!Request::fromCapture(QString(), {}, &e).isValid());
        QCOMPARE(e, Request::Error::EmptyPath);
        QVERIFY(!Request::fromCapture(m_dir.filePath("nope.png"), {}, &e).isValid());
        QCOMPARE(e, Request::Error::NotFound);
        QVERIFY(!Request::fromCapture(m_dir.path(), {}, &e).isValid());
        QCOMPARE(e, Request::Error::NotAFile);
        QVERIFY(!Request::fromCapture(writeFile("a.txt"), {}, &e).isValid());
        QCOMPARE(e, Request::Error::UnsupportedType);
        QVERIFY(!Request::fromCapture(writeFile("empty.png", QByteArray()), {}, &e).isValid());
        QCOMPARE(e, Request::Error::Empty);

        const Request img = Request::fromCapture(writeFile("shot.JPG"), QStringLiteral("Game"), &e);
        QVERIFY(img.isValid());
        QCOMPARE(img.mediaKind(), MediaKind::Image);
        QCOMPARE(img.mimeType(), QStringLiteral("image/jpeg"));
        const Request clip = Request::fromCapture(writeFile("clip.mp4"), {}, &e);
        QCOMPARE(clip.mediaKind(), MediaKind::Video);
        QCOMPARE(clip.requiredCapability(), Capability::Video);
    }

    void registryRejectsBadAndDuplicateIds()
    {
        ProviderRegistry reg;
        FakeProvider bad(QStringLiteral("Bad Id"), Capability::Image);
        FakeProvider a(QStringLiteral("telegram.desktop"), Capability::Image);
        FakeProvider dup(QStringLiteral("telegram.desktop"), Capability::Video);
        QCOMPARE(reg.add(nullptr), ProviderRegistry::AddError::NullProvider);
        QCOMPARE(reg.add(&bad), ProviderRegistry::AddError::InvalidId);
        QCOMPARE(reg.add(&a), ProviderRegistry::AddError::None);
        QCOMPARE(reg.add(&dup), ProviderRegistry::AddError::DuplicateId);
        QCOMPARE(reg.providers().size(), 1);
        {
            FakeProvider temp(QStringLiteral("temp"), Capability::Image);
            QCOMPARE(reg.add(&temp), ProviderRegistry::AddError::None);
            QCOMPARE(reg.providers().size(), 2);
        }
        QCOMPARE(reg.providers().size(), 1);   // destroyed provider drops out
    }

    void providersAreFilteredByMedia()
    {
        Service s;
        FakeProvider images(QStringLiteral("images"), Capability::Image | Capability::ExternalHandoff);
        FakeProvider videos(QStringLiteral("videos"), Capability::Video | Capability::ExternalHandoff);
        s.registry()->add(&images);
        s.registry()->add(&videos);
        QVERIFY(s.open(writeFile("p.png")));
        const QVariantList list = s.providers();
        QCOMPARE(list.size(), 1);
        QCOMPARE(list.first().toMap().value("id").toString(), QStringLiteral("images"));
    }

    void targetsFilterStaleAndWrongMedia()
    {
        Service s;
        FakeProvider p(QStringLiteral("prov"), Capability::Image | Capability::DirectSend);
        p.answerTargets = false;
        s.registry()->add(&p);
        QVERIFY(s.open(writeFile("t.png")));
        QVERIFY(s.requestTargets("prov"));
        const QString staleQuery = p.lastQueryId;
        QVERIFY(s.requestTargets("prov", "second"));
        // The answer to the superseded query must not populate the list.
        emit p.targetsReady(staleQuery, { target("stale") }, {});
        QVERIFY(s.targets().isEmpty());
        QVERIFY(s.targetsLoading());
        emit p.targetsReady(p.lastQueryId,
                            { target("ok", Capability::Image), target("videoOnly", Capability::Video),
                              target("any") }, {});
        QCOMPARE(s.targets().size(), 2);
        QCOMPARE(s.share("prov", "videoOnly"), QString());
        QCOMPARE(s.lastError(), QStringLiteral("unknown_target"));
        QCOMPARE(s.share("prov", "stale"), QString());
        QCOMPARE(p.starts, 0);
    }

    void sentOnlyWithDirectSendAndCorrectFile()
    {
        Service s;
        FakeProvider handoff(QStringLiteral("handoff"), Capability::Image | Capability::ExternalHandoff);
        handoff.targets = { target("app") };
        handoff.finishWith = Outcome::Sent;   // lying provider
        s.registry()->add(&handoff);
        const QString file = writeFile("h.png");
        ready(s, handoff, file);
        QVERIFY(!s.share("handoff", "app").isEmpty());
        QCOMPARE(handoff.startedFile, QFileInfo(file).absoluteFilePath());
        QCOMPARE(handoff.startedTarget, QStringLiteral("app"));
        QCOMPARE(s.lastResult().value("outcome").toString(), QStringLiteral("handed_off"));

        FakeProvider direct(QStringLiteral("direct"), Capability::Image | Capability::DirectSend);
        direct.targets = { target("friend") };
        direct.finishWith = Outcome::Sent;
        s.registry()->add(&direct);
        QVERIFY(s.requestTargets("direct"));
        QVERIFY(!s.share("direct", "friend").isEmpty());
        QCOMPARE(s.lastResult().value("outcome").toString(), QStringLiteral("sent"));
    }

    void duplicateSendNeedsConfirmation()
    {
        Service s;
        FakeProvider p(QStringLiteral("prov"), Capability::Image | Capability::DirectSend);
        p.targets = { target("friend") };
        p.finishWith = Outcome::Sent;
        s.registry()->add(&p);
        ready(s, p, writeFile("d.png"));
        QVERIFY(!s.share("prov", "friend").isEmpty());
        QCOMPARE(s.share("prov", "friend"), QString());
        QCOMPARE(s.lastError(), QStringLiteral("resend_needs_confirmation"));
        QCOMPARE(p.starts, 1);
        QVERIFY(!s.share("prov", "friend", true).isEmpty());
        QCOMPARE(p.starts, 2);
    }

    void busyAndLateResults()
    {
        Service s;
        FakeProvider p(QStringLiteral("prov"), Capability::Image | Capability::DirectSend);
        p.targets = { target("a"), target("b") };
        s.registry()->add(&p);
        ready(s, p, writeFile("b.png"));
        const QString job = s.share("prov", "a");
        QVERIFY(!job.isEmpty());
        QVERIFY(s.busy());
        QCOMPARE(s.share("prov", "b"), QString());
        QCOMPARE(s.lastError(), QStringLiteral("busy"));
        QVERIFY(!s.close());
        p.finish(Outcome::Sent, QStringLiteral("some-other-job"));
        QVERIFY(s.busy());
        p.finish(Outcome::Failed);
        QVERIFY(!s.busy());
        p.finish(Outcome::Sent);   // late duplicate result: ignored
        QCOMPARE(s.lastResult().value("outcome").toString(), QStringLiteral("failed"));
    }

    void changedFileIsNeverSent()
    {
        Service s;
        FakeProvider p(QStringLiteral("prov"), Capability::Image | Capability::DirectSend);
        p.targets = { target("a") };
        s.registry()->add(&p);
        const QString file = writeFile("c.png", "one");
        ready(s, p, file);
        writeFile("c.png", "replaced content");
        QCOMPARE(s.share("prov", "a"), QString());
        QCOMPARE(s.lastError(), QStringLiteral("capture_changed"));
        QCOMPARE(p.starts, 0);
    }

    void accountAndAvailabilityGates()
    {
        Service s;
        FakeProvider p(QStringLiteral("prov"), Capability::Image | Capability::DirectSend
                                             | Capability::RequiresAccount);
        p.targets = { target("a") };
        p.auth = AuthState::Connected;
        s.registry()->add(&p);
        ready(s, p, writeFile("g.png"));
        p.auth = AuthState::Disconnected;
        QCOMPARE(s.share("prov", "a"), QString());
        QCOMPARE(s.lastError(), QStringLiteral("not_connected"));
        p.auth = AuthState::Connected;
        p.availabilityValue = Availability::NotInstalled;
        QCOMPARE(s.share("prov", "a"), QString());
        QCOMPARE(s.lastError(), QStringLiteral("unavailable"));
        QCOMPARE(p.starts, 0);
    }

    void silenceEndsUnconfirmedAndCancelRules()
    {
        Service s;
        FakeProvider p(QStringLiteral("prov"), Capability::Image | Capability::DirectSend);
        p.targets = { target("a") };
        p.timeoutMs = 1000;
        s.registry()->add(&p);
        ready(s, p, writeFile("s.png"));
        QSignalSpy finished(&s, &Service::finished);
        const QString job = s.share("prov", "a");
        emit p.jobProgress(job, JobState::Transferring, 0.5);
        s.cancel();   // data in flight: must wait for the provider
        QVERIFY(s.busy());
        QVERIFY(finished.wait(3000));
        QCOMPARE(s.lastResult().value("outcome").toString(), QStringLiteral("unconfirmed"));
        QCOMPARE(s.lastResult().value("errorCode").toString(), QStringLiteral("timeout"));
        QVERIFY(p.cancelled.contains(job));

        // Nothing left GameHQ yet: cancel ends it immediately.
        const QString second = s.share("prov", "a", true);
        s.cancel();
        QVERIFY(!s.busy());
        QCOMPARE(s.lastResult().value("outcome").toString(), QStringLiteral("cancelled"));
        QVERIFY(p.cancelled.contains(second));
    }

    void errorCodesCannotCarrySecrets()
    {
        Service s;
        FakeProvider p(QStringLiteral("prov"), Capability::Image | Capability::DirectSend);
        p.targets = { target("a") };
        p.finishWith = Outcome::Failed;
        p.errorCode = QStringLiteral("https://discord.com/api/webhooks/123/SECRET");
        s.registry()->add(&p);
        ready(s, p, writeFile("e.png"));
        s.share("prov", "a");
        QCOMPARE(s.lastResult().value("errorCode").toString(), QStringLiteral("provider_error"));
    }

    void providerRemovedMidJob()
    {
        Service s;
        auto p = std::make_unique<FakeProvider>(QStringLiteral("prov"), Capability::Image | Capability::DirectSend);
        p->targets = { target("a") };
        s.registry()->add(p.get());
        ready(s, *p, writeFile("r.png"));
        const QString job = s.share("prov", "a");
        emit p->jobProgress(job, JobState::Transferring, 0.1);
        p.reset();
        QVERIFY(!s.busy());
        QCOMPARE(s.lastResult().value("outcome").toString(), QStringLiteral("unconfirmed"));
        QCOMPARE(s.lastResult().value("errorCode").toString(), QStringLiteral("provider_lost"));
    }
};

QTEST_MAIN(TestShareCore)
#include "tst_sharecore.moc"
