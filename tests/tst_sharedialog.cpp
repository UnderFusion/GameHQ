// Drives the REAL ShareDialog.qml against the real share::Service (t10).
//
// The dialog is pad-first and modal: these cases walk it the way a controller
// does (padNavigate / padConfirm / padBack) and assert what reaches the
// provider - which file, which recipient, how many sends - plus the honest
// wording of the result. No window is shown.

#include "share/ShareProvider.h"
#include "share/ShareService.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QScopedPointer>
#include <QTemporaryDir>
#include <QTest>

using namespace share;

namespace
{
class StubSounds : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    Q_INVOKABLE void play(const QString&) {}
};

class TestProvider : public Provider
{
    Q_OBJECT
public:
    TestProvider(QString id, QString name, Capabilities caps, QVector<Target> targets,
                 QObject* parent = nullptr)
        : Provider(parent), m_id(std::move(id)), m_name(std::move(name)), m_caps(caps),
          m_targets(std::move(targets)) {}

    QString id() const override { return m_id; }
    QString displayName() const override { return m_name; }
    Capabilities capabilities() const override { return m_caps; }
    Availability availability() const override { return available ? Availability::Available
                                                                   : Availability::NotInstalled; }
    QString availabilityReason() const override { return QStringLiteral("Not installed"); }

    void requestTargets(const QString& queryId, const Request&, const QString&) override
    {
        QVector<Target> out = m_targets;
        for (Target& t : out)
            t.providerId = m_id;
        emit targetsReady(queryId, out, {});
    }
    void start(const Job& job, const Request& request, const Target& target) override
    {
        ++starts;
        lastFile = request.filePath();
        lastTarget = target.id;
        lastJob = job.id;
        if (holdJobs)
            return;
        Result r;
        r.jobId = job.id;
        r.outcome = m_caps.testFlag(Capability::DirectSend) ? Outcome::Sent : Outcome::HandedOff;
        emit jobFinished(r);
    }
    void cancel(const QString& jobId) override { cancelled.append(jobId); }

    QString m_id;
    QString m_name;
    Capabilities m_caps;
    QVector<Target> m_targets;
    bool available = true;
    bool holdJobs = false;
    int starts = 0;
    QString lastFile;
    QString lastTarget;
    QString lastJob;
    QStringList cancelled;
};

Target makeTarget(const QString& id, TargetKind kind, const QString& name)
{
    Target t;
    t.id = id;
    t.kind = kind;
    t.displayName = name;
    return t;
}
} // namespace

class ShareDialogTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QQmlEngine m_engine;
    QUrl m_url;
    QString m_capture;
    QScopedPointer<Service> m_service;
    TestProvider* m_handoff = nullptr;
    TestProvider* m_direct = nullptr;
    TestProvider* m_missing = nullptr;
    QScopedPointer<QObject> m_dialog;

    QVariantList rows() const { return m_dialog->property("rows").toList(); }
    QString view() const { return m_dialog->property("view").toString(); }
    void call(const char* method, const QVariant& arg = QVariant())
    {
        if (arg.isValid())
            QVERIFY(QMetaObject::invokeMethod(m_dialog.data(), method, Q_ARG(QVariant, arg)));
        else
            QVERIFY(QMetaObject::invokeMethod(m_dialog.data(), method));
    }
    void openDialog()
    {
        QVariant ok;
        QVERIFY(QMetaObject::invokeMethod(m_dialog.data(), "openFor", Q_RETURN_ARG(QVariant, ok),
                                          Q_ARG(QVariant, m_capture), Q_ARG(QVariant, QString())));
        QVERIFY(ok.toBool());
    }

private slots:
    void initTestCase()
    {
        // Same module mirroring as tst_mappingpresetsection: the build-tree
        // qmldir minus its `prefer` line, with the source QML beside it.
        QFile manifest(QStringLiteral(GAMEHQ_QML_IMPORT_DIR "/GameHQ/qmldir"));
        QVERIFY(manifest.open(QIODevice::ReadOnly | QIODevice::Text));
        QStringList entries;
        while (!manifest.atEnd()) {
            const QString line = QString::fromUtf8(manifest.readLine()).trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1String("prefer ")))
                continue;
            entries.append(line);
        }
        const QString moduleRoot = m_dir.filePath(QStringLiteral("qmltest"));
        const QString moduleDir = moduleRoot + QStringLiteral("/GameHQ");
        QVERIFY(QDir().mkpath(moduleDir));
        const QString sourceRoot = QStringLiteral(GAMEHQ_QML_SOURCE_DIR "/ui");
        QDirIterator it(sourceRoot, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString source = it.next();
            const QString target = moduleDir + QStringLiteral("/ui/")
                                   + QDir(sourceRoot).relativeFilePath(source);
            QVERIFY(QDir().mkpath(QFileInfo(target).absolutePath()));
            QVERIFY(QFile::copy(source, target));
        }
        QFile generated(moduleDir + QStringLiteral("/qmldir"));
        QVERIFY(generated.open(QIODevice::WriteOnly | QIODevice::Text));
        generated.write(entries.join(QLatin1Char('\n')).toUtf8() + "\n");
        generated.close();
        m_engine.addImportPath(moduleRoot);
        m_url = QUrl::fromLocalFile(moduleDir + QStringLiteral("/ui/qml/components/ShareDialog.qml"));

        m_capture = m_dir.filePath(QStringLiteral("shot.png"));
        QFile f(m_capture);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("png-bytes");
    }

    void init()
    {
        m_dialog.reset();
        m_service.reset(new Service);
        m_missing = new TestProvider(QStringLiteral("missing"), QStringLiteral("Missing app"),
                                     Capability::Image | Capability::ExternalHandoff,
                                     { makeTarget("app", TargetKind::External, "Missing app") },
                                     m_service.data());
        m_missing->available = false;
        m_handoff = new TestProvider(QStringLiteral("handoff"), QStringLiteral("Desktop app"),
                                     Capability::Image | Capability::ExternalHandoff,
                                     { makeTarget("app", TargetKind::External, "Desktop app") },
                                     m_service.data());
        m_direct = new TestProvider(QStringLiteral("direct"), QStringLiteral("Messenger"),
                                    Capability::Image | Capability::DirectSend | Capability::Contacts,
                                    { makeTarget("alice", TargetKind::Contact, "Alice"),
                                      makeTarget("bob", TargetKind::Contact, "Bob") },
                                    m_service.data());
        // Unavailable first: the dialog must land on the first usable row.
        QCOMPARE(m_service->registry()->add(m_missing), ProviderRegistry::AddError::None);
        QCOMPARE(m_service->registry()->add(m_handoff), ProviderRegistry::AddError::None);
        QCOMPARE(m_service->registry()->add(m_direct), ProviderRegistry::AddError::None);

        m_engine.rootContext()->setContextProperty(QStringLiteral("shareService"), m_service.data());
        m_engine.rootContext()->setContextProperty(QStringLiteral("sounds"),
                                                   new StubSounds(m_service.data()));
        QQmlComponent component(&m_engine, m_url);
        m_dialog.reset(component.create());
        QVERIFY2(m_dialog, qPrintable(component.errorString()));
    }

    void cleanup()
    {
        m_dialog.reset();
        m_service.reset();
    }

    void destinationsSkipUnavailableRows()
    {
        openDialog();
        QCOMPARE(view(), QStringLiteral("destinations"));
        QCOMPARE(rows().size(), 3);
        QCOMPARE(rows().at(0).toMap().value("enabled").toBool(), false);
        QCOMPARE(m_dialog->property("currentIndex").toInt(), 1);
        call("padNavigate", -1);   // cannot land on the unavailable row
        QCOMPARE(m_dialog->property("currentIndex").toInt(), 1);
    }

    void handoffIsOneStepAndNeverSent()
    {
        openDialog();
        call("padConfirm");        // "Desktop app": single external target
        QCOMPARE(m_handoff->starts, 1);
        QCOMPARE(m_handoff->lastFile, QFileInfo(m_capture).absoluteFilePath());
        QCOMPARE(view(), QStringLiteral("result"));
        QCOMPARE(m_service->lastResult().value("outcome").toString(), QStringLiteral("handed_off"));
        // No catalog is loaded here, so qsTrId() yields the message id: enough
        // to prove the hand-off wording was chosen, not the "sent" one.
        QVERIFY(m_dialog->property("message").toString().contains(QStringLiteral("outcome.handed_off")));
    }

    void padPicksTheHighlightedRecipient()
    {
        openDialog();
        call("padNavigate", 1);    // -> Messenger
        call("padConfirm");
        QCOMPARE(view(), QStringLiteral("targets"));
        QCOMPARE(rows().size(), 2);
        call("padNavigate", 1);    // -> Bob
        call("padConfirm");
        QCOMPARE(m_direct->starts, 1);
        QCOMPARE(m_direct->lastTarget, QStringLiteral("bob"));
        QCOMPARE(m_service->lastResult().value("outcome").toString(), QStringLiteral("sent"));
    }

    void resendLandsOnCancel()
    {
        openDialog();
        call("padNavigate", 1);
        call("padConfirm");
        call("padConfirm");        // Alice, sent
        QCOMPARE(m_direct->starts, 1);
        call("padConfirm");        // Done
        QVERIFY(!m_dialog->property("isOpen").toBool());

        openDialog();
        call("padNavigate", 1);
        call("padConfirm");
        call("padConfirm");        // Alice again
        QCOMPARE(view(), QStringLiteral("resend"));
        QCOMPARE(m_dialog->property("currentIndex").toInt(), 1);
        call("padConfirm");        // Cancel is the landing row: nothing sent
        QCOMPARE(m_direct->starts, 1);
        QCOMPARE(view(), QStringLiteral("destinations"));
    }

    void circleWhileSendingCancelsOnly()
    {
        m_direct->holdJobs = true;
        openDialog();
        call("padNavigate", 1);
        call("padConfirm");
        QCOMPARE(view(), QStringLiteral("targets"));
        call("padConfirm");
        QCOMPARE(view(), QStringLiteral("sending"));
        call("padBack");           // asks to cancel; does not close under a live job
        QVERIFY(m_direct->cancelled.contains(m_direct->lastJob));
        QVERIFY(m_dialog->property("isOpen").toBool());
        QCOMPARE(view(), QStringLiteral("result"));
        QCOMPARE(m_service->lastResult().value("outcome").toString(), QStringLiteral("cancelled"));
    }

    void circleBacksOutThenCloses()
    {
        openDialog();
        call("padNavigate", 1);
        call("padConfirm");
        QCOMPARE(view(), QStringLiteral("targets"));
        call("padBack");
        QCOMPARE(view(), QStringLiteral("destinations"));
        call("padBack");
        QVERIFY(!m_dialog->property("isOpen").toBool());
        QVERIFY(!m_service->active());
    }
};

QTEST_MAIN(ShareDialogTest)
#include "tst_sharedialog.moc"
