#include "config/PortableProfileImporter.h"
#include "localization/NativeText.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

namespace
{
QString databaseOperationError(const QString &detail)
{
    return NativeText::get(
        //: Portable-profile import database failure; %1 is the unchanged database detail.
        //% "A portable-profile database operation failed: %1"
        QT_TRID_NOOP("gamehq.error.portable_import.database_operation_failed"),
        "A portable-profile database operation failed: %1").arg(detail);
}

constexpr int SupportedSchemaVersion = 3;

QString normalizedAbsolute(const QString& path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

bool isWithin(const QString& root, const QString& path)
{
    const QString relative = QDir(root).relativeFilePath(path);
    return relative == QStringLiteral(".")
        || (relative != QStringLiteral("..")
            && !relative.startsWith(QStringLiteral("../"))
            && !QDir::isAbsolutePath(relative));
}

bool validateExistingCanonicalPath(const QString& root, const QString& path, QString& error)
{
    QFileInfo rootInfo(root);
    const QString canonicalRoot = rootInfo.canonicalFilePath();
    if (canonicalRoot.isEmpty()) {
        error = NativeText::get(
            //: Portable-profile import validation failure shown in the native startup dialog.
            //% "The portable package root cannot be canonicalized."
            QT_TRID_NOOP("gamehq.error.portable_import.package_root_canonicalize_failed"),
            "The portable package root cannot be canonicalized.");
        return false;
    }

    QFileInfo candidate(path);
    while (!candidate.exists()) {
        const QString parent = candidate.absolutePath();
        if (parent == candidate.absoluteFilePath())
            break;
        candidate.setFile(parent);
    }
    const QString canonicalCandidate = candidate.canonicalFilePath();
    if (canonicalCandidate.isEmpty() || !isWithin(canonicalRoot, canonicalCandidate)) {
        error = NativeText::get(
            //: Portable-profile import path validation failure shown in the native startup dialog.
            //% "A portable path escapes the selected package root."
            QT_TRID_NOOP("gamehq.error.portable_import.path_escapes_package"),
            "A portable path escapes the selected package root.");
        return false;
    }
    return true;
}

bool resolvePortablePath(const QString& value, const QString& sourceRoot,
                         QString& resolved, QString& error)
{
    const QString clean = QDir::fromNativeSeparators(value.trimmed());
    if (clean.isEmpty()) {
        resolved.clear();
        return true;
    }
    if (!clean.startsWith(QStringLiteral("portable:/"), Qt::CaseInsensitive)) {
        if (QDir::isRelativePath(clean)) {
            error = NativeText::get(
                //: Portable-profile import path validation failure; keep portable:/ unchanged.
                //% "Imported paths must be absolute or use portable:/."
                QT_TRID_NOOP("gamehq.error.portable_import.path_scheme_required"),
                "Imported paths must be absolute or use portable:/.");
            return false;
        }
        resolved = QDir::cleanPath(clean);
        return true;
    }

    const QString relative = clean.mid(10);
    if (relative.isEmpty() || QDir::isAbsolutePath(relative)) {
        error = NativeText::get(
            //: Portable-profile import path validation failure; keep portable:/ unchanged.
            //% "A portable:/ path is empty or absolute."
            QT_TRID_NOOP("gamehq.error.portable_import.path_invalid"),
            "A portable:/ path is empty or absolute.");
        return false;
    }
    const QString candidate = QDir::cleanPath(sourceRoot + QLatin1Char('/') + relative);
    if (!isWithin(sourceRoot, candidate)
        || !validateExistingCanonicalPath(sourceRoot, candidate, error)) {
        if (error.isEmpty())
            error = NativeText::get(
                //: Portable-profile import path validation failure; keep portable:/ unchanged.
                //% "A portable:/ path escapes the selected package root."
                QT_TRID_NOOP("gamehq.error.portable_import.portable_path_escapes_package"),
                "A portable:/ path escapes the selected package root.");
        return false;
    }
    resolved = candidate;
    return true;
}

bool containsPortableString(const QJsonValue& value)
{
    if (value.isString())
        return QDir::fromNativeSeparators(value.toString().trimmed())
            .startsWith(QStringLiteral("portable:/"), Qt::CaseInsensitive);
    if (value.isArray()) {
        for (const QJsonValue& child : value.toArray())
            if (containsPortableString(child))
                return true;
    }
    if (value.isObject()) {
        for (const QJsonValue& child : value.toObject())
            if (containsPortableString(child))
                return true;
    }
    return false;
}

QByteArray sha256File(const QString& path, QString& error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = NativeText::get(
            //: Portable-profile import filesystem failure; %1 is an unchanged file name.
            //% "Cannot read %1 for source verification."
            QT_TRID_NOOP("gamehq.error.portable_import.source_read_failed"),
            "Cannot read %1 for source verification.").arg(QFileInfo(path).fileName());
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        error = NativeText::get(
            //: Portable-profile import integrity failure; %1 is an unchanged file name.
            //% "Cannot hash %1."
            QT_TRID_NOOP("gamehq.error.portable_import.hash_failed"),
            "Cannot hash %1.").arg(QFileInfo(path).fileName());
        return {};
    }
    return hash.result();
}

bool writeJson(const QString& path, const QJsonObject& object, QString& error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        error = NativeText::get(
            //: Portable-profile import filesystem failure; %1 is an unchanged file name.
            //% "Cannot stage %1."
            QT_TRID_NOOP("gamehq.error.portable_import.stage_file_failed"),
            "Cannot stage %1.").arg(QFileInfo(path).fileName());
        return false;
    }
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        error = NativeText::get(
            //: Portable-profile import filesystem failure; %1 is an unchanged file name.
            //% "Cannot commit staged %1."
            QT_TRID_NOOP("gamehq.error.portable_import.commit_staged_file_failed"),
            "Cannot commit staged %1.").arg(QFileInfo(path).fileName());
        return false;
    }
    return true;
}

QString transactionPathFor(const QString& destinationRoot)
{
    const QFileInfo destination(destinationRoot);
    return QDir(destination.absolutePath()).filePath(
        QStringLiteral(".%1.import-transaction.json").arg(destination.fileName()));
}

bool removeTree(const QString& path, QString& error)
{
    if (!QFileInfo::exists(path) || QDir(path).removeRecursively())
        return true;
    error = NativeText::get(
        //: Portable-profile import recovery filesystem failure.
        //% "Portable-import recovery could not remove a transaction directory."
        QT_TRID_NOOP("gamehq.error.portable_import.recovery_remove_directory_failed"),
        "Portable-import recovery could not remove a transaction directory.");
    return false;
}

bool writeTransaction(const QString& path, const QString& phase,
                      const QString& stageName, const QString& backupName,
                      QString& error)
{
    return writeJson(path, QJsonObject {
        { QStringLiteral("schemaVersion"), 1 },
        { QStringLiteral("phase"), phase },
        { QStringLiteral("stageName"), stageName },
        { QStringLiteral("backupName"), backupName }
    }, error);
}

bool recoverInterruptedTransaction(const QString& destinationRoot, QString& error)
{
    const QString transactionPath = transactionPathFor(destinationRoot);
    if (!QFileInfo::exists(transactionPath))
        return true;

    QFile file(transactionPath);
    if (!file.open(QIODevice::ReadOnly)) {
        error = NativeText::get(
            //: Portable-profile import recovery failure.
            //% "The interrupted portable-import journal cannot be read."
            QT_TRID_NOOP("gamehq.error.portable_import.journal_read_failed"),
            "The interrupted portable-import journal cannot be read.");
        return false;
    }
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    file.close();
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        error = NativeText::get(
            //: Portable-profile import recovery failure.
            //% "The interrupted portable-import journal is malformed."
            QT_TRID_NOOP("gamehq.error.portable_import.journal_malformed"),
            "The interrupted portable-import journal is malformed.");
        return false;
    }
    const QJsonObject journal = document.object();
    const QFileInfo destination(destinationRoot);
    const QString leaf = destination.fileName();
    const QString stageName = journal.value(QStringLiteral("stageName")).toString();
    const QString backupName = journal.value(QStringLiteral("backupName")).toString();
    const QString phase = journal.value(QStringLiteral("phase")).toString();
    const QString stagePrefix = QStringLiteral(".%1.import-stage-").arg(leaf);
    const QString backupPrefix = QStringLiteral(".%1.import-backup-").arg(leaf);
    if (journal.value(QStringLiteral("schemaVersion")).toInt() != 1
        || QFileInfo(stageName).fileName() != stageName || !stageName.startsWith(stagePrefix)
        || QFileInfo(backupName).fileName() != backupName || !backupName.startsWith(backupPrefix)
        || (phase != QStringLiteral("staging")
            && phase != QStringLiteral("destination-backed-up")
            && phase != QStringLiteral("published"))) {
        error = NativeText::get(
            //: Portable-profile import recovery failure.
            //% "The interrupted portable-import journal is invalid."
            QT_TRID_NOOP("gamehq.error.portable_import.journal_invalid"),
            "The interrupted portable-import journal is invalid.");
        return false;
    }

    const QDir parent(destination.absolutePath());
    const QString stageRoot = parent.filePath(stageName);
    const QString backupRoot = parent.filePath(backupName);
    const bool destinationExists = QFileInfo::exists(destinationRoot);
    const bool backupExists = QFileInfo::exists(backupRoot);

    if (phase == QStringLiteral("published")) {
        if (!destinationExists) {
            error = NativeText::get(
                //: Portable-profile import recovery failure.
                //% "The published portable-import destination is missing."
                QT_TRID_NOOP("gamehq.error.portable_import.published_destination_missing"),
                "The published portable-import destination is missing.");
            return false;
        }
        if (!removeTree(backupRoot, error) || !removeTree(stageRoot, error))
            return false;
    } else if (backupExists) {
        if (destinationExists && !removeTree(destinationRoot, error))
            return false;
        if (!QDir().rename(backupRoot, destinationRoot)) {
            error = NativeText::get(
                //: Portable-profile import recovery failure.
                //% "The interrupted portable import could not restore its destination backup."
                QT_TRID_NOOP("gamehq.error.portable_import.destination_restore_failed"),
                "The interrupted portable import could not restore its destination backup.");
            return false;
        }
        if (!removeTree(stageRoot, error))
            return false;
    } else {
        if (phase == QStringLiteral("destination-backed-up") || !destinationExists) {
            error = NativeText::get(
                //: Portable-profile import recovery failure.
                //% "The interrupted portable-import backup is missing."
                QT_TRID_NOOP("gamehq.error.portable_import.backup_missing"),
                "The interrupted portable-import backup is missing.");
            return false;
        }
        if (!removeTree(stageRoot, error))
            return false;
    }

    if (!QFile::remove(transactionPath) && QFileInfo::exists(transactionPath)) {
        error = NativeText::get(
            //: Portable-profile import recovery filesystem failure.
            //% "The recovered portable-import journal could not be removed."
            QT_TRID_NOOP("gamehq.error.portable_import.journal_remove_failed"),
            "The recovered portable-import journal could not be removed.");
        return false;
    }
    return true;
}

bool readConfig(const QString& path, QJsonObject& object, QString& error)
{
    QFile file(path);
    if (!file.exists()) {
        object = {};
        return true;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        error = NativeText::get(
            //: Portable-profile import configuration failure; keep config.json unchanged.
            //% "Cannot read the portable config.json."
            QT_TRID_NOOP("gamehq.error.portable_import.config_read_failed"),
            "Cannot read the portable config.json.");
        return false;
    }
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        error = NativeText::get(
            //: Portable-profile import configuration failure; keep config.json unchanged.
            //% "The portable config.json is malformed."
            QT_TRID_NOOP("gamehq.error.portable_import.config_malformed"),
            "The portable config.json is malformed.");
        return false;
    }
    object = document.object();
    return true;
}

bool rewriteConfig(QJsonObject& config, const QString& sourceRoot, QString& error)
{
    const QStringList pathKeys = {
        QStringLiteral("storage.screenshots_root"),
        QStringLiteral("storage.clips_root")
    };
    for (const QString& key : pathKeys) {
        if (!config.contains(key))
            continue;
        if (!config.value(key).isString()) {
            error = NativeText::get(
                //: Portable-profile import configuration failure; %1 is an unchanged persisted key.
                //% "%1 must be a string."
                QT_TRID_NOOP("gamehq.error.portable_import.config_value_not_string"),
                "%1 must be a string.").arg(key);
            return false;
        }
        QString resolved;
        if (!resolvePortablePath(config.value(key).toString(), sourceRoot, resolved, error))
            return false;
        config.insert(key, resolved);
    }

    const QString historyKey = QStringLiteral("internal.capture_root_history");
    QJsonArray rewrittenHistory;
    QSet<QString> seen;
    if (config.contains(historyKey)) {
        if (!config.value(historyKey).isArray()) {
            error = NativeText::get(
                //: Portable-profile import configuration failure; %1 is an unchanged persisted key.
                //% "%1 must be an array."
                QT_TRID_NOOP("gamehq.error.portable_import.config_value_not_array"),
                "%1 must be an array.").arg(historyKey);
            return false;
        }
        for (const QJsonValue& entry : config.value(historyKey).toArray()) {
            if (!entry.isString()) {
                error = NativeText::get(
                    //: Portable-profile import configuration validation failure.
                    //% "Capture-root history contains a non-string value."
                    QT_TRID_NOOP("gamehq.error.portable_import.capture_history_value_invalid"),
                    "Capture-root history contains a non-string value.");
                return false;
            }
            QString resolved;
            if (!resolvePortablePath(entry.toString(), sourceRoot, resolved, error))
                return false;
            if (resolved.isEmpty())
                continue;
            const QString folded = resolved.toCaseFolded();
            if (!seen.contains(folded)) {
                seen.insert(folded);
                rewrittenHistory.append(resolved);
            }
        }
    }
    const QString oldCaptures = QDir::cleanPath(sourceRoot + QStringLiteral("/Captures"));
    if (!seen.contains(oldCaptures.toCaseFolded()))
        rewrittenHistory.append(oldCaptures);
    config.insert(historyKey, rewrittenHistory);

    for (auto it = config.constBegin(); it != config.constEnd(); ++it) {
        if (containsPortableString(it.value())) {
            error = NativeText::get(
                //: Portable-profile import configuration failure; %1 is an unchanged persisted key.
                //% "Unsupported portable path in config key %1."
                QT_TRID_NOOP("gamehq.error.portable_import.config_path_unsupported"),
                "Unsupported portable path in config key %1.").arg(it.key());
            return false;
        }
    }
    return true;
}

bool destinationIsEmpty(const QString& root, QString& error)
{
    const QFileInfo rootInfo(root);
    if (rootInfo.exists() && rootInfo.isSymLink()) {
        error = NativeText::get(
            //: Portable-profile import destination safety failure.
            //% "The installed profile root must not be a symbolic link or junction."
            QT_TRID_NOOP("gamehq.error.portable_import.destination_root_link"),
            "The installed profile root must not be a symbolic link or junction.");
        return false;
    }
    QDir directory(root);
    if (!directory.exists())
        return true;

    // GameHQ refills these itself from the captures and the database, so
    // whatever they hold is a cache the import may replace.
    const QSet<QString> regenerableDirectories = {
        QStringLiteral("logs"), QStringLiteral("thumbnails"), QStringLiteral("game-icons"),
        QStringLiteral("replay-cache")
    };
    // Nothing seeds sound-packs — every file in it was put there by the user,
    // and the import writes into that same folder. Allowing the directory
    // without looking inside it is how custom sounds got overwritten.
    const QSet<QString> mustBeEmptyDirectories = { QStringLiteral("sound-packs") };
    const QSet<QString> allowedFiles = {
        QStringLiteral("config.json"), QStringLiteral("gamehq.db"),
        QStringLiteral("import-evidence.json")
    };
    for (const QFileInfo& entry : directory.entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries)) {
        if (entry.isSymLink()) {
            error = NativeText::get(
                        //: Portable-profile import destination safety failure; %1 is an unchanged path.
                        //% "The installed profile contains a symbolic link or junction: %1."
                        QT_TRID_NOOP("gamehq.error.portable_import.destination_contains_link"),
                        "The installed profile contains a symbolic link or junction: %1.")
                .arg(entry.fileName());
            return false;
        }
        if (entry.isDir() && mustBeEmptyDirectories.contains(entry.fileName())) {
            if (!QDir(entry.absoluteFilePath()).isEmpty(QDir::NoDotAndDotDot | QDir::AllEntries)) {
                error = NativeText::get(
                            //: Portable-profile import destination validation failure; %1 is an unchanged file name.
                            //% "The installed profile already has its own %1."
                            QT_TRID_NOOP("gamehq.error.portable_import.destination_file_exists"),
                            "The installed profile already has its own %1.")
                    .arg(entry.fileName());
                return false;
            }
            continue;
        }
        if ((entry.isDir() && regenerableDirectories.contains(entry.fileName()))
            || (entry.isFile() && allowedFiles.contains(entry.fileName())))
            continue;
        error = NativeText::get(
            //: Portable-profile import destination validation failure; %1 is an unchanged file name.
            //% "The installed profile contains unsupported data: %1."
            QT_TRID_NOOP("gamehq.error.portable_import.destination_data_unsupported"),
            "The installed profile contains unsupported data: %1.").arg(entry.fileName());
        return false;
    }

    const QString configPath = directory.filePath(QStringLiteral("config.json"));
    if (QFileInfo::exists(configPath)) {
        QJsonObject config;
        if (!readConfig(configPath, config, error))
            return false;
        for (auto it = config.constBegin(); it != config.constEnd(); ++it) {
            // A first launch necessarily records window/category state and may
            // record internal update metadata before the user reaches Import.
            // Neither represents library/profile content and both are replaced.
            if (!it.key().startsWith(QStringLiteral("ui."))
                && !it.key().startsWith(QStringLiteral("internal."))) {
                error = NativeText::get(
                    //: Portable-profile import destination validation failure.
                    //% "The installed profile has non-default configuration."
                    QT_TRID_NOOP("gamehq.error.portable_import.destination_config_not_default"),
                    "The installed profile has non-default configuration.");
                return false;
            }
        }
    }

    const QString databasePath = directory.filePath(QStringLiteral("gamehq.db"));
    if (!QFileInfo::exists(databasePath))
        return true;
    const QString connection = QStringLiteral("destination-empty-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    bool empty = true;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(databasePath);
        if (!db.open()) {
            error = NativeText::get(
                //: Portable-profile import destination database failure.
                //% "The installed database cannot be inspected."
                QT_TRID_NOOP("gamehq.error.portable_import.destination_database_inspection_failed"),
                "The installed database cannot be inspected.");
            empty = false;
        } else {
            // Every table the database actually has, not a fixed list: a table
            // a newer schema adds would otherwise carry the user's data
            // straight past this check and be overwritten.
            //
            // Two are exempt because a first launch fills them on its own:
            // `settings` holds internal.* sentinels written by the startup
            // repair, and `bindings` is the legacy table seeded with the
            // built-in defaults (the editor writes `binding_overrides`, which
            // is checked like everything else).
            for (const QString& table : db.tables()) {
                if (table.startsWith(QStringLiteral("sqlite_"), Qt::CaseInsensitive)
                    || table == QStringLiteral("bindings"))
                    continue;
                if (table == QStringLiteral("settings")) {
                    QSqlQuery keys(db);
                    if (!keys.exec(QStringLiteral("SELECT key FROM settings"))) {
                        error = NativeText::get(
                            //% "The installed database cannot be inspected."
                            QT_TRID_NOOP("gamehq.error.portable_import.destination_database_inspection_failed"),
                            "The installed database cannot be inspected.");
                        empty = false;
                        break;
                    }
                    while (keys.next()) {
                        if (keys.value(0).toString().startsWith(QStringLiteral("internal.")))
                            continue;
                        error = NativeText::get(
                            //: Portable-profile import destination validation failure.
                            //% "The installed profile is not empty."
                            QT_TRID_NOOP("gamehq.error.portable_import.destination_not_empty"),
                            "The installed profile is not empty.");
                        empty = false;
                        break;
                    }
                    if (!empty)
                        break;
                    continue;
                }
                QSqlQuery query(db);
                if (!query.exec(QStringLiteral("SELECT 1 FROM \"%1\" LIMIT 1").arg(table))
                    || query.next()) {
                    error = NativeText::get(
                        //% "The installed profile is not empty."
                        QT_TRID_NOOP("gamehq.error.portable_import.destination_not_empty"),
                        "The installed profile is not empty.");
                    empty = false;
                    break;
                }
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connection);
    return empty;
}

bool execSql(QSqlQuery& query, const QString& sql, QString& error)
{
    if (query.exec(sql))
        return true;
    error = databaseOperationError(query.lastError().text());
    return false;
}

bool rewriteColumn(QSqlDatabase& db, const QString& selectSql, const QString& updateSql,
                   const QString& sourceRoot, bool rejectPortable, int& count, QString& error)
{
    QSqlQuery select(db);
    if (!execSql(select, selectSql, error))
        return false;
    struct Row { qlonglong id; QString value; };
    QList<Row> rows;
    while (select.next())
        rows.append({ select.value(0).toLongLong(), select.value(1).toString() });
    for (const Row& row : rows) {
        const QString normalized = QDir::fromNativeSeparators(row.value.trimmed());
        if (rejectPortable && normalized.startsWith(QStringLiteral("portable:/"), Qt::CaseInsensitive)) {
            error = NativeText::get(
                //: Portable-profile import database path failure; keep portable:/ unchanged.
                //% "A game executable path incorrectly uses portable:/."
                QT_TRID_NOOP("gamehq.error.portable_import.game_path_scheme_invalid"),
                "A game executable path incorrectly uses portable:/.");
            return false;
        }
        QString resolved;
        if (!resolvePortablePath(row.value, sourceRoot, resolved, error))
            return false;
        QSqlQuery update(db);
        update.prepare(updateSql);
        update.bindValue(QStringLiteral(":value"), resolved);
        update.bindValue(QStringLiteral(":id"), row.id);
        if (!update.exec()) {
            error = databaseOperationError(update.lastError().text());
            return false;
        }
        ++count;
    }
    return true;
}

bool rewriteFolders(QSqlDatabase& db, const QString& sourceRoot, int& count, QString& error)
{
    QSqlQuery select(db);
    if (!execSql(select, QStringLiteral("SELECT id, path FROM folders ORDER BY id"), error))
        return false;
    struct Row { qlonglong id; QString value; };
    QList<Row> rows;
    while (select.next())
        rows.append({ select.value(0).toLongLong(), select.value(1).toString() });
    QSet<QString> seen;
    QList<Row> uniqueRows;
    QHash<qlonglong, QString> resolvedById;
    QList<qlonglong> duplicateIds;
    for (const Row& row : rows) {
        QString resolved;
        if (!resolvePortablePath(row.value, sourceRoot, resolved, error))
            return false;
        const QString folded = resolved.toCaseFolded();
        if (seen.contains(folded)) {
            duplicateIds.append(row.id);
        } else {
            seen.insert(folded);
            uniqueRows.append(row);
            resolvedById.insert(row.id, resolved);
            ++count;
        }
    }
    for (qlonglong id : duplicateIds) {
        QSqlQuery change(db);
        change.prepare(QStringLiteral("DELETE FROM folders WHERE id = :id"));
        change.bindValue(QStringLiteral(":id"), id);
        if (!change.exec()) {
            error = databaseOperationError(change.lastError().text());
            return false;
        }
    }
    for (const Row& row : uniqueRows) {
        QSqlQuery clear(db);
        clear.prepare(QStringLiteral("UPDATE folders SET path = :temporary WHERE id = :id"));
        clear.bindValue(QStringLiteral(":temporary"), QStringLiteral("import-pending:%1").arg(row.id));
        clear.bindValue(QStringLiteral(":id"), row.id);
        if (!clear.exec()) {
            error = databaseOperationError(clear.lastError().text());
            return false;
        }
    }
    for (const Row& row : uniqueRows) {
        QSqlQuery change(db);
        change.prepare(QStringLiteral("UPDATE folders SET path = :value WHERE id = :id"));
        change.bindValue(QStringLiteral(":value"), resolvedById.value(row.id));
        change.bindValue(QStringLiteral(":id"), row.id);
        if (!change.exec()) {
            error = databaseOperationError(change.lastError().text());
            return false;
        }
    }
    return true;
}

bool copyReferencedSounds(QSqlDatabase& db, const QString& sourceRoot,
                          const QString& stageRoot, const QString& destinationRoot,
                          int& count, QString& error)
{
    QSqlQuery select(db);
    if (!execSql(select, QStringLiteral(
        "SELECT id, sound_file FROM sound_settings WHERE sound_file IS NOT NULL AND sound_file <> ''"), error))
        return false;
    struct Row { qlonglong id; QString value; };
    QList<Row> rows;
    while (select.next())
        rows.append({ select.value(0).toLongLong(), select.value(1).toString() });
    const QString sourceSounds = QDir::cleanPath(sourceRoot + QStringLiteral("/gamehq-data/sound-packs"));
    for (const Row& row : rows) {
        QString resolved;
        if (!resolvePortablePath(row.value, sourceRoot, resolved, error))
            return false;
        QString finalValue = resolved;
        if (isWithin(sourceSounds, resolved)) {
            if (!QFileInfo(resolved).isFile()) {
                error = NativeText::get(
                    //: Portable-profile import referenced-file failure.
                    //% "A referenced portable sound file is missing."
                    QT_TRID_NOOP("gamehq.error.portable_import.sound_file_missing"),
                    "A referenced portable sound file is missing.");
                return false;
            }
            const QString relative = QDir(sourceSounds).relativeFilePath(resolved);
            const QString stagedFile = QDir::cleanPath(stageRoot + QStringLiteral("/sound-packs/") + relative);
            finalValue = QDir::cleanPath(destinationRoot + QStringLiteral("/sound-packs/") + relative);
            if (!QDir().mkpath(QFileInfo(stagedFile).absolutePath())
                || !QFile::copy(resolved, stagedFile)) {
                error = NativeText::get(
                    //: Portable-profile import referenced-file failure.
                    //% "A referenced sound file could not be staged."
                    QT_TRID_NOOP("gamehq.error.portable_import.sound_file_stage_failed"),
                    "A referenced sound file could not be staged.");
                return false;
            }
            ++count;
        }
        QSqlQuery update(db);
        update.prepare(QStringLiteral("UPDATE sound_settings SET sound_file = :value WHERE id = :id"));
        update.bindValue(QStringLiteral(":value"), finalValue);
        update.bindValue(QStringLiteral(":id"), row.id);
        if (!update.exec()) {
            error = databaseOperationError(update.lastError().text());
            return false;
        }
    }
    return true;
}

bool rewriteDatabase(const QString& databasePath, const QString& sourceRoot,
                     const QString& stageRoot, const QString& destinationRoot,
                     PortableProfileImporter::Result& result, QString& error)
{
    const QString connection = QStringLiteral("portable-import-%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    bool success = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(databasePath);
        if (!db.open()) {
            error = NativeText::get(
                //: Portable-profile import database failure; %1 is the unchanged database detail.
                //% "The staged portable database cannot be opened: %1"
                QT_TRID_NOOP("gamehq.error.portable_import.database_open_failed"),
                "The staged portable database cannot be opened: %1").arg(db.lastError().text());
        } else {
            QSqlQuery query(db);
            if (!query.exec(QStringLiteral("PRAGMA user_version")) || !query.next()) {
                error = NativeText::get(
                    //: Portable-profile import database validation failure.
                    //% "The portable database schema cannot be read."
                    QT_TRID_NOOP("gamehq.error.portable_import.database_schema_read_failed"),
                    "The portable database schema cannot be read.");
            } else if (query.value(0).toInt() < 1 || query.value(0).toInt() > SupportedSchemaVersion) {
                error = NativeText::get(
                    //: Portable-profile import database validation failure.
                    //% "The portable database schema is unsupported."
                    QT_TRID_NOOP("gamehq.error.portable_import.database_schema_unsupported"),
                    "The portable database schema is unsupported.");
            } else if (!db.transaction()) {
                error = NativeText::get(
                    //: Portable-profile import database failure.
                    //% "The portable database transaction cannot begin."
                    QT_TRID_NOOP("gamehq.error.portable_import.database_transaction_failed"),
                    "The portable database transaction cannot begin.");
            } else {
                int captureCount = 0;
                int gameCount = 0;
                int folderCount = 0;
                bool ok = rewriteColumn(db,
                    QStringLiteral("SELECT id, file_path FROM captures"),
                    QStringLiteral("UPDATE captures SET file_path = :value, thumbnail_path = NULL WHERE id = :id"),
                    sourceRoot, false, captureCount, error)
                    && rewriteColumn(db,
                    QStringLiteral("SELECT id, executable_path FROM games WHERE executable_path IS NOT NULL AND executable_path <> ''"),
                    QStringLiteral("UPDATE games SET executable_path = :value, icon_path = NULL WHERE id = :id"),
                    sourceRoot, true, gameCount, error)
                    && rewriteFolders(db, sourceRoot, folderCount, error)
                    && copyReferencedSounds(db, sourceRoot, stageRoot, destinationRoot,
                                            result.copiedSounds, error);
                if (ok) {
                    QSqlQuery clear(db);
                    ok = execSql(clear, QStringLiteral("UPDATE captures SET thumbnail_path = NULL"), error)
                        && execSql(clear, QStringLiteral("UPDATE games SET icon_path = NULL"), error)
                        && execSql(clear, QStringLiteral("DELETE FROM settings WHERE key = 'internal.icon_format'"), error);
                }
                if (ok) {
                    QSqlQuery settings(db);
                    ok = execSql(settings, QStringLiteral("SELECT key, value FROM settings"), error);
                    while (ok && settings.next()) {
                        if (QDir::fromNativeSeparators(settings.value(1).toString().trimmed())
                                .startsWith(QStringLiteral("portable:/"), Qt::CaseInsensitive)) {
                            error = NativeText::get(
                                        //: Portable-profile import database failure; %1 is an unchanged persisted key.
                                        //% "Unsupported portable path in database setting %1."
                                        QT_TRID_NOOP("gamehq.error.portable_import.database_path_unsupported"),
                                        "Unsupported portable path in database setting %1.")
                                .arg(settings.value(0).toString());
                            ok = false;
                        }
                    }
                }
                if (ok && !db.commit()) {
            error = databaseOperationError(db.lastError().text());
                    ok = false;
                }
                if (!ok)
                    db.rollback();
                if (ok) {
                    QSqlQuery integrity(db);
                    ok = integrity.exec(QStringLiteral("PRAGMA integrity_check"))
                        && integrity.next() && integrity.value(0).toString() == QStringLiteral("ok");
                    if (!ok)
                        error = NativeText::get(
                            //: Portable-profile import database integrity failure.
                            //% "The staged database failed its integrity check."
                            QT_TRID_NOOP("gamehq.error.portable_import.database_integrity_failed"),
                            "The staged database failed its integrity check.");
                }
                if (ok) {
                    QSqlQuery foreignKeys(db);
                    ok = foreignKeys.exec(QStringLiteral("PRAGMA foreign_key_check"))
                        && !foreignKeys.next();
                    if (!ok)
                        error = NativeText::get(
                            //: Portable-profile import database integrity failure.
                            //% "The staged database failed its foreign-key check."
                            QT_TRID_NOOP("gamehq.error.portable_import.database_foreign_key_failed"),
                            "The staged database failed its foreign-key check.");
                }
                if (ok) {
                    result.captures = captureCount;
                    result.games = gameCount;
                    result.watchedFolders = folderCount;
                    success = true;
                }
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connection);
    return success;
}

bool failAt(PortableProfileImporter::FailurePoint actual,
            PortableProfileImporter::FailurePoint requested, QString& error)
{
    if (actual != requested)
        return false;
    error = QStringLiteral("Injected portable-import failure.");
    return true;
}
} // namespace

bool PortableProfileImporter::importProfile(const Options& options, Result& result, QString& error)
{
    result = {};
    error.clear();
    const QString sourceRoot = normalizedAbsolute(options.sourcePackageRoot);
    const QString destinationRoot = normalizedAbsolute(options.destinationDataRoot);
    QLockFile transactionLock(transactionPathFor(destinationRoot) + QStringLiteral(".lock"));
    transactionLock.setStaleLockTime(0);
    if (!transactionLock.tryLock(0)) {
        error = NativeText::get(
            //: Portable-profile import concurrency failure.
            //% "Another portable-profile import is already running."
            QT_TRID_NOOP("gamehq.error.portable_import.transaction_running"),
            "Another portable-profile import is already running.");
        return false;
    }
    const QFileInfo sourceInfo(sourceRoot);
    if (!sourceInfo.isDir() || sourceInfo.isSymLink()
        || !QFileInfo(sourceRoot + QStringLiteral("/portable.flag")).isFile()
        || !QFileInfo(sourceRoot + QStringLiteral("/GameHQ.exe")).isFile()
        || !QFileInfo(sourceRoot + QStringLiteral("/gamehq-data")).isDir()) {
        error = NativeText::get(
            //: Portable-profile import source validation failure.
            //% "Select a valid GameHQ portable package root."
            QT_TRID_NOOP("gamehq.error.portable_import.source_package_invalid"),
            "Select a valid GameHQ portable package root.");
        return false;
    }
    const QString canonicalData = QFileInfo(sourceRoot + QStringLiteral("/gamehq-data")).canonicalFilePath();
    if (canonicalData.isEmpty() || !isWithin(sourceInfo.canonicalFilePath(), canonicalData)) {
        error = NativeText::get(
            //: Portable-profile import source path safety failure.
            //% "The portable data directory escapes the selected package root."
            QT_TRID_NOOP("gamehq.error.portable_import.data_directory_escapes_package"),
            "The portable data directory escapes the selected package root.");
        return false;
    }
    if (isWithin(sourceRoot, destinationRoot) || isWithin(destinationRoot, sourceRoot)) {
        error = NativeText::get(
            //: Portable-profile import source and destination validation failure.
            //% "Source and destination profiles must be separate."
            QT_TRID_NOOP("gamehq.error.portable_import.source_destination_overlap"),
            "Source and destination profiles must be separate.");
        return false;
    }
    if (!recoverInterruptedTransaction(destinationRoot, error)
        || !destinationIsEmpty(destinationRoot, error))
        return false;

    const QString sourceConfig = sourceRoot + QStringLiteral("/gamehq-data/config.json");
    const QString sourceDatabase = sourceRoot + QStringLiteral("/gamehq-data/gamehq.db");
    if (!QFileInfo(sourceDatabase).isFile()) {
        error = NativeText::get(
            //: Portable-profile import source validation failure; keep gamehq.db unchanged.
            //% "The portable profile has no gamehq.db."
            QT_TRID_NOOP("gamehq.error.portable_import.database_missing"),
            "The portable profile has no gamehq.db.");
        return false;
    }
    const QByteArray configHashBefore = QFileInfo::exists(sourceConfig)
        ? sha256File(sourceConfig, error) : QByteArray();
    if (!error.isEmpty())
        return false;
    const QByteArray databaseHashBefore = sha256File(sourceDatabase, error);
    if (!error.isEmpty())
        return false;

    const QString parent = QFileInfo(destinationRoot).absolutePath();
    const QString leaf = QFileInfo(destinationRoot).fileName();
    const QString suffix = QUuid::createUuid().toString(QUuid::Id128);
    const QString stageRoot = QDir(parent).filePath(QStringLiteral(".%1.import-stage-%2").arg(leaf, suffix));
    const QString backupRoot = QDir(parent).filePath(QStringLiteral(".%1.import-backup-%2").arg(leaf, suffix));
    const QString transactionPath = transactionPathFor(destinationRoot);
    const QString stageName = QFileInfo(stageRoot).fileName();
    const QString backupName = QFileInfo(backupRoot).fileName();
    if (!QDir().mkpath(stageRoot)) {
        error = NativeText::get(
            //: Portable-profile import filesystem failure.
            //% "The import staging directory cannot be created."
            QT_TRID_NOOP("gamehq.error.portable_import.staging_directory_failed"),
            "The import staging directory cannot be created.");
        return false;
    }
    if (!writeTransaction(transactionPath, QStringLiteral("staging"),
                          stageName, backupName, error)) {
        removeTree(stageRoot, error);
        return false;
    }

    bool destinationBackedUp = false;
    bool published = false;
    auto rollback = [&] {
        QString rollbackError;
        bool ok = true;
        if (published && QFileInfo::exists(destinationRoot))
            ok = removeTree(destinationRoot, rollbackError);
        if (ok && destinationBackedUp && QFileInfo::exists(backupRoot)
            && !QDir().rename(backupRoot, destinationRoot)) {
            rollbackError = NativeText::get(
                //: Portable-profile import rollback failure.
                //% "The installed profile backup could not be restored."
                QT_TRID_NOOP("gamehq.error.portable_import.rollback_restore_failed"),
                "The installed profile backup could not be restored.");
            ok = false;
        }
        if (ok)
            ok = removeTree(stageRoot, rollbackError);
        if (ok)
            QFile::remove(transactionPath);
        if (!ok)
            error = NativeText::get(
                        //: Portable-profile import compound failure; %1 is the original error and %2 is the unchanged rollback detail.
                        //% "%1 Recovery required: %2"
                        QT_TRID_NOOP("gamehq.error.portable_import.recovery_required"),
                        "%1 Recovery required: %2")
                        .arg(error, rollbackError);
    };

    QJsonObject config;
    if (!readConfig(sourceConfig, config, error)
        || !rewriteConfig(config, sourceRoot, error)
        || !writeJson(stageRoot + QStringLiteral("/config.json"), config, error)
        || !QFile::copy(sourceDatabase, stageRoot + QStringLiteral("/gamehq.db"))) {
        if (error.isEmpty())
            error = NativeText::get(
                //: Portable-profile import filesystem failure.
                //% "The portable database cannot be staged."
                QT_TRID_NOOP("gamehq.error.portable_import.database_stage_failed"),
                "The portable database cannot be staged.");
        rollback();
        return false;
    }
    if (failAt(FailurePoint::AfterStaging, options.failurePoint, error)) {
        rollback();
        return false;
    }
    if (!rewriteDatabase(stageRoot + QStringLiteral("/gamehq.db"), sourceRoot,
                         stageRoot, destinationRoot, result, error)
        || failAt(FailurePoint::AfterDatabaseRewrite, options.failurePoint, error)) {
        rollback();
        return false;
    }

    const QByteArray configHashAfter = QFileInfo::exists(sourceConfig)
        ? sha256File(sourceConfig, error) : QByteArray();
    const QByteArray databaseHashAfter = sha256File(sourceDatabase, error);
    if (!error.isEmpty() || configHashBefore != configHashAfter
        || databaseHashBefore != databaseHashAfter) {
        if (error.isEmpty())
            error = NativeText::get(
                //: Portable-profile import integrity failure.
                //% "The portable source changed during import."
                QT_TRID_NOOP("gamehq.error.portable_import.source_changed"),
                "The portable source changed during import.");
        rollback();
        return false;
    }

    QJsonObject evidence {
        { QStringLiteral("schemaVersion"), 1 },
        { QStringLiteral("completedAtUtc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate) },
        { QStringLiteral("sourceProfileUnchanged"), true },
        { QStringLiteral("captures"), result.captures },
        { QStringLiteral("games"), result.games },
        { QStringLiteral("watchedFolders"), result.watchedFolders },
        { QStringLiteral("copiedSounds"), result.copiedSounds }
    };
    if (!writeJson(stageRoot + QStringLiteral("/import-evidence.json"), evidence, error)
        || failAt(FailurePoint::BeforePublish, options.failurePoint, error)) {
        rollback();
        return false;
    }

    if (QFileInfo::exists(destinationRoot)) {
        if (!QDir().rename(destinationRoot, backupRoot)) {
            error = NativeText::get(
                //: Portable-profile import destination backup failure.
                //% "The empty installed profile cannot be backed up for import."
                QT_TRID_NOOP("gamehq.error.portable_import.destination_backup_failed"),
                "The empty installed profile cannot be backed up for import.");
            rollback();
            return false;
        }
        destinationBackedUp = true;
        if (!writeTransaction(transactionPath, QStringLiteral("destination-backed-up"),
                              stageName, backupName, error)) {
            rollback();
            return false;
        }
    }
    if (options.failurePoint == FailurePoint::InterruptAfterDestinationBackup) {
        error = QStringLiteral("Injected hard interruption after destination backup.");
        return false;
    }
    if (failAt(FailurePoint::AfterDestinationBackup, options.failurePoint, error)) {
        rollback();
        return false;
    }
    if (!QDir().rename(stageRoot, destinationRoot)) {
        error = NativeText::get(
            //: Portable-profile import publish failure.
            //% "The staged profile cannot be published."
            QT_TRID_NOOP("gamehq.error.portable_import.publish_failed"),
            "The staged profile cannot be published.");
        rollback();
        return false;
    }
    published = true;
    if (options.failurePoint == FailurePoint::InterruptAfterPublish) {
        error = QStringLiteral("Injected hard interruption after publish.");
        return false;
    }
    if (failAt(FailurePoint::AfterPublish, options.failurePoint, error)) {
        rollback();
        return false;
    }
    if (!writeTransaction(transactionPath, QStringLiteral("published"),
                          stageName, backupName, error)) {
        rollback();
        return false;
    }
    if (destinationBackedUp && !removeTree(backupRoot, error))
        return false;
    QFile::remove(transactionPath);
    result.evidencePath = destinationRoot + QStringLiteral("/import-evidence.json");
    return true;
}
