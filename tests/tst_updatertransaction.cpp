#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include "TestReleaseSigner.h"
#include <QProcess>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>
#include <QUuid>
#include <miniz.h>
#include "updater/UpdaterTransaction.h"
#include "updater/UpdaterDataSnapshot.h"
#include "updater/UpdaterSwap.h"
#include "updater/UpdaterHealth.h"
#include "updater/UpdaterRecovery.h"
#include "launcher/UpdaterPromotion.h"
#include "core/UpdateMaintenance.h"
#include <windows.h>

class UpdaterTransactionTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void dryRunListsOperationsWithoutWriting();
    void rejectsPathOutsidePackageRoot();
    void stagesValidPackage();
    void rejectsTraversalAndRemovesStaging();
    void rejectsForbiddenDataAndBadManifest();
    void rejectsPackageChangedAfterVerification();
    void snapshotsAndRestoresDataWithoutTouchingCaptures();
    void restoresARealSqliteDatabaseAfterFailedMigration();
    void failedRestoreKeepsUntouchedDataFiles();
    void swapsOnlyOwnedProgramFiles();
    void lockedFileAbortsAndRollsBack();
    void healthyStartPublishesToken();
    void missingHealthTokenTimesOut();
    void failedHealthRollbackRestoresProgramAndData();
    void interruptedMixedSwapRecoversPreviousProgram();
    void rejectsPackageRequiringNewerUpdater();
    void promotesOnlySelfTestingPendingHelper();
    void completeApplyCleansStaleStagingAndPreservesUserData();
    void bindsHandoffToTheExactAuthorisingProcess();
    void shippedHelperRefusesTestTrustMaterial();

private:
    static QString writeFixture(const QString &root, const QString &backupDir);
    // Re-signs the staged package so the transaction still carries evidence a
    // real release manifest would have produced.
    static bool refreshEvidence(const QString &packagePath, QJsonObject &object);
    static bool writeZip(const QString &path, const QList<QPair<QByteArray, QByteArray>> &entries);
    static bool syncTransactionHash(const QString &transaction, const QString &package);
    static QProcess *runHelper(const QString &mode, const QString &transaction, QObject *parent);
};

namespace
{
// The helper now pins its wait to one exact process, so fixtures must carry a
// creation time that really belongs to the process id they name.
quint64 currentProcessCreationTime()
{
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
        return 0;
    return (static_cast<quint64>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}
} // namespace

QString UpdaterTransactionTest::writeFixture(const QString &root, const QString &backupDir)
{
    const QString updateDir = QDir(root).filePath(QStringLiteral(".update"));
    const QString downloadsDir = QDir(updateDir).filePath(QStringLiteral("downloads"));
    QDir().mkpath(downloadsDir);
    QDir().mkpath(QDir(root).filePath(QStringLiteral("gamehq-data")));
    const QString packagePath = QDir(downloadsDir).filePath(
        QStringLiteral("GameHQ-9.8.7-win64-update.zip"));
    QFile package(packagePath);
    if (!package.open(QIODevice::WriteOnly) || package.write("verified package") != 16)
        return {};
    package.close();

    QJsonObject object {
        { QStringLiteral("schemaVersion"), 2 },
        { QStringLiteral("productId"), QStringLiteral("underfusion.gamehq") },
        { QStringLiteral("expectedVersion"), QStringLiteral("9.8.7") },
        { QStringLiteral("packageRoot"), QDir::toNativeSeparators(root) },
        { QStringLiteral("packagePath"), QDir::toNativeSeparators(packagePath) },
        { QStringLiteral("stagingDir"), QDir::toNativeSeparators(QDir(updateDir).filePath(QStringLiteral("staging"))) },
        { QStringLiteral("backupDir"), QDir::toNativeSeparators(backupDir) },
        { QStringLiteral("restartExecutable"), QDir::toNativeSeparators(QDir(root).filePath(QStringLiteral("GameHQ.exe"))) },
        { QStringLiteral("healthTokenPath"), QDir::toNativeSeparators(QDir(updateDir).filePath(QStringLiteral("healthy.token"))) },
        { QStringLiteral("dataDir"), QDir::toNativeSeparators(QDir(root).filePath(QStringLiteral("gamehq-data"))) },
        { QStringLiteral("dataSnapshotDir"), QDir::toNativeSeparators(QDir(updateDir).filePath(QStringLiteral("data-snapshot"))) },
        { QStringLiteral("callerPid"), static_cast<qint64>(QCoreApplication::applicationPid()) },
        { QStringLiteral("callerCreationTime"), static_cast<qint64>(currentProcessCreationTime()) },
        { QStringLiteral("phase"), QStringLiteral("download_verified") }
    };
    if (!refreshEvidence(packagePath, object))
        return {};
    const QString transactionPath = QDir(updateDir).filePath(QStringLiteral("transaction.json"));
    QFile transaction(transactionPath);
    if (!transaction.open(QIODevice::WriteOnly)
        || transaction.write(QJsonDocument(object).toJson(QJsonDocument::Compact)) <= 0)
        return {};
    return transactionPath;
}

bool UpdaterTransactionTest::writeZip(
    const QString &path, const QList<QPair<QByteArray, QByteArray>> &entries)
{
    mz_zip_archive zip{};
    const QByteArray nativePath = QFile::encodeName(path);
    if (!mz_zip_writer_init_file(&zip, nativePath.constData(), 0))
        return false;
    bool ok = true;
    for (const auto &entry : entries) {
        if (!mz_zip_writer_add_mem(&zip, entry.first.constData(), entry.second.constData(),
                                   static_cast<size_t>(entry.second.size()), MZ_BEST_SPEED)) {
            ok = false;
            break;
        }
    }
    if (ok)
        ok = mz_zip_writer_finalize_archive(&zip);
    mz_zip_writer_end(&zip);
    return ok;
}

QProcess *UpdaterTransactionTest::runHelper(const QString &mode, const QString &transaction,
                                            QObject *parent)
{
    auto *process = new QProcess(parent);
    process->start(QStringLiteral(UPDATER_EXE), { mode, transaction });
    return process;
}

bool UpdaterTransactionTest::refreshEvidence(const QString &packagePath, QJsonObject &object)
{
    QFile packageFile(packagePath);
    if (!packageFile.open(QIODevice::ReadOnly))
        return false;
    const QByteArray bytes = packageFile.readAll();
    packageFile.close();
    const QString packageHash = QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    const QString version = object.value(QStringLiteral("expectedVersion")).toString();
    const QString assetName = QFileInfo(packagePath).fileName();
    const auto manifest = test_release_signer::buildManifest(
        version.toStdString(), 3,
        {{"update", assetName.toStdString(), static_cast<std::uint64_t>(bytes.size()),
          packageHash.toStdString()}});
    const QByteArray manifestBytes(reinterpret_cast<const char *>(manifest.data()),
                                   static_cast<qsizetype>(manifest.size()));
    const QString signature = QString::fromStdString(test_release_signer::sign(manifest));

    const QString downloads = QFileInfo(packagePath).absolutePath();
    const QString manifestPath = QDir(downloads).filePath(QStringLiteral("gamehq-release.json"));
    const QString signaturePath = QDir(downloads).filePath(QStringLiteral("gamehq-release.sig"));
    QFile manifestFile(manifestPath);
    if (!manifestFile.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || manifestFile.write(manifestBytes) != manifestBytes.size())
        return false;
    manifestFile.close();
    QFile signatureFile(signaturePath);
    const QByteArray signatureBytes = signature.toLatin1() + QByteArrayLiteral("\n");
    if (!signatureFile.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || signatureFile.write(signatureBytes) != signatureBytes.size())
        return false;
    signatureFile.close();

    object.insert(QStringLiteral("expectedSha256"), packageHash);
    object.insert(QStringLiteral("manifestPath"), QDir::toNativeSeparators(manifestPath));
    object.insert(QStringLiteral("signaturePath"), QDir::toNativeSeparators(signaturePath));
    object.insert(QStringLiteral("manifestSha256"), QString::fromLatin1(
        QCryptographicHash::hash(manifestBytes, QCryptographicHash::Sha256).toHex()));
    object.insert(QStringLiteral("releaseSignature"), signature);
    object.insert(QStringLiteral("releaseKeyId"),
                  QString::fromLatin1(test_release_signer::kTestKeyId));
    object.insert(QStringLiteral("releaseSequence"), 3);
    object.insert(QStringLiteral("artifactName"), assetName);
    object.insert(QStringLiteral("artifactSize"), static_cast<qint64>(bytes.size()));
    object.insert(QStringLiteral("artifactSha256"), packageHash);
    return true;
}

bool UpdaterTransactionTest::syncTransactionHash(const QString &transaction, const QString &package)
{
    QFile transactionFile(transaction);
    if (!transactionFile.open(QIODevice::ReadOnly))
        return false;
    QJsonObject object = QJsonDocument::fromJson(transactionFile.readAll()).object();
    transactionFile.close();
    if (!refreshEvidence(package, object))
        return false;
    if (!transactionFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return transactionFile.write(QJsonDocument(object).toJson(QJsonDocument::Compact)) > 0;
}

void UpdaterTransactionTest::dryRunListsOperationsWithoutWriting()
{
    QTemporaryDir dir(QDir::current().filePath(QStringLiteral("tst-updater-dryrun-XXXXXX")));
    QVERIFY(dir.isValid());
    const QString staging = QDir(dir.path()).filePath(QStringLiteral(".update/staging"));
    const QString backup = QDir(dir.path()).filePath(QStringLiteral(".update/backup"));
    const QString transaction = writeFixture(dir.path(), backup);
    QVERIFY(!transaction.isEmpty());
    QVERIFY(!QFileInfo::exists(staging));
    QVERIFY(!QFileInfo::exists(backup));

    QProcess process;
    process.start(QStringLiteral(UPDATER_EXE), { QStringLiteral("--dry-run"), transaction });
    QVERIFY(process.waitForFinished(10000));
    QCOMPARE(process.exitCode(), 0);
    const QByteArray output = process.readAllStandardOutput();
    QVERIFY(output.contains("DRY RUN - no files will be changed"));
    QVERIFY(output.contains("BACKUP IF PRESENT GameHQ.exe"));
    QVERIFY(output.contains("INSTALL IF PRESENT app/"));
    QVERIFY(output.contains("INSTALL IF PRESENT licenses/"));
    QVERIFY(output.contains("--post-update 9.8.7"));
    QVERIFY2(process.readAllStandardError().isEmpty(), process.readAllStandardError().constData());
    QVERIFY(!QFileInfo::exists(staging));
    QVERIFY(!QFileInfo::exists(backup));
}

void UpdaterTransactionTest::rejectsPathOutsidePackageRoot()
{
    QTemporaryDir dir(QDir::current().filePath(QStringLiteral("tst-updater-escape-XXXXXX")));
    QVERIFY(dir.isValid());
    const QString escaped = QDir(dir.path()).absoluteFilePath(QStringLiteral("../escaped-backup"));
    const QString transaction = writeFixture(dir.path(), escaped);
    QVERIFY(!transaction.isEmpty());

    QProcess process;
    process.start(QStringLiteral(UPDATER_EXE), { QStringLiteral("--dry-run"), transaction });
    QVERIFY(process.waitForFinished(10000));
    QVERIFY(process.exitCode() != 0);
    QVERIFY(process.readAllStandardError().contains("escapes the package root"));
    QVERIFY(!QFileInfo::exists(escaped));
}

void UpdaterTransactionTest::stagesValidPackage()
{
    QTemporaryDir dir(QDir::current().filePath(QStringLiteral("tst-updater-stage-XXXXXX")));
    QVERIFY(dir.isValid());
    const QString transaction = writeFixture(
        dir.path(), QDir(dir.path()).filePath(QStringLiteral(".update/backup")));
    QVERIFY(!transaction.isEmpty());
    const QString package = QDir(dir.path()).filePath(
        QStringLiteral(".update/downloads/GameHQ-9.8.7-win64-update.zip"));
    const QByteArray manifest = R"({"schemaVersion":1,"productId":"underfusion.gamehq","appVersion":"9.8.7","layoutVersion":1,"minimumUpdaterVersion":"0.6.10"})";
    QVERIFY(writeZip(package, {{"GameHQ.exe", "launcher"}, {"app/GameHQ.exe", "application"},
                               {"update-package.json", manifest}, {"README.txt", "readme"},
                               {"licenses/SOURCE_OFFER.txt", "source binding"}}));
    QVERIFY(syncTransactionHash(transaction, package));

    QScopedPointer<QProcess> process(runHelper(QStringLiteral("--stage"), transaction, this));
    QVERIFY(process->waitForFinished(10000));
    QCOMPARE(process->exitCode(), 0);
    QVERIFY(process->readAllStandardOutput().contains("STAGED AND VALIDATED"));
    QVERIFY(QFileInfo::exists(QDir(dir.path()).filePath(QStringLiteral(".update/staging/app/GameHQ.exe"))));
    QVERIFY(!QFileInfo::exists(QDir(dir.path()).filePath(QStringLiteral("app/GameHQ.exe"))));
}

void UpdaterTransactionTest::rejectsTraversalAndRemovesStaging()
{
    QTemporaryDir dir(QDir::current().filePath(QStringLiteral("tst-updater-traversal-XXXXXX")));
    QVERIFY(dir.isValid());
    const QString transaction = writeFixture(
        dir.path(), QDir(dir.path()).filePath(QStringLiteral(".update/backup")));
    const QString package = QDir(dir.path()).filePath(
        QStringLiteral(".update/downloads/GameHQ-9.8.7-win64-update.zip"));
    QVERIFY(writeZip(package, {{"../escaped.txt", "hostile"}}));
    QVERIFY(syncTransactionHash(transaction, package));

    QScopedPointer<QProcess> process(runHelper(QStringLiteral("--stage"), transaction, this));
    QVERIFY(process->waitForFinished(10000));
    QVERIFY(process->exitCode() != 0);
    QVERIFY(process->readAllStandardError().contains("traversal"));
    QVERIFY(!QFileInfo::exists(QDir(dir.path()).filePath(QStringLiteral("escaped.txt"))));
    QVERIFY(!QFileInfo::exists(QDir(dir.path()).filePath(QStringLiteral(".update/staging"))));
}

void UpdaterTransactionTest::rejectsForbiddenDataAndBadManifest()
{
    const QList<QPair<QByteArray, QByteArray>> badEntries[] = {
        {{"GameHQ.exe", "launcher"}, {"app/GameHQ.exe", "application"},
         {"portable.flag", "1"}, {"update-package.json", "{}"}},
        {{"GameHQ.exe", "launcher"}, {"app/GameHQ.exe", "application"},
         {"update-package.json", R"({"schemaVersion":1,"productId":"underfusion.gamehq","appVersion":"9.8.6","layoutVersion":1,"minimumUpdaterVersion":"0.6.10"})"}}
    };
    for (const auto &entries : badEntries) {
        QTemporaryDir dir(QDir::current().filePath(QStringLiteral("tst-updater-invalid-XXXXXX")));
        QVERIFY(dir.isValid());
        const QString transaction = writeFixture(
            dir.path(), QDir(dir.path()).filePath(QStringLiteral(".update/backup")));
        const QString package = QDir(dir.path()).filePath(
            QStringLiteral(".update/downloads/GameHQ-9.8.7-win64-update.zip"));
        QVERIFY(writeZip(package, entries));
        QVERIFY(syncTransactionHash(transaction, package));
        QScopedPointer<QProcess> process(runHelper(QStringLiteral("--stage"), transaction, this));
        QVERIFY(process->waitForFinished(10000));
        QVERIFY(process->exitCode() != 0);
        QVERIFY(!QFileInfo::exists(QDir(dir.path()).filePath(QStringLiteral(".update/staging"))));
        QVERIFY(!QFileInfo::exists(QDir(dir.path()).filePath(QStringLiteral("app/GameHQ.exe"))));
    }
}

void UpdaterTransactionTest::rejectsPackageChangedAfterVerification()
{
    QTemporaryDir dir(QDir::current().filePath(QStringLiteral("tst-updater-hash-XXXXXX")));
    QVERIFY(dir.isValid());
    const QString transaction = writeFixture(
        dir.path(), QDir(dir.path()).filePath(QStringLiteral(".update/backup")));
    const QString package = QDir(dir.path()).filePath(
        QStringLiteral(".update/downloads/GameHQ-9.8.7-win64-update.zip"));
    const QByteArray manifest = R"({"schemaVersion":1,"productId":"underfusion.gamehq","appVersion":"9.8.7","layoutVersion":1,"minimumUpdaterVersion":"0.6.10"})";
    QVERIFY(writeZip(package, {{"GameHQ.exe", "launcher"}, {"app/GameHQ.exe", "application"},
                               {"update-package.json", manifest}}));
    // Deliberately keep the transaction's hash of the earlier placeholder. The
    // signed manifest now catches the swap before staging even begins, because
    // the replacement has a different length than the authorised artifact.
    QScopedPointer<QProcess> process(runHelper(QStringLiteral("--stage"), transaction, this));
    QVERIFY(process->waitForFinished(10000));
    QVERIFY(process->exitCode() != 0);
    QVERIFY(process->readAllStandardError().contains("signed manifest"));
    QVERIFY(!QFileInfo::exists(QDir(dir.path()).filePath(QStringLiteral(".update/staging"))));

    // A same-length swap passes the manifest length check and must still be
    // stopped by the package hash comparison during staging.
    QFile staged(package);
    QVERIFY(staged.open(QIODevice::ReadOnly));
    const qint64 authorisedSize = staged.size();
    staged.close();
    QVERIFY(syncTransactionHash(transaction, package));
    QVERIFY(staged.open(QIODevice::ReadWrite));
    QVERIFY(staged.seek(authorisedSize - 1));
    QVERIFY(staged.write(QByteArrayLiteral("!")) == 1);
    staged.close();
    QCOMPARE(QFileInfo(package).size(), authorisedSize);
    QScopedPointer<QProcess> sameLength(runHelper(QStringLiteral("--stage"), transaction, this));
    QVERIFY(sameLength->waitForFinished(10000));
    QVERIFY(sameLength->exitCode() != 0);
    QVERIFY(sameLength->readAllStandardError().contains("SHA-256 changed"));
    QVERIFY(!QFileInfo::exists(QDir(dir.path()).filePath(QStringLiteral(".update/staging"))));
}

void UpdaterTransactionTest::snapshotsAndRestoresDataWithoutTouchingCaptures()
{
    QTemporaryDir dir(QDir::current().filePath(QStringLiteral("tst-updater-data-XXXXXX")));
    QVERIFY(dir.isValid());
    const QString data = QDir(dir.path()).filePath(QStringLiteral("gamehq-data"));
    const QString captures = QDir(dir.path()).filePath(QStringLiteral("Captures"));
    const QString snapshot = QDir(dir.path()).filePath(QStringLiteral(".update/data-snapshot"));
    QDir().mkpath(data);
    QDir().mkpath(captures);
    auto write = [](const QString &path, const QByteArray &contents) {
        QFile file(path); return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
            && file.write(contents) == contents.size();
    };
    QVERIFY(write(QDir(data).filePath(QStringLiteral("config.json")), "old config"));
    QVERIFY(write(QDir(data).filePath(QStringLiteral("gamehq.db")), "old database"));
    QVERIFY(write(QDir(data).filePath(QStringLiteral("gamehq.db-wal")), "old wal"));
    const QString capture = QDir(captures).filePath(QStringLiteral("clip.mp4"));
    QVERIFY(write(capture, "user media"));
    std::string error;
    QVERIFY2(maintenance::begin(dir.path().toStdWString(), error), error.c_str());
    QVERIFY2(updater::createDataSnapshot(data.toStdWString(), snapshot.toStdWString(), error), error.c_str());
    QVERIFY(write(QDir(data).filePath(QStringLiteral("config.json")), "new config"));
    QVERIFY(write(QDir(data).filePath(QStringLiteral("gamehq.db")), "migrated database"));
    QFile::remove(QDir(data).filePath(QStringLiteral("gamehq.db-wal")));
    QVERIFY(write(QDir(data).filePath(QStringLiteral("gamehq.db-shm")), "new shm"));
    QVERIFY2(updater::restoreDataSnapshot(data.toStdWString(), snapshot.toStdWString(), error), error.c_str());
    auto read = [](const QString &path) { QFile file(path); file.open(QIODevice::ReadOnly); return file.readAll(); };
    QCOMPARE(read(QDir(data).filePath(QStringLiteral("config.json"))), QByteArray("old config"));
    QCOMPARE(read(QDir(data).filePath(QStringLiteral("gamehq.db"))), QByteArray("old database"));
    QCOMPARE(read(QDir(data).filePath(QStringLiteral("gamehq.db-wal"))), QByteArray("old wal"));
    QVERIFY(!QFileInfo::exists(QDir(data).filePath(QStringLiteral("gamehq.db-shm"))));
    QCOMPARE(read(capture), QByteArray("user media"));
}

void UpdaterTransactionTest::restoresARealSqliteDatabaseAfterFailedMigration()
{
    QTemporaryDir dir(QDir::current().filePath(QStringLiteral("tst-updater-sqlite-XXXXXX")));
    QVERIFY(dir.isValid());
    const QString data = QDir(dir.path()).filePath(QStringLiteral("gamehq-data"));
    const QString databasePath = QDir(data).filePath(QStringLiteral("gamehq.db"));
    const QString snapshot = QDir(dir.path()).filePath(QStringLiteral(".update/data-snapshot"));
    QVERIFY(QDir().mkpath(data));

    const auto withDatabase = [&](const auto& operation) {
        const QString connectionName = QUuid::createUuid().toString(QUuid::WithoutBraces);
        bool result = false;
        {
            QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                               connectionName);
            database.setDatabaseName(databasePath);
            if (database.open())
                result = operation(database);
        }
        QSqlDatabase::removeDatabase(connectionName);
        return result;
    };

    QVERIFY(withDatabase([](QSqlDatabase& database) {
        QSqlQuery query(database);
        return query.exec(QStringLiteral("PRAGMA journal_mode=DELETE"))
            && query.exec(QStringLiteral("PRAGMA user_version=7"))
            && query.exec(QStringLiteral(
                "CREATE TABLE captures (id INTEGER PRIMARY KEY, name TEXT NOT NULL)"))
            && query.exec(QStringLiteral("INSERT INTO captures (name) VALUES ('original')"));
    }));

    std::string error;
    QVERIFY2(updater::createDataSnapshot(data.toStdWString(), snapshot.toStdWString(), error),
             error.c_str());

    QVERIFY(withDatabase([](QSqlDatabase& database) {
        QSqlQuery query(database);
        return query.exec(QStringLiteral("ALTER TABLE captures ADD COLUMN migrated TEXT"))
            && query.exec(QStringLiteral("UPDATE captures SET name = 'changed', migrated = 'yes'"))
            && query.exec(QStringLiteral("CREATE TABLE migration_only (id INTEGER)"))
            && query.exec(QStringLiteral("PRAGMA user_version=99"));
    }));

    QVERIFY2(updater::restoreDataSnapshot(data.toStdWString(), snapshot.toStdWString(), error),
             error.c_str());

    QVERIFY(withDatabase([](QSqlDatabase& database) {
        QSqlQuery version(database);
        if (!version.exec(QStringLiteral("PRAGMA user_version")) || !version.next()
            || version.value(0).toInt() != 7)
            return false;

        QSqlQuery capture(database);
        if (!capture.exec(QStringLiteral("SELECT name FROM captures")) || !capture.next()
            || capture.value(0).toString() != QStringLiteral("original"))
            return false;

        QSqlQuery schema(database);
        if (!schema.exec(QStringLiteral(
                "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
                "AND name='migration_only'"))
            || !schema.next() || schema.value(0).toInt() != 0)
            return false;

        QSqlQuery columns(database);
        if (!columns.exec(QStringLiteral("PRAGMA table_info(captures)")))
            return false;
        int columnCount = 0;
        while (columns.next())
            ++columnCount;
        return columnCount == 2;
    }));
}

void UpdaterTransactionTest::failedRestoreKeepsUntouchedDataFiles()
{
    QTemporaryDir dir(QDir::current().filePath(QStringLiteral("tst-updater-datafail-XXXXXX")));
    QVERIFY(dir.isValid());
    const QString data = QDir(dir.path()).filePath(QStringLiteral("gamehq-data"));
    const QString snapshot = QDir(dir.path()).filePath(QStringLiteral(".update/data-snapshot"));
    QDir().mkpath(data);
    auto write = [](const QString &path, const QByteArray &contents) {
        QFile file(path); return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
            && file.write(contents) == contents.size();
    };
    QVERIFY(write(QDir(data).filePath(QStringLiteral("config.json")), "old config"));
    QVERIFY(write(QDir(data).filePath(QStringLiteral("gamehq.db")), "old database"));
    QVERIFY(write(QDir(data).filePath(QStringLiteral("gamehq.db-wal")), "old wal"));
    std::string error;
    QVERIFY2(updater::createDataSnapshot(data.toStdWString(), snapshot.toStdWString(), error), error.c_str());
    // A directory in the gamehq.db slot makes the backup step fail after
    // config.json was already backed up; the rollback must leave the untouched
    // gamehq.db-wal alone instead of deleting every known state file.
    QVERIFY(write(QDir(data).filePath(QStringLiteral("config.json")), "new config"));
    QVERIFY(QFile::remove(QDir(data).filePath(QStringLiteral("gamehq.db"))));
    QVERIFY(QDir().mkpath(QDir(data).filePath(QStringLiteral("gamehq.db"))));
    QVERIFY(write(QDir(data).filePath(QStringLiteral("gamehq.db-wal")), "current wal"));
    QVERIFY(!updater::restoreDataSnapshot(data.toStdWString(), snapshot.toStdWString(), error));
    auto read = [](const QString &path) { QFile file(path); file.open(QIODevice::ReadOnly); return file.readAll(); };
    QCOMPARE(read(QDir(data).filePath(QStringLiteral("config.json"))), QByteArray("new config"));
    QCOMPARE(read(QDir(data).filePath(QStringLiteral("gamehq.db-wal"))), QByteArray("current wal"));
    QVERIFY(QFileInfo(QDir(data).filePath(QStringLiteral("gamehq.db"))).isDir());
    QVERIFY(!QFileInfo::exists(QDir(data).filePath(QStringLiteral("config.json.restore.partial"))));
    QVERIFY(!QFileInfo::exists(QDir(data).filePath(QStringLiteral("config.json.failed-update.backup"))));
}

void UpdaterTransactionTest::swapsOnlyOwnedProgramFiles()
{
    QTemporaryDir dir(QDir::current().filePath(QStringLiteral("tst-updater-swap-XXXXXX")));
    QVERIFY(dir.isValid());
    const QString staging = QDir(dir.path()).filePath(QStringLiteral(".update/staging"));
    const QString backup = QDir(dir.path()).filePath(QStringLiteral(".update/backup"));
    QDir().mkpath(QDir(staging).filePath(QStringLiteral("app")));
    QDir().mkpath(QDir(dir.path()).filePath(QStringLiteral("app")));
    auto write = [](const QString &path, const QByteArray &data) { QFile f(path); return f.open(QIODevice::WriteOnly) && f.write(data) == data.size(); };
    QVERIFY(write(QDir(dir.path()).filePath(QStringLiteral("GameHQ.exe")), "old launcher"));
    QVERIFY(write(QDir(dir.path()).filePath(QStringLiteral("app/GameHQ.exe")), "old app"));
    QVERIFY(write(QDir(dir.path()).filePath(QStringLiteral("portable.flag")), "keep"));
    QVERIFY(write(QDir(staging).filePath(QStringLiteral("GameHQ.exe")), "new launcher"));
    QVERIFY(write(QDir(staging).filePath(QStringLiteral("app/GameHQ.exe")), "new app"));
    updater::Transaction tx; tx.packageRoot = dir.path().toStdWString();
    tx.stagingDir = staging.toStdWString(); tx.backupDir = backup.toStdWString();
    tx.healthTokenPath = QDir(dir.path()).filePath(QStringLiteral(".update/healthy.token")).toStdWString();
    std::string error;
    QVERIFY2(updater::swapProgramFiles(tx, error), error.c_str());
    auto read = [](const QString &path) { QFile f(path); f.open(QIODevice::ReadOnly); return f.readAll(); };
    QCOMPARE(read(QDir(dir.path()).filePath(QStringLiteral("GameHQ.exe"))), QByteArray("new launcher"));
    QCOMPARE(read(QDir(backup).filePath(QStringLiteral("app/GameHQ.exe"))), QByteArray("old app"));
    QCOMPARE(read(QDir(dir.path()).filePath(QStringLiteral("portable.flag"))), QByteArray("keep"));
}

void UpdaterTransactionTest::lockedFileAbortsAndRollsBack()
{
    QTemporaryDir dir(QDir::current().filePath(QStringLiteral("tst-updater-locked-XXXXXX")));
    QVERIFY(dir.isValid());
    const QString staging = QDir(dir.path()).filePath(QStringLiteral(".update/staging"));
    const QString backup = QDir(dir.path()).filePath(QStringLiteral(".update/backup"));
    QDir().mkpath(staging);
    auto write = [](const QString &path, const QByteArray &data) { QFile f(path); return f.open(QIODevice::WriteOnly) && f.write(data) == data.size(); };
    const QString live = QDir(dir.path()).filePath(QStringLiteral("GameHQ.exe"));
    const QString incoming = QDir(staging).filePath(QStringLiteral("GameHQ.exe"));
    QVERIFY(write(live, "old")); QVERIFY(write(incoming, "new"));
    HANDLE lock = CreateFileW(live.toStdWString().c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    QVERIFY(lock != INVALID_HANDLE_VALUE);
    updater::Transaction tx; tx.packageRoot = dir.path().toStdWString();
    tx.stagingDir = staging.toStdWString(); tx.backupDir = backup.toStdWString();
    tx.healthTokenPath = QDir(dir.path()).filePath(QStringLiteral(".update/healthy.token")).toStdWString();
    std::string error;
    QVERIFY(!updater::swapProgramFiles(tx, error));
    CloseHandle(lock);
    QFile oldFile(live); QVERIFY(oldFile.open(QIODevice::ReadOnly)); QCOMPARE(oldFile.readAll(), QByteArray("old"));
    QFile newFile(incoming); QVERIFY(newFile.open(QIODevice::ReadOnly)); QCOMPARE(newFile.readAll(), QByteArray("new"));
    QVERIFY(!QFileInfo::exists(backup));
}

void UpdaterTransactionTest::healthyStartPublishesToken()
{
    QTemporaryDir dir(QDir::current().filePath(QStringLiteral("tst-updater-health-XXXXXX")));
    QVERIFY(dir.isValid());
    updater::Transaction tx;
    tx.packageRoot = dir.path().toStdWString();
    tx.restartExecutable = QStringLiteral(HEALTH_FIXTURE_EXE).toStdWString();
    tx.healthTokenPath = QDir(dir.path()).filePath(QStringLiteral("healthy.token")).toStdWString();
    tx.expectedVersion = "9.8.7";
    std::string error;
    QVERIFY2(updater::launchAndWaitForHealth(tx, 5000, error), error.c_str());
    QFile token(QString::fromStdWString(tx.healthTokenPath.wstring()));
    QVERIFY(token.open(QIODevice::ReadOnly));
    QCOMPARE(token.readAll().trimmed(), QByteArray("9.8.7"));
}

void UpdaterTransactionTest::missingHealthTokenTimesOut()
{
    QTemporaryDir dir(QDir::current().filePath(QStringLiteral("tst-updater-timeout-XXXXXX")));
    QVERIFY(dir.isValid());
    updater::Transaction tx;
    tx.packageRoot = dir.path().toStdWString();
    tx.restartExecutable = QStringLiteral(HEALTH_FIXTURE_EXE).toStdWString();
    tx.healthTokenPath = QDir(dir.path()).filePath(QStringLiteral("healthy.token")).toStdWString();
    tx.expectedVersion = "9.9.8";
    std::string error;
    QVERIFY(!updater::launchAndWaitForHealth(tx, 500, error));
    QVERIFY(QString::fromStdString(error).contains(QStringLiteral("timed out")));
}

void UpdaterTransactionTest::failedHealthRollbackRestoresProgramAndData()
{
    QTemporaryDir dir(QDir::current().filePath(QStringLiteral("tst-updater-rollback-XXXXXX")));
    QVERIFY(dir.isValid());
    const QString update = QDir(dir.path()).filePath(QStringLiteral(".update"));
    const QString staging = QDir(update).filePath(QStringLiteral("staging"));
    const QString backup = QDir(update).filePath(QStringLiteral("backup"));
    const QString snapshot = QDir(update).filePath(QStringLiteral("data-snapshot"));
    const QString data = QDir(dir.path()).filePath(QStringLiteral("gamehq-data"));
    const QString captures = QDir(dir.path()).filePath(QStringLiteral("Captures"));
    QDir().mkpath(QDir(staging).filePath(QStringLiteral("app")));
    QDir().mkpath(QDir(dir.path()).filePath(QStringLiteral("app")));
    QDir().mkpath(data);
    QDir().mkpath(captures);
    auto write = [](const QString &path, const QByteArray &bytes) {
        QFile file(path); return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
            && file.write(bytes) == bytes.size();
    };
    const QString liveExe = QDir(dir.path()).filePath(QStringLiteral("GameHQ.exe"));
    QVERIFY(write(liveExe, "old program"));
    QVERIFY(write(QDir(staging).filePath(QStringLiteral("GameHQ.exe")), "new program"));
    QVERIFY(write(QDir(data).filePath(QStringLiteral("config.json")), "old config"));
    QVERIFY(write(QDir(captures).filePath(QStringLiteral("clip.mp4")), "user media"));
    QVERIFY(write(QDir(dir.path()).filePath(QStringLiteral("portable.flag")), "keep portable"));
    std::string error;
    QVERIFY2(updater::createDataSnapshot(data.toStdWString(), snapshot.toStdWString(), error), error.c_str());
    QVERIFY(write(QDir(data).filePath(QStringLiteral("config.json")), "new config"));
    updater::Transaction tx;
    tx.packageRoot = dir.path().toStdWString(); tx.stagingDir = staging.toStdWString();
    tx.backupDir = backup.toStdWString(); tx.dataDir = data.toStdWString();
    tx.dataSnapshotDir = snapshot.toStdWString();
    tx.healthTokenPath = QDir(update).filePath(QStringLiteral("healthy.token")).toStdWString();
    tx.restartExecutable = QStringLiteral(HEALTH_FIXTURE_EXE).toStdWString();
    QVERIFY2(updater::swapProgramFiles(tx, error), error.c_str());
    QVERIFY2(updater::writeTransactionPhase(tx, "validating", error), error.c_str());
    QVERIFY2(updater::recoverInterruptedUpdate(tx, error), error.c_str());
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(
        QDir(dir.path()).filePath(QStringLiteral("previous-started.token"))), 2000);
    QFile program(liveExe); QVERIFY(program.open(QIODevice::ReadOnly));
    QCOMPARE(program.readAll(), QByteArray("old program"));
    QFile config(QDir(data).filePath(QStringLiteral("config.json")));
    QVERIFY(config.open(QIODevice::ReadOnly)); QCOMPARE(config.readAll(), QByteArray("old config"));
    QFile clip(QDir(captures).filePath(QStringLiteral("clip.mp4")));
    QVERIFY(clip.open(QIODevice::ReadOnly)); QCOMPARE(clip.readAll(), QByteArray("user media"));
    QFile portable(QDir(dir.path()).filePath(QStringLiteral("portable.flag")));
    QVERIFY(portable.open(QIODevice::ReadOnly)); QCOMPARE(portable.readAll(), QByteArray("keep portable"));
    QVERIFY(!QFileInfo::exists(backup));
    QFile phase(QDir(update).filePath(QStringLiteral("transaction.phase")));
    QVERIFY(phase.open(QIODevice::ReadOnly)); QCOMPARE(phase.readAll().trimmed(), QByteArray("rolled_back"));
    QVERIFY(!QFileInfo::exists(QDir(update).filePath(QStringLiteral("maintenance.lock"))));
}

void UpdaterTransactionTest::interruptedMixedSwapRecoversPreviousProgram()
{
    QTemporaryDir dir(QDir::current().filePath(QStringLiteral("tst-updater-interrupted-XXXXXX")));
    QVERIFY(dir.isValid());
    const QString update = QDir(dir.path()).filePath(QStringLiteral(".update"));
    const QString staging = QDir(update).filePath(QStringLiteral("staging"));
    const QString backup = QDir(update).filePath(QStringLiteral("backup"));
    QDir().mkpath(QDir(staging).filePath(QStringLiteral("app")));
    QDir().mkpath(QDir(dir.path()).filePath(QStringLiteral("app")));
    auto write = [](const QString &path, const QByteArray &bytes) {
        QFile file(path); return file.open(QIODevice::WriteOnly)
            && file.write(bytes) == bytes.size();
    };
    QVERIFY(write(QDir(dir.path()).filePath(QStringLiteral("GameHQ.exe")), "old root"));
    QVERIFY(write(QDir(dir.path()).filePath(QStringLiteral("app/GameHQ.exe")), "old app"));
    QVERIFY(write(QDir(staging).filePath(QStringLiteral("GameHQ.exe")), "new root"));
    QVERIFY(write(QDir(staging).filePath(QStringLiteral("app/GameHQ.exe")), "new app"));
    updater::Transaction tx;
    tx.packageRoot = dir.path().toStdWString(); tx.stagingDir = staging.toStdWString();
    tx.backupDir = backup.toStdWString();
    tx.dataDir = QDir(dir.path()).filePath(QStringLiteral("gamehq-data")).toStdWString();
    tx.dataSnapshotDir = QDir(update).filePath(QStringLiteral("data-snapshot")).toStdWString();
    tx.healthTokenPath = QDir(update).filePath(QStringLiteral("healthy.token")).toStdWString();
    tx.restartExecutable = QStringLiteral(HEALTH_FIXTURE_EXE).toStdWString();
    std::string error;
    QVERIFY2(updater::swapProgramFiles(tx, error), error.c_str());
    QVERIFY(QDir().rename(QDir(dir.path()).filePath(QStringLiteral("app")),
                          QDir(staging).filePath(QStringLiteral("app"))));
    QVERIFY(QDir().rename(QDir(backup).filePath(QStringLiteral("app")),
                          QDir(dir.path()).filePath(QStringLiteral("app"))));
    QVERIFY2(updater::recoverInterruptedUpdate(tx, error), error.c_str());
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(
        QDir(dir.path()).filePath(QStringLiteral("previous-started.token"))), 2000);
    auto read = [](const QString &path) { QFile file(path); file.open(QIODevice::ReadOnly); return file.readAll(); };
    QCOMPARE(read(QDir(dir.path()).filePath(QStringLiteral("GameHQ.exe"))), QByteArray("old root"));
    QCOMPARE(read(QDir(dir.path()).filePath(QStringLiteral("app/GameHQ.exe"))), QByteArray("old app"));
    QVERIFY(!QFileInfo::exists(backup));
}

void UpdaterTransactionTest::rejectsPackageRequiringNewerUpdater()
{
    QTemporaryDir dir(QDir::current().filePath(QStringLiteral("tst-updater-minimum-XXXXXX")));
    QVERIFY(dir.isValid());
    const QString transaction = writeFixture(
        dir.path(), QDir(dir.path()).filePath(QStringLiteral(".update/backup")));
    const QString package = QDir(dir.path()).filePath(
        QStringLiteral(".update/downloads/GameHQ-9.8.7-win64-update.zip"));
    const QByteArray manifest = R"({"schemaVersion":1,"productId":"underfusion.gamehq","appVersion":"9.8.7","layoutVersion":1,"minimumUpdaterVersion":"99.0.0"})";
    QVERIFY(writeZip(package, {{"GameHQ.exe", "launcher"}, {"app/GameHQ.exe", "application"},
                               {"update-package.json", manifest}}));
    QVERIFY(syncTransactionHash(transaction, package));
    QScopedPointer<QProcess> process(runHelper(QStringLiteral("--stage"), transaction, this));
    QVERIFY(process->waitForFinished(10000));
    QVERIFY(process->exitCode() != 0);
    QVERIFY(process->readAllStandardError().contains("requires a newer updater helper"));
    QVERIFY(!QFileInfo::exists(QDir(dir.path()).filePath(QStringLiteral(".update/staging"))));
}

void UpdaterTransactionTest::promotesOnlySelfTestingPendingHelper()
{
    QTemporaryDir good(QDir::current().filePath(QStringLiteral("tst-updater-promote-XXXXXX")));
    QVERIFY(good.isValid());
    const QString current = QDir(good.path()).filePath(QStringLiteral("GameHQUpdater.exe"));
    const QString pending = QDir(good.path()).filePath(QStringLiteral("GameHQUpdater.pending.exe"));
    QFile old(current); QVERIFY(old.open(QIODevice::WriteOnly)); QVERIFY(old.write("old") == 3); old.close();
    QVERIFY(QFile::copy(QStringLiteral(UPDATER_EXE), pending));
    QVERIFY(launcher::promotePendingUpdater(good.path().toStdWString()));
    QVERIFY(!QFileInfo::exists(pending));
    QVERIFY(QFileInfo(current).size() > 3);

    QTemporaryDir bad(QDir::current().filePath(QStringLiteral("tst-updater-reject-helper-XXXXXX")));
    QVERIFY(bad.isValid());
    const QString badCurrent = QDir(bad.path()).filePath(QStringLiteral("GameHQUpdater.exe"));
    const QString badPending = QDir(bad.path()).filePath(QStringLiteral("GameHQUpdater.pending.exe"));
    QFile existing(badCurrent); QVERIFY(existing.open(QIODevice::WriteOnly)); QVERIFY(existing.write("keep") == 4); existing.close();
    // A real executable that deliberately rejects --self-test avoids involving
    // Windows' slow invalid-image compatibility path while testing the same gate.
    QVERIFY(QFile::copy(QStringLiteral(HEALTH_FIXTURE_EXE), badPending));
    QVERIFY(!launcher::promotePendingUpdater(bad.path().toStdWString()));
    QVERIFY(!QFileInfo::exists(badPending));
    QFile preserved(badCurrent); QVERIFY(preserved.open(QIODevice::ReadOnly));
    QCOMPARE(preserved.readAll(), QByteArray("keep"));
}

void UpdaterTransactionTest::completeApplyCleansStaleStagingAndPreservesUserData()
{
    QTemporaryDir dir(QDir::current().filePath(QStringLiteral("tst-updater-apply-XXXXXX")));
    QVERIFY(dir.isValid());
    const QString update = QDir(dir.path()).filePath(QStringLiteral(".update"));
    const QString backup = QDir(update).filePath(QStringLiteral("backup"));
    const QString transactionPath = writeFixture(dir.path(), backup);
    QVERIFY(!transactionPath.isEmpty());
    const QString data = QDir(dir.path()).filePath(QStringLiteral("gamehq-data"));
    const QString captures = QDir(dir.path()).filePath(QStringLiteral("Captures"));
    QDir().mkpath(captures);
    auto write = [](const QString &path, const QByteArray &bytes) {
        QFile file(path); return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
            && file.write(bytes) == bytes.size();
    };
    QVERIFY(write(QDir(dir.path()).filePath(QStringLiteral("GameHQ.exe")), "old root"));
    QVERIFY(write(QDir(data).filePath(QStringLiteral("config.json")), "settings"));
    QVERIFY(write(QDir(captures).filePath(QStringLiteral("clip.mp4")), "user media"));
    const QString staging = QDir(update).filePath(QStringLiteral("staging"));
    QDir().mkpath(staging);
    QVERIFY(write(QDir(staging).filePath(QStringLiteral("abandoned.tmp")), "stale"));
    QFile fixture(QStringLiteral(HEALTH_FIXTURE_EXE));
    QVERIFY(fixture.open(QIODevice::ReadOnly));
    const QByteArray newLauncher = fixture.readAll();
    QVERIFY(!newLauncher.isEmpty());
    const QString package = QDir(update).filePath(
        QStringLiteral("downloads/GameHQ-9.8.7-win64-update.zip"));
    const QByteArray manifest = R"({"schemaVersion":1,"productId":"underfusion.gamehq","appVersion":"9.8.7","layoutVersion":1,"minimumUpdaterVersion":"0.6.10"})";
    QVERIFY(writeZip(package, {{"GameHQ.exe", newLauncher}, {"app/GameHQ.exe", "new app"},
                               {"update-package.json", manifest}}));
    QVERIFY(syncTransactionHash(transactionPath, package));
    updater::Transaction tx;
    std::string error;
    QVERIFY2(updater::loadAndValidateTransaction(transactionPath.toStdWString(), tx, error), error.c_str());
    QVERIFY2(maintenance::begin(dir.path().toStdWString(), error), error.c_str());
    QVERIFY2(updater::applyUpdate(tx, 5000, error), error.c_str());
    auto read = [](const QString &path) { QFile file(path); file.open(QIODevice::ReadOnly); return file.readAll(); };
    QVERIFY(!QFileInfo::exists(QDir(dir.path()).filePath(QStringLiteral("portable.flag"))));
    QCOMPARE(read(QDir(data).filePath(QStringLiteral("config.json"))), QByteArray("settings"));
    QCOMPARE(read(QDir(captures).filePath(QStringLiteral("clip.mp4"))), QByteArray("user media"));
    QVERIFY(!QFileInfo::exists(QDir(staging).filePath(QStringLiteral("abandoned.tmp"))));
    QCOMPARE(read(QDir(update).filePath(QStringLiteral("transaction.phase"))).trimmed(), QByteArray("healthy"));
    QVERIFY(!QFileInfo::exists(QDir(update).filePath(QStringLiteral("maintenance.lock"))));
    QCOMPARE(read(QDir(backup).filePath(QStringLiteral("GameHQ.exe"))), QByteArray("old root"));
}


void UpdaterTransactionTest::bindsHandoffToTheExactAuthorisingProcess()
{
    QTemporaryDir dir(QDir::current().filePath(QStringLiteral("tst-updater-handoff-XXXXXX")));
    QVERIFY(dir.isValid());
    const QString update = QDir(dir.path()).filePath(QStringLiteral(".update"));
    const QString transactionPath = writeFixture(
        dir.path(), QDir(update).filePath(QStringLiteral("backup")));
    QVERIFY(!transactionPath.isEmpty());

    const auto rewrite = [&transactionPath](auto mutate) {
        QFile file(transactionPath);
        if (!file.open(QIODevice::ReadOnly))
            return false;
        QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        mutate(object);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return false;
        return file.write(QJsonDocument(object).toJson(QJsonDocument::Compact)) > 0;
    };
    const auto load = [&transactionPath](updater::Transaction &tx, std::string &error) {
        return updater::loadAndValidateTransaction(transactionPath.toStdWString(), tx, error);
    };

    // The fixture names this process with its real creation time, so the helper
    // can open exactly it.
    updater::Transaction tx;
    std::string error;
    QVERIFY2(load(tx, error), error.c_str());
    QCOMPARE(tx.schemaVersion, 2);
    QVERIFY(tx.callerCreationTime != 0);
    bool alreadyExited = false;
    void *handle = updater::openAuthorisingCaller(tx, alreadyExited, error);
    QVERIFY2(handle != nullptr, error.c_str());
    QVERIFY(!alreadyExited);
    // The handle must be the live caller, so waiting on it times out.
    QCOMPARE(WaitForSingleObject(handle, 0), static_cast<DWORD>(WAIT_TIMEOUT));
    CloseHandle(handle);

    // Same process id, wrong creation time: this is the PID-reuse case, and it
    // must never resolve to a handle.
    QVERIFY(rewrite([&](QJsonObject &object) {
        object.insert(QStringLiteral("callerCreationTime"),
                      static_cast<qint64>(tx.callerCreationTime - 1));
    }));
    updater::Transaction reused;
    QVERIFY(load(reused, error));
    QVERIFY(updater::openAuthorisingCaller(reused, alreadyExited, error) == nullptr);
    QVERIFY(!alreadyExited);
    QVERIFY(!error.empty());

    // A process id that cannot exist is reported as an exited caller, not as a
    // failure: there is nothing left that could hold files open.
    QVERIFY(rewrite([&](QJsonObject &object) {
        object.insert(QStringLiteral("callerCreationTime"),
                      static_cast<qint64>(tx.callerCreationTime));
        object.insert(QStringLiteral("callerPid"), static_cast<qint64>(0x7ffffff0));
    }));
    updater::Transaction gone;
    error.clear();
    QVERIFY2(load(gone, error), error.c_str());
    QVERIFY(updater::openAuthorisingCaller(gone, alreadyExited, error) == nullptr);
    QVERIFY(alreadyExited);

    // A missing or zero creation time is refused outright at schema 2.
    QVERIFY(rewrite([](QJsonObject &object) {
        object.remove(QStringLiteral("callerCreationTime"));
    }));
    updater::Transaction missing;
    QVERIFY(!load(missing, error));

    // Schema 1 still loads, but no mutating mode may run it.
    QVERIFY(rewrite([](QJsonObject &object) {
        object.remove(QStringLiteral("callerCreationTime"));
        object.insert(QStringLiteral("schemaVersion"), 1);
    }));
    updater::Transaction legacy;
    QVERIFY2(load(legacy, error), error.c_str());
    QCOMPARE(legacy.schemaVersion, 1);
    QVERIFY(updater::openAuthorisingCaller(legacy, alreadyExited, error) == nullptr);
    QScopedPointer<QProcess> staged(runHelper(QStringLiteral("--stage"), transactionPath, this));
    QVERIFY(staged->waitForFinished(10000));
    QVERIFY(staged->exitCode() != 0);
    QVERIFY(staged->readAllStandardError().contains("run the update again"));
    QVERIFY(!QFileInfo::exists(QDir(update).filePath(QStringLiteral("staging"))));
}

// Every case above drives updater_test_trust_fixture, which differs from the
// shipped helper in exactly one way: it links the trust library built with the
// public RFC 8032 vector key. This case closes that gap from the other side -
// the helper the release actually ships must refuse the same evidence, and must
// not even list a test key. Both binaries are built from one source list in one
// build tree, so this also keeps the packaged build directory the tested one.
void UpdaterTransactionTest::shippedHelperRefusesTestTrustMaterial()
{
    QTemporaryDir dir(QDir::current().filePath(QStringLiteral("tst-updater-shipped-XXXXXX")));
    QVERIFY(dir.isValid());
    const QString backup = QDir(dir.path()).filePath(QStringLiteral(".update/backup"));
    const QString transaction = writeFixture(dir.path(), backup);
    QVERIFY(!transaction.isEmpty());

    QProcess helper;
    helper.start(QStringLiteral(PRODUCTION_UPDATER_EXE),
                 { QStringLiteral("--dry-run"), transaction });
    QVERIFY(helper.waitForFinished(10000));
    QVERIFY(helper.exitCode() != 0);
    const QByteArray errors = helper.readAllStandardError();
    QVERIFY2(errors.contains("no trusted key signs this release manifest"), errors.constData());

    QProcess report;
    report.start(QStringLiteral(PRODUCTION_UPDATER_EXE),
                 { QStringLiteral("--release-trust-self-test") });
    QVERIFY(report.waitForFinished(10000));
    QCOMPARE(report.exitCode(), 0);
    const QByteArray table = report.readAllStandardOutput();
    QVERIFY2(table.contains("TRUST TABLE production"), table.constData());
    QVERIFY2(!table.contains("TRUSTED KEY gamehq-test-"), table.constData());
}

QTEST_GUILESS_MAIN(UpdaterTransactionTest)
#include "tst_updatertransaction.moc"
