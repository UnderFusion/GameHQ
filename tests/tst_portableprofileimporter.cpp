#include <QtTest>

#include "config/PortableProfileImporter.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>

namespace
{
bool writeFile(const QString& path, const QByteArray& bytes)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QByteArray treeDigest(const QString& root)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QDir base(root);
    const QFileInfoList files = base.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                                                   QDir::Name);
    std::function<void(const QString&)> visit = [&](const QString& directory) {
        QDir current(directory);
        for (const QFileInfo& entry : current.entryInfoList(
                 QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            const QByteArray relative = QDir::fromNativeSeparators(base.relativeFilePath(entry.absoluteFilePath())).toUtf8();
            hash.addData(relative);
            if (entry.isDir())
                visit(entry.absoluteFilePath());
            else
                hash.addData(readFile(entry.absoluteFilePath()));
        }
    };
    Q_UNUSED(files);
    visit(root);
    return hash.result();
}

bool createDatabase(const QString& path, int schemaVersion = 3)
{
    const QString connection = QStringLiteral("fixture-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    bool ok = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(path);
        if (db.open()) {
            QSqlQuery q(db);
            ok = q.exec(QStringLiteral("CREATE TABLE games (id INTEGER PRIMARY KEY, executable_path TEXT, icon_path TEXT)"))
                && q.exec(QStringLiteral("CREATE TABLE captures (id INTEGER PRIMARY KEY, file_path TEXT UNIQUE, thumbnail_path TEXT, game_id INTEGER REFERENCES games(id))"))
                && q.exec(QStringLiteral("CREATE TABLE folders (id INTEGER PRIMARY KEY, path TEXT UNIQUE)"))
                && q.exec(QStringLiteral("CREATE TABLE sound_settings (id INTEGER PRIMARY KEY, sound_file TEXT)"))
                && q.exec(QStringLiteral("CREATE TABLE settings (key TEXT PRIMARY KEY, value TEXT)"))
                && q.exec(QStringLiteral("CREATE TABLE binding_overrides (id INTEGER PRIMARY KEY)"))
                && q.exec(QStringLiteral("PRAGMA user_version = %1").arg(schemaVersion));
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connection);
    return ok;
}

bool execDatabase(const QString& path, const QStringList& statements)
{
    const QString connection = QStringLiteral("fixture-write-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    bool ok = true;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(path);
        ok = db.open();
        for (const QString& statement : statements) {
            QSqlQuery q(db);
            ok = ok && q.exec(statement);
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(connection);
    return ok;
}

QVariant scalar(const QString& path, const QString& sql)
{
    const QString connection = QStringLiteral("fixture-read-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    QVariant value;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(path);
        if (db.open()) {
            QSqlQuery q(db);
            if (q.exec(sql) && q.next())
                value = q.value(0);
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connection);
    return value;
}

struct Fixture {
    QTemporaryDir root;
    QString source;
    QString destination;

    Fixture()
    {
        source = root.filePath(QStringLiteral("Portable Łódź"));
        destination = root.filePath(QStringLiteral("Installed profile"));
        QDir().mkpath(source + QStringLiteral("/gamehq-data/sound-packs/custom"));
        writeFile(source + QStringLiteral("/portable.flag"), "portable");
        writeFile(source + QStringLiteral("/GameHQ.exe"), "fixture");
        writeFile(source + QStringLiteral("/Captures/Game A/shot.png"), "capture-bytes");
        writeFile(source + QStringLiteral("/gamehq-data/sound-packs/custom/notify.wav"), "sound-bytes");
        createDatabase(source + QStringLiteral("/gamehq-data/gamehq.db"));
    }

    PortableProfileImporter::Options options(
        PortableProfileImporter::FailurePoint failure = PortableProfileImporter::FailurePoint::None) const
    {
        return { source, destination, failure };
    }
};
}

class TestPortableProfileImporter : public QObject
{
    Q_OBJECT
private slots:
    void importsAndRewritesAuditedState()
    {
        Fixture fixture;
        const QString db = fixture.source + QStringLiteral("/gamehq-data/gamehq.db");
        const QString external = QDir::cleanPath(fixture.root.filePath(QStringLiteral("External/Game.exe")));
        QVERIFY(execDatabase(db, {
            QStringLiteral("INSERT INTO games VALUES (1, '%1', 'portable:/gamehq-data/game-icons/a.png')").arg(external),
            QStringLiteral("INSERT INTO captures VALUES (1, 'portable:/Captures/Game A/shot.png', 'portable:/gamehq-data/thumbnails/a.jpg', 1)"),
            QStringLiteral("INSERT INTO captures VALUES (2, 'portable:/Captures/Game A/missing.png', 'portable:/gamehq-data/thumbnails/missing.jpg', 1)"),
            QStringLiteral("INSERT INTO folders VALUES (1, 'portable:/Captures')"),
            QStringLiteral("INSERT INTO folders VALUES (2, 'portable:/captures')"),
            QStringLiteral("INSERT INTO sound_settings VALUES (1, 'portable:/gamehq-data/sound-packs/custom/notify.wav')"),
            QStringLiteral("INSERT INTO settings VALUES ('internal.icon_format', 'old')")
        }));
        const QJsonObject config {
            { QStringLiteral("storage.screenshots_root"), QStringLiteral("portable:/Captures") },
            { QStringLiteral("storage.clips_root"), QString() },
            { QStringLiteral("internal.capture_root_history"), QJsonArray {
                QStringLiteral("portable:/Captures"), QStringLiteral("portable:/captures") } },
            { QStringLiteral("theme.active_skin"), QStringLiteral("obsidian") }
        };
        QVERIFY(writeFile(fixture.source + QStringLiteral("/gamehq-data/config.json"),
                          QJsonDocument(config).toJson()));
        const QByteArray sourceBefore = treeDigest(fixture.source);

        PortableProfileImporter::Result result;
        QString error;
        QVERIFY2(PortableProfileImporter::importProfile(fixture.options(), result, error), qPrintable(error));
        QCOMPARE(treeDigest(fixture.source), sourceBefore);
        QCOMPARE(result.captures, 2);
        QCOMPARE(result.games, 1);
        QCOMPARE(result.watchedFolders, 1);
        QCOMPARE(result.copiedSounds, 1);

        const QString importedDb = fixture.destination + QStringLiteral("/gamehq.db");
        QCOMPARE(scalar(importedDb, QStringLiteral("SELECT file_path FROM captures WHERE id = 1")).toString(),
                 QDir::cleanPath(fixture.source + QStringLiteral("/Captures/Game A/shot.png")));
        QVERIFY(scalar(importedDb, QStringLiteral("SELECT thumbnail_path FROM captures")).isNull());
        QCOMPARE(scalar(importedDb, QStringLiteral("SELECT executable_path FROM games")).toString(), external);
        QVERIFY(scalar(importedDb, QStringLiteral("SELECT icon_path FROM games")).isNull());
        QCOMPARE(scalar(importedDb, QStringLiteral("SELECT COUNT(*) FROM settings WHERE key='internal.icon_format'")).toInt(), 0);
        QCOMPARE(scalar(importedDb, QStringLiteral("SELECT COUNT(*) FROM folders")).toInt(), 1);
        QCOMPARE(readFile(fixture.destination + QStringLiteral("/sound-packs/custom/notify.wav")), QByteArray("sound-bytes"));
        QVERIFY(QFileInfo(result.evidencePath).isFile());

        const QJsonObject importedConfig = QJsonDocument::fromJson(
            readFile(fixture.destination + QStringLiteral("/config.json"))).object();
        QCOMPARE(importedConfig.value(QStringLiteral("storage.screenshots_root")).toString(),
                 QDir::cleanPath(fixture.source + QStringLiteral("/Captures")));
        QCOMPARE(importedConfig.value(QStringLiteral("internal.capture_root_history")).toArray().size(), 1);
    }

    void rejectsUnknownPortablePathsAndEscapes()
    {
        for (const QJsonObject& config : {
                 QJsonObject{{ QStringLiteral("future.path"), QStringLiteral("portable:/Captures") }},
                 QJsonObject{{ QStringLiteral("future.path"), QStringLiteral("PORTABLE:/Captures") }},
                 QJsonObject{{ QStringLiteral("storage.screenshots_root"), QStringLiteral("portable:/../outside") }} }) {
            Fixture fixture;
            QVERIFY(writeFile(fixture.source + QStringLiteral("/gamehq-data/config.json"),
                              QJsonDocument(config).toJson()));
            const QByteArray before = treeDigest(fixture.source);
            PortableProfileImporter::Result result;
            QString error;
            QVERIFY(!PortableProfileImporter::importProfile(fixture.options(), result, error));
            QVERIFY(error.contains(QStringLiteral("portable"), Qt::CaseInsensitive)
                    || error.contains(QStringLiteral("escape"), Qt::CaseInsensitive));
            QCOMPARE(treeDigest(fixture.source), before);
            QVERIFY(!QFileInfo::exists(fixture.destination));
        }
    }

    void rejectsPopulatedDestinationAndNewerSchema()
    {
        {
            Fixture fixture;
            QVERIFY(writeFile(fixture.destination + QStringLiteral("/unrelated.txt"), "keep"));
            PortableProfileImporter::Result result;
            QString error;
            QVERIFY(!PortableProfileImporter::importProfile(fixture.options(), result, error));
            QCOMPARE(readFile(fixture.destination + QStringLiteral("/unrelated.txt")), QByteArray("keep"));
        }
        {
            Fixture fixture;
            QVERIFY(writeFile(fixture.destination + QStringLiteral("/config.json"),
                              R"({"ui.window_width":1280,"internal.updates.etag":"test"})"));
            QVERIFY(createDatabase(fixture.destination + QStringLiteral("/gamehq.db")));
            QVERIFY(execDatabase(fixture.destination + QStringLiteral("/gamehq.db"),
                                 { QStringLiteral("INSERT INTO settings VALUES ('internal.repairs_v1_done', '1')") }));
            PortableProfileImporter::Result result;
            QString error;
            QVERIFY2(PortableProfileImporter::importProfile(fixture.options(), result, error), qPrintable(error));
        }
        {
            Fixture fixture;
            QVERIFY(writeFile(fixture.destination + QStringLiteral("/config.json"),
                              R"({"theme.active_skin":"light"})"));
            PortableProfileImporter::Result result;
            QString error;
            QVERIFY(!PortableProfileImporter::importProfile(fixture.options(), result, error));
            QVERIFY(error.contains(QStringLiteral("non-default")));
        }
        {
            Fixture fixture;
            QVERIFY(execDatabase(fixture.source + QStringLiteral("/gamehq-data/gamehq.db"),
                                 { QStringLiteral("PRAGMA user_version = 99") }));
            PortableProfileImporter::Result result;
            QString error;
            QVERIFY(!PortableProfileImporter::importProfile(fixture.options(), result, error));
            QVERIFY(error.contains(QStringLiteral("schema")));
        }
    }

    // The destination was called empty from a fixed list of tables and files.
    // Anything it did not name - a sound the user added, a table a newer schema
    // introduced - read as empty and was then overwritten by the import.
    void refusesADestinationHoldingUserDataOutsideTheKnownList()
    {
        {
            // sound-packs was allowed as a directory without ever being opened,
            // and the import writes into that very folder.
            Fixture fixture;
            QVERIFY(writeFile(fixture.destination + QStringLiteral("/sound-packs/mine/alert.wav"),
                              "user-sound"));
            PortableProfileImporter::Result result;
            QString error;
            QVERIFY(!PortableProfileImporter::importProfile(fixture.options(), result, error));
            QVERIFY2(error.contains(QStringLiteral("sound-packs")), qPrintable(error));
            QCOMPARE(readFile(fixture.destination + QStringLiteral("/sound-packs/mine/alert.wav")),
                     QByteArray("user-sound"));
        }
        {
            // A table the fixed list never mentioned. It must fail closed.
            Fixture fixture;
            QVERIFY(QDir().mkpath(fixture.destination));
            QVERIFY(createDatabase(fixture.destination + QStringLiteral("/gamehq.db")));
            QVERIFY(execDatabase(fixture.destination + QStringLiteral("/gamehq.db"), {
                QStringLiteral("CREATE TABLE playlists (id INTEGER PRIMARY KEY, name TEXT)"),
                QStringLiteral("INSERT INTO playlists VALUES (1, 'Best of 2026')") }));
            PortableProfileImporter::Result result;
            QString error;
            QVERIFY(!PortableProfileImporter::importProfile(fixture.options(), result, error));
            QCOMPARE(scalar(fixture.destination + QStringLiteral("/gamehq.db"),
                            QStringLiteral("SELECT name FROM playlists")).toString(),
                     QStringLiteral("Best of 2026"));
        }
        {
            // A settings key that is not one of the internal startup sentinels.
            Fixture fixture;
            QVERIFY(QDir().mkpath(fixture.destination));
            QVERIFY(createDatabase(fixture.destination + QStringLiteral("/gamehq.db")));
            QVERIFY(execDatabase(fixture.destination + QStringLiteral("/gamehq.db"),
                                 { QStringLiteral("INSERT INTO settings VALUES ('user.note', 'keep me')") }));
            PortableProfileImporter::Result result;
            QString error;
            QVERIFY(!PortableProfileImporter::importProfile(fixture.options(), result, error));
            QCOMPARE(scalar(fixture.destination + QStringLiteral("/gamehq.db"),
                            QStringLiteral("SELECT value FROM settings WHERE key='user.note'")).toString(),
                     QStringLiteral("keep me"));
        }
    }

    // The flip side: a destination that only looks used because GameHQ was
    // started once must still import, or the feature is unusable.
    void acceptsADestinationAFirstLaunchLeftBehind()
    {
        Fixture fixture;
        QVERIFY(QDir().mkpath(fixture.destination + QStringLiteral("/sound-packs")));
        QVERIFY(writeFile(fixture.destination + QStringLiteral("/logs/gamehq.log"), "log line"));
        QVERIFY(writeFile(fixture.destination + QStringLiteral("/thumbnails/a.jpg"), "thumb"));
        QVERIFY(createDatabase(fixture.destination + QStringLiteral("/gamehq.db")));
        QVERIFY(execDatabase(fixture.destination + QStringLiteral("/gamehq.db"), {
            QStringLiteral("CREATE TABLE bindings (id INTEGER PRIMARY KEY, action TEXT)"),
            QStringLiteral("INSERT INTO bindings VALUES (1, 'screenshot')"),
            QStringLiteral("INSERT INTO settings VALUES ('internal.repairs_v1_done', '1')") }));
        PortableProfileImporter::Result result;
        QString error;
        QVERIFY2(PortableProfileImporter::importProfile(fixture.options(), result, error),
                 qPrintable(error));
    }

    static QByteArray trustState(int sequence)
    {
        return QByteArray("{\"schemaVersion\":1,\"highestReleaseSequence\":")
            + QByteArray::number(sequence) + QByteArray(",\"manifestSha256\":\"")
            + QByteArray(64, 'a') + QByteArray("\"}");
    }

    static int trustSequence(const QString& path)
    {
        const QByteArray text = readFile(path);
        const int start = text.indexOf("\"highestReleaseSequence\":") + 25;
        return text.mid(start, text.indexOf(',', start) - start).toInt();
    }

    // What an installed GameHQ writes on its own before the user ever reaches
    // Import: release notes cache, an empty share/, the crash marker, a
    // quarantined config and the anti-rollback state. None of it is library
    // data, so the import proceeds; the trust state and the quarantined
    // config are kept, the per-run files are not.
    void acceptsAnInstalledProfileWithAppGeneratedState()
    {
        Fixture fixture;
        const QString dest = fixture.destination;
        QVERIFY(writeFile(dest + QStringLiteral("/release-notes-cache/discovery.json"), "{\"x\":1}"));
        QVERIFY(writeFile(dest + QStringLiteral("/share/discord-webhooks.json"), "{}"));
        QVERIFY(writeFile(dest + QStringLiteral("/session.marker"), "pid"));
        QVERIFY(writeFile(dest + QStringLiteral("/config.json.corrupt-20261020-101010.json"), "{broken"));
        QVERIFY(writeFile(dest + QStringLiteral("/release-trust-state.json"), trustState(7)));
        PortableProfileImporter::Result result;
        QString error;
        QVERIFY2(PortableProfileImporter::importProfile(fixture.options(), result, error),
                 qPrintable(error));
        QCOMPARE(trustSequence(dest + QStringLiteral("/release-trust-state.json")), 7);
        QCOMPARE(readFile(dest + QStringLiteral("/config.json.corrupt-20261020-101010.json")),
                 QByteArray("{broken"));
        QVERIFY(!QFileInfo::exists(dest + QStringLiteral("/session.marker")));
    }

    // Import must never lower update protection: the stricter (higher)
    // release sequence of the two profiles wins.
    void keepsTheStricterReleaseTrustState()
    {
        for (const auto& [installed, portable] : { std::pair{ 3, 9 }, std::pair{ 9, 3 } }) {
            Fixture fixture;
            QVERIFY(writeFile(fixture.destination + QStringLiteral("/release-trust-state.json"),
                              trustState(installed)));
            QVERIFY(writeFile(fixture.source + QStringLiteral("/gamehq-data/release-trust-state.json"),
                              trustState(portable)));
            PortableProfileImporter::Result result;
            QString error;
            QVERIFY2(PortableProfileImporter::importProfile(fixture.options(), result, error),
                     qPrintable(error));
            QCOMPARE(trustSequence(fixture.destination + QStringLiteral("/release-trust-state.json")), 9);
        }
        {
            // Unreadable protection state is never silently replaced.
            Fixture fixture;
            QVERIFY(writeFile(fixture.destination + QStringLiteral("/release-trust-state.json"), "garbage"));
            PortableProfileImporter::Result result;
            QString error;
            QVERIFY(!PortableProfileImporter::importProfile(fixture.options(), result, error));
            QCOMPARE(readFile(fixture.destination + QStringLiteral("/release-trust-state.json")),
                     QByteArray("garbage"));
        }
    }

    void refusesAShareFolderHoldingUserData()
    {
        Fixture fixture;
        const QByteArray hooks = "{\"hooks\":[{\"name\":\"clips\"}]}";
        QVERIFY(writeFile(fixture.destination + QStringLiteral("/share/discord-webhooks.json"), hooks));
        PortableProfileImporter::Result result;
        QString error;
        QVERIFY(!PortableProfileImporter::importProfile(fixture.options(), result, error));
        QVERIFY2(error.contains(QStringLiteral("share")), qPrintable(error));
        QCOMPARE(readFile(fixture.destination + QStringLiteral("/share/discord-webhooks.json")), hooks);
    }

    void rejectsPortableGameExecutableAndDatabaseSetting()
    {
        for (const QString& statement : {
                 QStringLiteral("INSERT INTO games VALUES (1, 'portable:/Game.exe', NULL)"),
                 QStringLiteral("INSERT INTO settings VALUES ('future.path', 'portable:/secret')") }) {
            Fixture fixture;
            QVERIFY(execDatabase(fixture.source + QStringLiteral("/gamehq-data/gamehq.db"), {statement}));
            PortableProfileImporter::Result result;
            QString error;
            QVERIFY(!PortableProfileImporter::importProfile(fixture.options(), result, error));
            QVERIFY(error.contains(QStringLiteral("portable"), Qt::CaseInsensitive));
            QVERIFY(!QFileInfo::exists(fixture.destination));
        }
    }

    void rejectsMissingReferencedSound()
    {
        Fixture fixture;
        QVERIFY(execDatabase(fixture.source + QStringLiteral("/gamehq-data/gamehq.db"), {
            QStringLiteral("INSERT INTO sound_settings VALUES (1, 'portable:/gamehq-data/sound-packs/missing.wav')")
        }));
        PortableProfileImporter::Result result;
        QString error;
        QVERIFY(!PortableProfileImporter::importProfile(fixture.options(), result, error));
        QVERIFY(error.contains(QStringLiteral("missing")));
        QVERIFY(!QFileInfo::exists(fixture.destination));
    }

    void rollsBackEveryInjectedFailure_data()
    {
        QTest::addColumn<int>("failure");
        using F = PortableProfileImporter::FailurePoint;
        QTest::newRow("after staging") << int(F::AfterStaging);
        QTest::newRow("after database") << int(F::AfterDatabaseRewrite);
        QTest::newRow("before publish") << int(F::BeforePublish);
        QTest::newRow("after backup") << int(F::AfterDestinationBackup);
        QTest::newRow("after publish") << int(F::AfterPublish);
    }

    void rollsBackEveryInjectedFailure()
    {
        QFETCH(int, failure);
        Fixture fixture;
        QVERIFY(QDir().mkpath(fixture.destination));
        QVERIFY(writeFile(fixture.destination + QStringLiteral("/config.json"), "{}"));
        QVERIFY(writeFile(fixture.destination + QStringLiteral("/logs/startup.log"), "preserve-empty-profile"));
        const QByteArray sourceBefore = treeDigest(fixture.source);
        const QByteArray destinationBefore = treeDigest(fixture.destination);

        PortableProfileImporter::Result result;
        QString error;
        QVERIFY(!PortableProfileImporter::importProfile(
            fixture.options(static_cast<PortableProfileImporter::FailurePoint>(failure)), result, error));
        QVERIFY(error.contains(QStringLiteral("Injected")));
        QCOMPARE(treeDigest(fixture.source), sourceBefore);
        QCOMPARE(treeDigest(fixture.destination), destinationBefore);

        error.clear();
        QVERIFY2(PortableProfileImporter::importProfile(fixture.options(), result, error), qPrintable(error));
        QVERIFY(QFileInfo(fixture.destination + QStringLiteral("/gamehq.db")).isFile());
    }

    void recoversHardInterruptions_data()
    {
        QTest::addColumn<int>("failure");
        using F = PortableProfileImporter::FailurePoint;
        QTest::newRow("destination backed up") << int(F::InterruptAfterDestinationBackup);
        QTest::newRow("staged profile published") << int(F::InterruptAfterPublish);
    }

    void recoversHardInterruptions()
    {
        QFETCH(int, failure);
        Fixture fixture;
        QVERIFY(QDir().mkpath(fixture.destination));
        QVERIFY(writeFile(fixture.destination + QStringLiteral("/config.json"), "{}"));
        QVERIFY(writeFile(fixture.destination + QStringLiteral("/logs/startup.log"), "original"));
        const QByteArray sourceBefore = treeDigest(fixture.source);
        const QByteArray destinationBefore = treeDigest(fixture.destination);

        PortableProfileImporter::Result result;
        QString error;
        QVERIFY(!PortableProfileImporter::importProfile(
            fixture.options(static_cast<PortableProfileImporter::FailurePoint>(failure)), result, error));
        QVERIFY(error.contains(QStringLiteral("hard interruption")));
        QCOMPARE(treeDigest(fixture.source), sourceBefore);

        error.clear();
        QVERIFY2(PortableProfileImporter::importProfile(fixture.options(), result, error), qPrintable(error));
        QCOMPARE(treeDigest(fixture.source), sourceBefore);
        QVERIFY(treeDigest(fixture.destination) != destinationBefore);
        QVERIFY(QFileInfo(fixture.destination + QStringLiteral("/gamehq.db")).isFile());
        const QString parent = QFileInfo(fixture.destination).absolutePath();
        const QString prefix = QStringLiteral(".%1.import-").arg(QFileInfo(fixture.destination).fileName());
        for (const QFileInfo& entry : QDir(parent).entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries))
            QVERIFY2(!entry.fileName().startsWith(prefix), qPrintable(entry.fileName()));
    }

    void rejectsMalformedInputs()
    {
        {
            Fixture fixture;
            QVERIFY(writeFile(fixture.source + QStringLiteral("/gamehq-data/config.json"), "{"));
            PortableProfileImporter::Result result;
            QString error;
            QVERIFY(!PortableProfileImporter::importProfile(fixture.options(), result, error));
            QVERIFY(error.contains(QStringLiteral("malformed")));
        }
        {
            Fixture fixture;
            QVERIFY(QFile::remove(fixture.source + QStringLiteral("/gamehq-data/gamehq.db")));
            QVERIFY(writeFile(fixture.source + QStringLiteral("/gamehq-data/gamehq.db"), "not sqlite"));
            PortableProfileImporter::Result result;
            QString error;
            QVERIFY(!PortableProfileImporter::importProfile(fixture.options(), result, error));
            QVERIFY(!error.isEmpty());
        }
    }
};

QTEST_MAIN(TestPortableProfileImporter)
#include "tst_portableprofileimporter.moc"
