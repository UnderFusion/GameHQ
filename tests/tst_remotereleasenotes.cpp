#include "updates/RemoteReleaseNotes.h"
#include "updates/ReleaseCatalog.h"
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include <cstring>

struct Answer { QByteArray bytes; int status = 200; int delay = 0; };
class Reply final : public QNetworkReply
{
public:
    Reply(const QNetworkRequest &request, Answer answer, QObject *parent)
        : QNetworkReply(parent), m_bytes(std::move(answer.bytes))
    {
        setRequest(request);
        setUrl(request.url());
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, answer.status);
        if (answer.status != 200)
            setError(ContentNotFoundError, "unavailable");
        open(QIODevice::ReadOnly);
        QTimer::singleShot(answer.delay, this, [this] {
            if (m_done) return;
            Q_EMIT readyRead();
            if (m_done) return;
            m_done = true;
            setFinished(true);
            Q_EMIT finished();
        });
    }
    void abort() override {
        if (m_done) return;
        m_done = true;
        setError(OperationCanceledError, "aborted");
        setFinished(true);
        Q_EMIT finished();
    }
    qint64 bytesAvailable() const override { return m_bytes.size() - m_offset; }
protected:
    qint64 readData(char *target, qint64 maximum) override {
        const qint64 count = qMin(maximum, bytesAvailable());
        if (!count) return -1;
        std::memcpy(target, m_bytes.constData() + m_offset, size_t(count));
        m_offset += count;
        return count;
    }
private:
    QByteArray m_bytes;
    qint64 m_offset = 0;
    bool m_done = false;
};
class Network final : public QNetworkAccessManager
{
public:
    QList<Answer> answers;
    QList<QUrl> urls;
protected:
    QNetworkReply *createRequest(Operation, const QNetworkRequest &request, QIODevice *) override {
        urls.append(request.url());
        return new Reply(request, answers.isEmpty() ? Answer{{},404} : answers.takeFirst(), this);
    }
};
static QByteArray document(const QByteArray &version, const QByteArray &text)
{ return "# GameHQ " + version + " (2026-09-14)\n\n## Fixed\n\n- " + text + "\n"; }
class RemoteReleaseNotesTest : public QObject
{
    Q_OBJECT
private slots:
    void persistsOnlyOfferableDiscoverySnapshots() {
        ReleaseInfo release;
        release.version = "0.7.7"; release.tag = "v0.7.7"; release.name = "GameHQ 0.7.7";
        release.notes = "Polish publication remains associated with this release";
        release.zipName = "GameHQ-0.7.7-win64-update.zip"; release.zipSize = 123;
        release.zipUrl = "https://github.com/underfusion/GameHQ/releases/download/v0.7.7/" + release.zipName;
        release.manifestUrl = "https://github.com/underfusion/GameHQ/releases/download/v0.7.7/gamehq-release.json";
        release.signatureUrl = "https://github.com/underfusion/GameHQ/releases/download/v0.7.7/gamehq-release.sig";
        release.webUrl = "https://github.com/underfusion/GameHQ/releases/tag/v0.7.7";
        auto restored = ReleaseCatalog::restoreSnapshot(ReleaseCatalog::cacheSnapshot(release));
        QVERIFY(restored.has_value());
        QCOMPARE(restored->version, release.version); QCOMPARE(restored->tag, release.tag);
        QCOMPARE(restored->notes, release.notes); QCOMPARE(restored->webUrl, release.webUrl);
        release.draft = true;
        QVERIFY(!ReleaseCatalog::restoreSnapshot(ReleaseCatalog::cacheSnapshot(release)));
        release.draft = false; release.signatureUrl.clear();
        QVERIFY(!ReleaseCatalog::restoreSnapshot(ReleaseCatalog::cacheSnapshot(release)));
        QVERIFY(!ReleaseCatalog::restoreSnapshot("not json"));
        QVERIFY(!ReleaseCatalog::restoreSnapshot(QByteArray(1024 * 1024 + 1, 'x')));
    }
    void bodyThenLocalizedAndSafeFormatting() {
        QTemporaryDir dir(QDir::tempPath() + "/gamehq-notes-test-XXXXXX");
        QVERIFY(dir.isValid());
        Network network;
        network.answers = {{document("0.7.7", "**Fix:** [details](https://example.com) <img src=x>")}};
        RemoteReleaseNotes notes("underfusion", "GameHQ", dir.path(), nullptr, &network);
        notes.select("0.7.7", "v0.7.7", "## English\n- Immediate notes\n<!-- gamehq:locale-index -->\nweb table", "pl-PL");
        QVERIFY(notes.markdown().contains("Immediate notes"));
        QVERIFY(!notes.markdown().contains("web table"));
        QTRY_VERIFY(!notes.loading());
        QCOMPARE(network.urls.first().toString(), QString("https://raw.githubusercontent.com/underfusion/GameHQ/v0.7.7/assets/release-notes/publication/0.7.7/release-notes.pl-PL.md"));
        QVERIFY(notes.markdown().contains("**Fix:**"));
        const auto block = notes.blocks().last().toMap();
        QCOMPARE(block.value("kind").toString(), QString("bullet"));
        // Raw HTML remains inert text; QML's escape boundary is tested in tst_updatenotes.cjs.
        QVERIFY(block.value("text").toString().contains("<img"));
        QVERIFY(!block.value("text").toString().contains("https://"));
    }
    void fallsBackAndRestoresCacheOffline() {
        QTemporaryDir dir(QDir::tempPath() + "/gamehq-notes-test-XXXXXX");
        Network network;
        network.answers = {{{},404}, {document("0.7.7", "English publication")}};
        RemoteReleaseNotes notes("underfusion", "GameHQ", dir.path(), nullptr, &network);
        notes.select("0.7.7", "v0.7.7", "Discovery body", "pl-PL");
        QTRY_VERIFY(!notes.loading());
        QCOMPARE(network.urls.size(), 2);
        QVERIFY(notes.markdown().contains("English publication"));
        Network offline;
        RemoteReleaseNotes restored("underfusion", "GameHQ", dir.path(), nullptr, &offline);
        restored.select("0.7.7", "v0.7.7", "", "pl-PL");
        QVERIFY(restored.markdown().contains("English publication"));
        QTRY_VERIFY(!restored.loading());
        QVERIFY(restored.markdown().contains("English publication"));
        restored.select("0.7.8", "v0.7.8", "New version", "pl-PL");
        QCOMPARE(restored.markdown(), QString("New version"));
        QTRY_VERIFY(!restored.loading());
    }
    void rejectsBadDocumentsAndSupportsRetry() {
        QTemporaryDir dir(QDir::tempPath() + "/gamehq-notes-test-XXXXXX");
        Network network;
        network.answers = {{document("0.7.8", "Wrong version")}, {QByteArray(140000,'x')}, {document("0.7.7","Retried")}};
        RemoteReleaseNotes notes("underfusion", "GameHQ", dir.path(), nullptr, &network);
        notes.select("0.7.7", "v0.7.7", "Known body", "pl-PL");
        QTRY_VERIFY(!notes.loading());
        QCOMPARE(notes.markdown(), QString("Known body"));
        notes.retry();
        QTRY_VERIFY(!notes.loading());
        QVERIFY(notes.markdown().contains("Retried"));
        const int requests = network.urls.size();
        notes.select("0.7.7", "../../main", "", "pl-PL");
        QVERIFY(!notes.loading());
        QCOMPARE(network.urls.size(), requests);
    }
    void staleLanguageAndVersionRepliesCannotReplaceSelection() {
        QTemporaryDir dir(QDir::tempPath() + "/gamehq-notes-test-XXXXXX");
        Network network;
        network.answers = {{document("0.7.7", "Old Polish"),200,80}, {document("0.7.7", "New German")}};
        RemoteReleaseNotes notes("underfusion", "GameHQ", dir.path(), nullptr, &network);
        notes.select("0.7.7", "v0.7.7", "English", "pl-PL");
        notes.select("0.7.7", "v0.7.7", "English", "de-DE");
        QTRY_VERIFY(!notes.loading());
        QTest::qWait(100);
        QVERIFY(notes.markdown().contains("New German"));
        network.answers = {{document("0.7.7", "Stale version"),200,80}, {document("0.7.8", "Current version")}};
        notes.select("0.7.7", "v0.7.7", "English", "pl-PL");
        notes.select("0.7.8", "v0.7.8", "English", "pl-PL");
        QTRY_VERIFY(!notes.loading());
        QTest::qWait(100);
        QVERIFY(notes.markdown().contains("Current version"));
    }
};
QTEST_GUILESS_MAIN(RemoteReleaseNotesTest)
#include "tst_remotereleasenotes.moc"
