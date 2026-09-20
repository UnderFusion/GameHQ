#include "storage/CaptureDatabase.h"
#include "core/GameIdentity.h"
#include "storage/GameIconCache.h"
#include "storage/GameMetadataBackfill.h"
#include "storage/CaptureQueries.h"
#include "storage/GameRowRepair.h"
#include "config/Paths.h"
#include "input/BindingPattern.h"

#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QSet>
#include <QStringList>
#include <QUuid>
#include <QVariant>
#include <QDebug>
#include <algorithm>

CaptureDatabase::CaptureDatabase(QString filePath, QObject* parent)
    : QObject(parent)
    , m_filePath(std::move(filePath))
{
}

CaptureDatabase::~CaptureDatabase()
{
    if (m_db.isOpen())
        m_db.close();
}

bool CaptureDatabase::open()
{
    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("gamehq"));
    m_db.setDatabaseName(m_filePath);
    if (!m_db.open()) {
        qCritical() << "DB: open failed:" << m_db.lastError().text();
        return false;
    }
    QSqlQuery(QStringLiteral("PRAGMA foreign_keys = ON"), m_db);
    return migrate();
}

int CaptureDatabase::schemaVersion() const
{
    QSqlQuery q(QStringLiteral("PRAGMA user_version"), m_db);
    return q.next() ? q.value(0).toInt() : 0;
}

bool CaptureDatabase::migrate()
{
    const int version = schemaVersion();
    // An older build must not touch a database a newer one created: its writes
    // would be interpreted against a schema it cannot see.
    if (version > kCurrentSchemaVersion) {
        qWarning() << "DB: refusing to open schema version" << version
                   << "- this GameHQ understands at most" << kCurrentSchemaVersion;
        return false;
    }
    if (version < 1 && !applyV1())
        return false;
    if (version < 2 && !applyV2())
        return false;
    if (version < 3 && !applyV3())
        return false;
    if (version < 4 && !applyV4())
        return false;
    if (version < 5 && !applyV5())
        return false;
    if (version < 6 && !applyV6())
        return false;
    if (version < 7 && !applyV7())
        return false;
    if (version < 8 && !applyV8())
        return false;
    if (version < 9 && !applyV9())
        return false;
    if (!ensureGameMetadataColumns())
        return false;

    // Run the whole startup repair pass (brand paths, duplicate collapse, path
    // renormalization, game-row and metadata repair) inside one transaction so
    // a mid-run crash cannot leave the library half-repaired.
    // Without a transaction there is no way to undo a partial repair, so this
    // is fatal rather than a warning: the caller keeps the untouched database.
    if (!m_db.transaction()) {
        qWarning() << "DB: could not open repair transaction:" << m_db.lastError().text();
        return false;
    }
    // Every failure below must leave the database exactly as it was found.
    const auto abortRepair = [this](const QString& reason) {
        qWarning() << "DB: startup repair aborted -" << reason;
        m_db.rollback();
        return false;
    };

    // One-time brand-path compatibility for databases created by earlier brands.
    // The filesystem migration renames the managed roots; keep absolute paths
    // in existing gallery rows aligned with their new locations.
    const QStringList brandPathUpdates = {
        QStringLiteral("UPDATE captures SET thumbnail_path = replace(thumbnail_path, '/playhq-data/', '/gamehq-data/') WHERE thumbnail_path LIKE '%/playhq-data/%'"),
        QStringLiteral("UPDATE captures SET thumbnail_path = replace(thumbnail_path, '\\playhq-data\\', '\\gamehq-data\\') WHERE thumbnail_path LIKE '%\\playhq-data\\%'"),
        QStringLiteral("UPDATE games SET icon_path = replace(icon_path, '/playhq-data/', '/gamehq-data/') WHERE icon_path LIKE '%/playhq-data/%'"),
        QStringLiteral("UPDATE games SET icon_path = replace(icon_path, '\\playhq-data\\', '\\gamehq-data\\') WHERE icon_path LIKE '%\\playhq-data\\%'"),
        QStringLiteral("UPDATE captures SET thumbnail_path = replace(thumbnail_path, '/saveplay-data/', '/gamehq-data/') WHERE thumbnail_path LIKE '%/saveplay-data/%'"),
        QStringLiteral("UPDATE captures SET thumbnail_path = replace(thumbnail_path, '\\saveplay-data\\', '\\gamehq-data\\') WHERE thumbnail_path LIKE '%\\saveplay-data\\%'"),
        QStringLiteral("UPDATE games SET icon_path = replace(icon_path, '/saveplay-data/', '/gamehq-data/') WHERE icon_path LIKE '%/saveplay-data/%'"),
        QStringLiteral("UPDATE games SET icon_path = replace(icon_path, '\\saveplay-data\\', '\\gamehq-data\\') WHERE icon_path LIKE '%\\saveplay-data\\%'"),
        QStringLiteral("UPDATE captures SET source = 'GameHQ' WHERE source IN ('PlayHQ', 'SavePlay')")
    };
    for (const QString& statement : brandPathUpdates) {
        QSqlQuery q(m_db);
        if (!q.exec(statement))
            return abortRepair(QStringLiteral("brand-path migration: ") + q.lastError().text());
    }

    // Collapse rows that became the same physical capture after a portable
    // folder rename. Keep the best thumbnail and preserve favorite state.
    struct CapturePathRow { int id; QString path; QString thumbnail; bool favorite; };
    QHash<QString, QVector<CapturePathRow>> captureGroups;
    QSqlQuery captureSelect(QStringLiteral(
        "SELECT id, file_path, thumbnail_path, is_favorite FROM captures WHERE deleted_at IS NULL"), m_db);
    if (captureSelect.lastError().isValid())
        return abortRepair(QStringLiteral("could not list captures: ") + captureSelect.lastError().text());
    while (captureSelect.next()) {
        CapturePathRow row{captureSelect.value(0).toInt(), captureSelect.value(1).toString(),
                           captureSelect.value(2).toString(), captureSelect.value(3).toBool()};
        const QString repaired = Paths::toStoredPath(Paths::repairMovedPath(row.path));
        captureGroups[repaired.toCaseFolded()].append(row);
    }
    captureSelect.finish();
    for (auto it = captureGroups.cbegin(); it != captureGroups.cend(); ++it) {
        const QVector<CapturePathRow>& rows = it.value();
        int winnerIndex = 0;
        const QString canonicalPath = Paths::toStoredPath(Paths::repairMovedPath(rows.first().path));
        bool winnerIsCanonical = rows.first().path.compare(canonicalPath, Qt::CaseInsensitive) == 0;
        for (int i = 1; i < rows.size(); ++i) {
            const bool candidateIsCanonical = rows[i].path.compare(canonicalPath, Qt::CaseInsensitive) == 0;
            const bool candidateHasThumb = QFileInfo::exists(Paths::repairMovedPath(rows[i].thumbnail));
            const bool winnerHasThumb = QFileInfo::exists(Paths::repairMovedPath(rows[winnerIndex].thumbnail));
            if ((candidateIsCanonical && !winnerIsCanonical)
                || (candidateIsCanonical == winnerIsCanonical && candidateHasThumb && !winnerHasThumb)) {
                winnerIndex = i;
                winnerIsCanonical = candidateIsCanonical;
            }
        }
        bool favorite = false;
        QString thumbnail;
        for (int i = 0; i < rows.size(); ++i) {
            favorite = favorite || rows[i].favorite;
            if (thumbnail.isEmpty() && QFileInfo::exists(Paths::repairMovedPath(rows[i].thumbnail)))
                thumbnail = Paths::toStoredPath(Paths::repairMovedPath(rows[i].thumbnail));
            if (i == winnerIndex)
                continue;
            QSqlQuery tombstone(m_db);
            tombstone.prepare(QStringLiteral("UPDATE captures SET deleted_at = :now WHERE id = :id"));
            tombstone.bindValue(QStringLiteral(":now"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
            tombstone.bindValue(QStringLiteral(":id"), rows[i].id);
            if (!tombstone.exec())
                return abortRepair(QStringLiteral("could not tombstone a duplicate capture: ")
                                   + tombstone.lastError().text());
        }
        QSqlQuery update(m_db);
        update.prepare(QStringLiteral(
            "UPDATE captures SET file_path = :path, thumbnail_path = :thumb, is_favorite = :favorite WHERE id = :id"));
        update.bindValue(QStringLiteral(":path"), canonicalPath);
        update.bindValue(QStringLiteral(":thumb"), thumbnail.isEmpty() ? QVariant() : QVariant(thumbnail));
        update.bindValue(QStringLiteral(":favorite"), favorite ? 1 : 0);
        update.bindValue(QStringLiteral(":id"), rows[winnerIndex].id);
        if (!update.exec())
            return abortRepair(QStringLiteral("could not collapse duplicate captures: ")
                               + update.lastError().text());
    }

    // Normalize remaining package-owned paths after a move/rename.
    const struct { const char* table; const char* id; const char* column; } pathColumns[] = {
        { "captures", "id", "thumbnail_path" },
        { "games", "id", "icon_path" }, { "folders", "id", "path" }
    };
    for (const auto& spec : pathColumns) {
        QSqlQuery select(m_db);
        const QString sql = QStringLiteral("SELECT %1, %2 FROM %3 WHERE %2 IS NOT NULL AND %2 != ''")
                                .arg(QString::fromLatin1(spec.id), QString::fromLatin1(spec.column),
                                     QString::fromLatin1(spec.table));
        if (!select.exec(sql))
            return abortRepair(QStringLiteral("could not read stored paths: ") + select.lastError().text());
        while (select.next()) {
            const QString oldValue = select.value(1).toString();
            const QString newValue = Paths::toStoredPath(Paths::repairMovedPath(oldValue));
            if (newValue == oldValue)
                continue;
            QSqlQuery update(m_db);
            update.prepare(QStringLiteral("UPDATE %1 SET %2 = :path WHERE %3 = :id")
                               .arg(QString::fromLatin1(spec.table), QString::fromLatin1(spec.column),
                                    QString::fromLatin1(spec.id)));
            update.bindValue(QStringLiteral(":path"), newValue);
            update.bindValue(QStringLiteral(":id"), select.value(0));
            if (!update.exec())
                return abortRepair(QStringLiteral("could not normalize a stored path: ")
                                   + update.lastError().text());
        }
    }
    // The remaining repairs are heavy one-time passes: a full O(n²) duplicate
    // display-name scan and a full gamehq.log rescan. Gate both behind a
    // completion sentinel so they run once after an upgrade, then skip forever.
    // The sentinel is written inside this same repair transaction, so it only
    // sticks if the repairs themselves commit.
    if (!repairsV1Done()) {
        qInfo() << "DB: running one-time game-row/metadata repairs (repairs_v1)";
        // The sentinel is only written once every repair above it succeeded, so
        // a failed pass is retried on the next start instead of being skipped
        // forever.
        if (!GameRowRepair::normalizeDuplicateNames(m_db))
            return abortRepair(QStringLiteral("duplicate game-row repair failed"));
        if (!GameMetadataBackfill::run(m_db))
            return abortRepair(QStringLiteral("game metadata backfill failed"));
        if (!markRepairsV1Done())
            return abortRepair(QStringLiteral("could not record the repair sentinel"));
    } else {
        qInfo() << "DB: one-time game-row/metadata repairs already done, skipping (repairs_v1)";
    }
    if (!refreshIconsForExtractorFormat())
        return abortRepair(QStringLiteral("icon refresh failed"));
    if (!m_db.commit()) {
        qWarning() << "DB: repair transaction commit failed:" << m_db.lastError().text();
        m_db.rollback();
        return false;
    }
    qInfo() << "DB: schema version" << schemaVersion();
    return true;
}

QVector<CaptureRecord> CaptureDatabase::listCaptures(const QString& category, int gameId) const
{
    return CaptureQueries::listCaptures(m_db, category, gameId);
}

bool CaptureDatabase::hasCapture(const QString& filePath) const
{
    return CaptureQueries::hasCapture(m_db, filePath);
}

QHash<QString, CaptureIndexEntry> CaptureDatabase::captureIndex() const
{
    return CaptureQueries::captureIndex(m_db);
}

QString CaptureDatabase::storedPathKey(const QString& filePath)
{
    return Paths::toStoredPath(filePath);
}

bool CaptureDatabase::hasCapturesForGame(int gameId) const
{
    return CaptureQueries::hasCapturesForGame(m_db, gameId);
}

int CaptureDatabase::insertCapture(const QString& filePath, const QString& type,
                                   const QString& gameName, const QString& createdAt,
                                   const QString& source, const QString& executablePath)
{
    if (hasCapture(filePath))
        return -1;
    const int gameId = findOrCreateGame(gameName, executablePath);
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO captures (file_path, type, game_id, created_at, source) "
        "VALUES (:path, :type, :game, :created, :source)"));
    q.bindValue(QStringLiteral(":path"), Paths::toStoredPath(filePath));
    q.bindValue(QStringLiteral(":type"), type);
    q.bindValue(QStringLiteral(":game"), gameId >= 0 ? QVariant(gameId) : QVariant());
    q.bindValue(QStringLiteral(":created"), createdAt);
    q.bindValue(QStringLiteral(":source"), source);
    if (!q.exec()) {
        qWarning() << "DB: insertCapture failed:" << q.lastError().text();
        return -1;
    }
    return q.lastInsertId().toInt();
}

bool CaptureDatabase::setFavorite(int captureId, bool favorite)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE captures SET is_favorite = :f WHERE id = :id"));
    q.bindValue(QStringLiteral(":f"), favorite ? 1 : 0);
    q.bindValue(QStringLiteral(":id"), captureId);
    return q.exec();
}

bool CaptureDatabase::setThumbnail(int captureId, const QString& thumbnailPath)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE captures SET thumbnail_path = :t WHERE id = :id"));
    q.bindValue(QStringLiteral(":t"), Paths::toStoredPath(thumbnailPath));
    q.bindValue(QStringLiteral(":id"), captureId);
    return q.exec();
}

QString CaptureDatabase::thumbnailForCapture(const QString& filePath) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT thumbnail_path FROM captures WHERE file_path = :path AND deleted_at IS NULL LIMIT 1"));
    q.bindValue(QStringLiteral(":path"), Paths::toStoredPath(filePath));
    return q.exec() && q.next() ? Paths::repairMovedPath(q.value(0).toString()) : QString();
}

bool CaptureDatabase::setThumbnailForCapture(const QString& filePath, const QString& thumbnailPath)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "UPDATE captures SET thumbnail_path = :thumb WHERE file_path = :path AND deleted_at IS NULL"));
    q.bindValue(QStringLiteral(":thumb"), Paths::toStoredPath(thumbnailPath));
    q.bindValue(QStringLiteral(":path"), Paths::toStoredPath(filePath));
    return q.exec();
}

bool CaptureDatabase::deleteCapture(int captureId)
{
    // Soft-delete: the row is tombstoned (deleted_at) so it drops out of every
    // listing; the physical file is removed by the caller (AppController).
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE captures SET deleted_at = :t WHERE id = :id"));
    q.bindValue(QStringLiteral(":t"),
                QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    q.bindValue(QStringLiteral(":id"), captureId);
    if (!q.exec()) {
        qWarning() << "DB: deleteCapture failed" << q.lastError().text();
        return false;
    }
    return true;
}

QVector<GameEntry> CaptureDatabase::listGames() const
{
    return CaptureQueries::listGames(m_db);
}

bool CaptureDatabase::rememberGameExecutable(const QString& displayName,
                                             const QString& executablePath)
{
    if (displayName.isEmpty() || executablePath.isEmpty() || !QFileInfo::exists(executablePath))
        return false;

    const QString key = GameIdentity::key(displayName);
    int gameId = -1;
    QString oldExecutable;
    QString oldIcon;

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id, display_name, executable_path, icon_path FROM games"));
    if (q.exec()) {
        while (q.next()) {
            if (GameIdentity::key(q.value(1).toString()) != key)
                continue;
            gameId = q.value(0).toInt();
            oldExecutable = q.value(2).toString();
            oldIcon = q.value(3).toString();
            break;
        }
    }

    if (gameId < 0)
        gameId = findOrCreateGame(displayName, executablePath);
    else
        updateGameExecutable(gameId, executablePath);

    if (gameId < 0)
        return false;

    QSqlQuery after(m_db);
    after.prepare(QStringLiteral("SELECT executable_path, icon_path FROM games WHERE id = :id"));
    after.bindValue(QStringLiteral(":id"), gameId);
    if (!after.exec() || !after.next())
        return false;

    return oldExecutable != after.value(0).toString()
           || (oldIcon.isEmpty() && !after.value(1).toString().isEmpty());
}

bool CaptureDatabase::seedDefaultBindings()
{
    QSqlQuery count(QStringLiteral("SELECT COUNT(*) FROM bindings"), m_db);
    if (count.next() && count.value(0).toInt() > 0)
        return true;   // already seeded / user-edited — never overwrite

    struct Def { const char* device; const char* code; const char* action;
                 const char* press; int hold; };
    static const Def defaults[] = {
        { "controller", "Share", "screenshot",     "tap",  0 },
        { "controller", "Share", "save_replay",    "hold", 2000 },
        { "controller", "PS",    "overlay_toggle", "tap",  0 },
        { "controller", "Circle","overlay_hide",   "tap",  0 },
        { "keyboard",   "Ctrl+Shift+G", "overlay_toggle", "combo", 0 },
        { "keyboard",   "Ctrl+Shift+S", "screenshot",     "combo", 0 },
        { "keyboard",   "Ctrl+Shift+R", "save_replay",    "combo", 0 },
    };

    if (!m_db.transaction())
        return false;
    for (const Def& d : defaults) {
        QSqlQuery q(m_db);
        q.prepare(QStringLiteral(
            "INSERT INTO bindings (device_type, input_code, action, press_type, hold_ms) "
            "VALUES (:dev, :code, :act, :press, :hold)"));
        q.bindValue(QStringLiteral(":dev"), QString::fromLatin1(d.device));
        q.bindValue(QStringLiteral(":code"), QString::fromLatin1(d.code));
        q.bindValue(QStringLiteral(":act"), QString::fromLatin1(d.action));
        q.bindValue(QStringLiteral(":press"), QString::fromLatin1(d.press));
        q.bindValue(QStringLiteral(":hold"), d.hold > 0 ? QVariant(d.hold) : QVariant());
        if (!q.exec()) {
            qWarning() << "DB: seedDefaultBindings failed:" << q.lastError().text();
            m_db.rollback();
            return false;
        }
    }
    const bool ok = m_db.commit();
    if (ok)
        qInfo() << "DB: seeded default input bindings";
    return ok;
}

QVector<BindingRow> CaptureDatabase::listBindings() const
{
    QVector<BindingRow> out;
    QSqlQuery q(QStringLiteral(
        "SELECT device_type, input_code, action, press_type, hold_ms FROM bindings "
        "ORDER BY id"), m_db);
    while (q.next()) {
        BindingRow r;
        r.deviceType = q.value(0).toString();
        r.inputCode  = q.value(1).toString();
        r.action     = q.value(2).toString();
        r.pressType  = q.value(3).toString();
        r.holdMs     = q.value(4).isNull() ? 0 : q.value(4).toInt();
        out.append(r);
    }
    return out;
}

QVector<BindingOverrideRow> CaptureDatabase::listBindingOverrides() const
{
    QVector<BindingOverrideRow> out;
    loadLegacyOverridesChecked(&out, nullptr);
    return out;
}

bool CaptureDatabase::loadLegacyOverridesChecked(QVector<BindingOverrideRow>* sink,
                                                 QString* error) const
{
    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral(
            "SELECT device_group, device_profile, action_id, slot, trigger_code, "
            "activation, hold_ms, unbound, tap_count FROM binding_overrides ORDER BY id"))) {
        if (error)
            *error = q.lastError().text();
        return false;
    }
    while (q.next()) {
        BindingOverrideRow r;
        r.deviceGroup   = q.value(0).toString();
        r.deviceProfile = q.value(1).toString();
        r.actionId      = q.value(2).toString();
        r.slot          = q.value(3).toInt();
        r.triggerCode   = q.value(4).toString();
        r.activation    = q.value(5).toString();
        r.holdMs        = q.value(6).isNull() ? 0 : q.value(6).toInt();
        r.unbound       = q.value(7).toInt() != 0;
        r.tapCount      = q.value(8).isNull() ? 1 : q.value(8).toInt();
        sink->append(r);
    }
    return true;
}

bool CaptureDatabase::upsertBindingOverride(const BindingOverrideRow& row)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO binding_overrides "
        "(device_group, device_profile, action_id, slot, trigger_code, activation, hold_ms, unbound, tap_count) "
        "VALUES (:group, :profile, :action, :slot, :trigger, :activation, :hold, :unbound, :taps) "
        "ON CONFLICT(device_group, device_profile, action_id, slot) DO UPDATE SET "
        "trigger_code = excluded.trigger_code, activation = excluded.activation, "
        "hold_ms = excluded.hold_ms, unbound = excluded.unbound, tap_count = excluded.tap_count"));
    q.bindValue(QStringLiteral(":group"), row.deviceGroup);
    q.bindValue(QStringLiteral(":profile"), row.deviceProfile.isEmpty()
                                              ? QStringLiteral("")
                                              : row.deviceProfile);
    q.bindValue(QStringLiteral(":action"), row.actionId);
    q.bindValue(QStringLiteral(":slot"), row.slot);
    q.bindValue(QStringLiteral(":trigger"), row.triggerCode.isEmpty() ? QVariant() : row.triggerCode);
    q.bindValue(QStringLiteral(":activation"), row.activation);
    q.bindValue(QStringLiteral(":hold"), row.holdMs > 0 ? QVariant(row.holdMs) : QVariant());
    q.bindValue(QStringLiteral(":unbound"), row.unbound ? 1 : 0);
    q.bindValue(QStringLiteral(":taps"), row.tapCount > 0 ? row.tapCount : 1);
    if (!q.exec()) {
        qWarning() << "DB: upsertBindingOverride failed:" << q.lastError().text();
        return false;
    }
    return true;
}

bool CaptureDatabase::upsertBindingOverridesAtomically(const QVector<BindingOverrideRow>& rows)
{
    if (rows.isEmpty())
        return true;
    if (!m_db.transaction()) {
        qWarning() << "DB: could not open binding override transaction:"
                   << m_db.lastError().text();
        return false;
    }
    for (const BindingOverrideRow& row : rows) {
        if (upsertBindingOverride(row))
            continue;
        m_db.rollback();
        return false;
    }
    if (!m_db.commit()) {
        qWarning() << "DB: binding override transaction commit failed:"
                   << m_db.lastError().text();
        m_db.rollback();
        return false;
    }
    return true;
}

bool CaptureDatabase::clearBindingOverride(const QString& deviceGroup, const QString& deviceProfile,
                                            const QString& actionId, int slot)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "DELETE FROM binding_overrides WHERE device_group = :group AND device_profile = :profile "
        "AND action_id = :action AND slot = :slot"));
    q.bindValue(QStringLiteral(":group"), deviceGroup);
    q.bindValue(QStringLiteral(":profile"), deviceProfile.isEmpty()
                                              ? QStringLiteral("")
                                              : deviceProfile);
    q.bindValue(QStringLiteral(":action"), actionId);
    q.bindValue(QStringLiteral(":slot"), slot);
    return q.exec();
}

bool CaptureDatabase::clearBindingOverridesForGroup(const QString& deviceGroup)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM binding_overrides WHERE device_group = :group"));
    q.bindValue(QStringLiteral(":group"), deviceGroup);
    return q.exec();
}

bool CaptureDatabase::clearBindingOverridesForProfile(const QString& deviceGroup,
                                                       const QString& deviceProfile)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "DELETE FROM binding_overrides WHERE device_group = :group AND device_profile = :profile"));
    q.bindValue(QStringLiteral(":group"), deviceGroup);
    q.bindValue(QStringLiteral(":profile"), deviceProfile.isEmpty()
                                              ? QStringLiteral("")
                                              : deviceProfile);
    return q.exec();
}

bool CaptureDatabase::clearAllBindingOverrides()
{
    QSqlQuery q(m_db);
    return q.exec(QStringLiteral("DELETE FROM binding_overrides"));
}

ControllerLayoutRow CaptureDatabase::controllerLayout(const QString& logicalId) const
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT logical_id, layout_signature, button_labels_json, needs_reconfirmation "
        "FROM controller_layouts WHERE logical_id = :id"));
    query.bindValue(QStringLiteral(":id"), logicalId);
    if (!query.exec() || !query.next())
        return {};
    ControllerLayoutRow row;
    row.logicalId = query.value(0).toString();
    row.layoutSignature = query.value(1).toString();
    const auto array = QJsonDocument::fromJson(query.value(2).toByteArray()).array();
    for (const auto& value : array)
        row.buttonLabels.push_back(value.toString());
    row.needsReconfirmation = query.value(3).toBool();
    return row;
}

bool CaptureDatabase::upsertControllerLayout(const ControllerLayoutRow& row)
{
    QJsonArray labels;
    for (const QString& label : row.buttonLabels)
        labels.push_back(label);
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO controller_layouts "
        "(logical_id, layout_signature, button_labels_json, needs_reconfirmation) "
        "VALUES (:id, :signature, :labels, :reconfirm) "
        "ON CONFLICT(logical_id) DO UPDATE SET layout_signature=excluded.layout_signature, "
        "button_labels_json=excluded.button_labels_json, "
        "needs_reconfirmation=excluded.needs_reconfirmation"));
    query.bindValue(QStringLiteral(":id"), row.logicalId);
    query.bindValue(QStringLiteral(":signature"), row.layoutSignature);
    query.bindValue(QStringLiteral(":labels"),
                    QString::fromUtf8(QJsonDocument(labels).toJson(QJsonDocument::Compact)));
    query.bindValue(QStringLiteral(":reconfirm"), row.needsReconfirmation ? 1 : 0);
    return query.exec();
}

bool CaptureDatabase::confirmControllerLayout(const QString& logicalId)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "UPDATE controller_layouts SET needs_reconfirmation = 0 WHERE logical_id = :id"));
    query.bindValue(QStringLiteral(":id"), logicalId);
    return query.exec();
}

QStringList CaptureDatabase::watchedFolders() const
{
    QStringList out;
    QSqlQuery q(QStringLiteral("SELECT path FROM folders WHERE is_watched = 1"), m_db);
    while (q.next())
        out.append(Paths::repairMovedPath(q.value(0).toString()));
    return out;
}

bool CaptureDatabase::addWatchedFolder(const QString& path, const QString& source)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO folders (path, source, is_watched) VALUES (:p, :s, 1)"));
    q.bindValue(QStringLiteral(":p"), Paths::toStoredPath(path));
    q.bindValue(QStringLiteral(":s"), source);
    return q.exec();
}

bool CaptureDatabase::removeWatchedFolder(const QString& path)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM folders WHERE path = :p"));
    q.bindValue(QStringLiteral(":p"), Paths::toStoredPath(path));
    return q.exec();
}

bool CaptureDatabase::ensureGameMetadataColumns()
{
    QStringList columns;
    QSqlQuery info(QStringLiteral("PRAGMA table_info(games)"), m_db);
    while (info.next())
        columns.append(info.value(1).toString());

    struct Column { const char* name; const char* sql; };
    const Column wanted[] = {
        { "executable_path", "ALTER TABLE games ADD COLUMN executable_path TEXT" },
        { "icon_path",       "ALTER TABLE games ADD COLUMN icon_path TEXT" },
        { "last_seen_at",    "ALTER TABLE games ADD COLUMN last_seen_at TEXT" },
    };
    for (const Column& c : wanted) {
        if (columns.contains(QString::fromLatin1(c.name)))
            continue;
        QSqlQuery alter(m_db);
        if (!alter.exec(QString::fromLatin1(c.sql))) {
            qCritical() << "DB: could not add games." << c.name << alter.lastError().text();
            return false;
        }
        qInfo() << "DB: added games." << c.name;
    }
    return true;
}

bool CaptureDatabase::repairsV1Done() const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT value FROM settings WHERE key = :k"));
    q.bindValue(QStringLiteral(":k"), QStringLiteral("internal.repairs_v1_done"));
    return q.exec() && q.next() && q.value(0).toString() == QLatin1String("1");
}

bool CaptureDatabase::markRepairsV1Done()
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO settings (key, value) VALUES (:k, '1') "
        "ON CONFLICT(key) DO UPDATE SET value = '1'"));
    q.bindValue(QStringLiteral(":k"), QStringLiteral("internal.repairs_v1_done"));
    if (!q.exec()) {
        qWarning() << "DB: could not record repairs_v1 sentinel:" << q.lastError().text();
        return false;
    }
    return true;
}

// An icon is extracted once and then pinned in games.icon_path, and the only
// thing that ever rewrites it is detecting that game running again. So teaching
// the extractor a new trick does nothing for a library that is already filled
// in: every row keeps serving the icon the old extractor produced, possibly
// forever, because the user has no reason to relaunch the game.
//
// Re-extract for every game with a known executable whenever the extractor's
// format version moves. That version is bumped precisely when the output can
// change, so this costs one pass per upgrade and nothing on every other start.
bool CaptureDatabase::refreshIconsForExtractorFormat()
{
    const QString format = GameIconCache::formatVersion();

    QSqlQuery stored(m_db);
    stored.prepare(QStringLiteral("SELECT value FROM settings WHERE key = :k"));
    stored.bindValue(QStringLiteral(":k"), QStringLiteral("internal.icon_format"));
    if (stored.exec() && stored.next() && stored.value(0).toString() == format)
        return true;

    QSqlQuery select(m_db);
    if (!select.exec(QStringLiteral(
            "SELECT id, executable_path FROM games "
            "WHERE executable_path IS NOT NULL AND executable_path <> ''"))) {
        qWarning() << "DB: icon refresh could not list games:" << select.lastError().text();
        return false;
    }

    struct Candidate { int id; QString executablePath; };
    QVector<Candidate> candidates;
    while (select.next())
        candidates.append({ select.value(0).toInt(), select.value(1).toString() });

    int refreshed = 0;
    for (const Candidate& candidate : candidates) {
        const QString iconPath = GameIconCache::iconPathForExecutable(candidate.executablePath);
        if (iconPath.isEmpty())
            continue;   // extractor already logged why
        QSqlQuery update(m_db);
        update.prepare(QStringLiteral("UPDATE games SET icon_path = :icon WHERE id = :id"));
        update.bindValue(QStringLiteral(":icon"), iconPath);
        update.bindValue(QStringLiteral(":id"), candidate.id);
        if (update.exec())
            ++refreshed;
        else
            qWarning() << "DB: icon refresh update failed:" << update.lastError().text();
    }

    QSqlQuery mark(m_db);
    mark.prepare(QStringLiteral(
        "INSERT INTO settings (key, value) VALUES (:k, :v) "
        "ON CONFLICT(key) DO UPDATE SET value = :v"));
    mark.bindValue(QStringLiteral(":k"), QStringLiteral("internal.icon_format"));
    mark.bindValue(QStringLiteral(":v"), format);
    if (!mark.exec()) {
        // Without the sentinel this pass would repeat on every start, so treat
        // it as a repair failure rather than silently churning.
        qWarning() << "DB: could not record icon format sentinel:" << mark.lastError().text();
        return false;
    }

    qInfo() << "DB: re-extracted icons for" << refreshed << "of" << candidates.size()
            << "games (extractor format" << format << ")";
    return true;
}

int CaptureDatabase::findOrCreateGame(const QString& displayName, const QString& executablePath)
{
    const QString name = displayName.isEmpty() ? QStringLiteral("Unknown Game") : displayName;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id FROM games WHERE display_name = :n"));
    q.bindValue(QStringLiteral(":n"), name);
    if (q.exec() && q.next()) {
        const int id = q.value(0).toInt();
        updateGameExecutable(id, executablePath);
        return id;
    }

    q.prepare(QStringLiteral("SELECT id, display_name FROM games"));
    if (q.exec()) {
        const QString key = GameIdentity::key(name);
        while (q.next()) {
            const int id = q.value(0).toInt();
            const QString existing = q.value(1).toString();
            if (GameIdentity::key(existing) != key)
                continue;

            if (GameRowRepair::isBetterDisplayName(name, existing)) {
                QSqlQuery updateName(m_db);
                updateName.prepare(QStringLiteral(
                    "UPDATE games SET display_name = :name WHERE id = :id"));
                updateName.bindValue(QStringLiteral(":name"), name);
                updateName.bindValue(QStringLiteral(":id"), id);
                if (!updateName.exec())
                    qWarning() << "DB: could not update game display name:"
                               << updateName.lastError().text();
            }
            updateGameExecutable(id, executablePath);
            return id;
        }
    }

    q.prepare(QStringLiteral(
        "INSERT INTO games (display_name, executable_path, icon_path, created_at) "
        "VALUES (:n, :exe, :icon, :c)"));
    const QString iconPath = GameIconCache::iconPathForExecutable(executablePath);
    q.bindValue(QStringLiteral(":n"), name);
    q.bindValue(QStringLiteral(":exe"), executablePath.isEmpty() ? QVariant() : QVariant(executablePath));
    q.bindValue(QStringLiteral(":icon"), iconPath.isEmpty() ? QVariant() : QVariant(iconPath));
    q.bindValue(QStringLiteral(":c"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    if (!q.exec()) {
        qWarning() << "DB: findOrCreateGame failed:" << q.lastError().text();
        return -1;
    }
    return q.lastInsertId().toInt();
}

void CaptureDatabase::updateGameExecutable(int gameId, const QString& executablePath)
{
    if (gameId < 0 || executablePath.isEmpty() || !QFileInfo::exists(executablePath))
        return;

    const QString iconPath = GameIconCache::iconPathForExecutable(executablePath);
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "UPDATE games SET executable_path = :exe, icon_path = COALESCE(:icon, icon_path), "
        "last_seen_at = :seen WHERE id = :id"));
    q.bindValue(QStringLiteral(":exe"), executablePath);
    q.bindValue(QStringLiteral(":icon"), iconPath.isEmpty() ? QVariant() : QVariant(iconPath));
    q.bindValue(QStringLiteral(":seen"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    q.bindValue(QStringLiteral(":id"), gameId);
    if (!q.exec())
        qWarning() << "DB: could not update game executable/icon:" << q.lastError().text();
}

bool CaptureDatabase::applyV1()
{
    const QStringList statements = {
        QStringLiteral(R"(CREATE TABLE IF NOT EXISTS games (
            id              INTEGER PRIMARY KEY AUTOINCREMENT,
            display_name    TEXT NOT NULL,
            process_name    TEXT UNIQUE,
            executable_path TEXT,
            icon_path       TEXT,
            created_at      TEXT NOT NULL,
            last_seen_at    TEXT,
            is_whitelisted  INTEGER NOT NULL DEFAULT 0))"),
        QStringLiteral(R"(CREATE TABLE IF NOT EXISTS captures (
            id             INTEGER PRIMARY KEY AUTOINCREMENT,
            file_path      TEXT NOT NULL UNIQUE,
            type           TEXT NOT NULL CHECK(type IN ('screenshot','video')),
            game_id        INTEGER REFERENCES games(id),
            process_name   TEXT,
            window_title   TEXT,
            created_at     TEXT NOT NULL,
            duration_ms    INTEGER,
            width          INTEGER,
            height         INTEGER,
            fps            INTEGER,
            codec          TEXT,
            bitrate        INTEGER,
            is_favorite    INTEGER NOT NULL DEFAULT 0,
            source         TEXT NOT NULL DEFAULT 'GameHQ',
            thumbnail_path TEXT,
            deleted_at     TEXT))"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_captures_game ON captures(game_id)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_captures_created ON captures(created_at)"),
        QStringLiteral(R"(CREATE TABLE IF NOT EXISTS settings (
            key   TEXT PRIMARY KEY,
            value TEXT))"),
        QStringLiteral(R"(CREATE TABLE IF NOT EXISTS bindings (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            device_type TEXT NOT NULL CHECK(device_type IN ('keyboard','controller')),
            input_code  TEXT NOT NULL,
            action      TEXT NOT NULL,
            press_type  TEXT NOT NULL DEFAULT 'tap' CHECK(press_type IN ('tap','hold','combo')),
            hold_ms     INTEGER))"),
        QStringLiteral(R"(CREATE TABLE IF NOT EXISTS folders (
            id         INTEGER PRIMARY KEY AUTOINCREMENT,
            path       TEXT NOT NULL UNIQUE,
            source     TEXT NOT NULL DEFAULT 'Custom',
            is_watched INTEGER NOT NULL DEFAULT 1))"),
        QStringLiteral(R"(CREATE TABLE IF NOT EXISTS sound_settings (
            id         INTEGER PRIMARY KEY AUTOINCREMENT,
            event_name TEXT NOT NULL UNIQUE,
            enabled    INTEGER NOT NULL DEFAULT 1,
            volume     INTEGER NOT NULL DEFAULT 80,
            sound_file TEXT))"),
    };

    if (!m_db.transaction()) {
        qCritical() << "DB: cannot start migration transaction";
        return false;
    }
    for (const QString& sql : statements) {
        QSqlQuery q(m_db);
        if (!q.exec(sql)) {
            qCritical() << "DB: migration v1 failed:" << q.lastError().text();
            m_db.rollback();
            return false;
        }
    }
    QSqlQuery(QStringLiteral("PRAGMA user_version = 1"), m_db);
    return m_db.commit();
}

bool CaptureDatabase::applyV2()
{
    // Additive only — v1's tables (including the legacy `bindings` seed data)
    // are untouched. Built-in trigger defaults live in code, so this table
    // starts empty on every upgrade; there is nothing to migrate out of v1.
    const QStringList statements = {
        QStringLiteral(R"(CREATE TABLE IF NOT EXISTS binding_overrides (
            id             INTEGER PRIMARY KEY AUTOINCREMENT,
            device_group   TEXT NOT NULL CHECK(device_group IN ('keyboard','controller')),
            device_profile TEXT NOT NULL DEFAULT '',
            action_id      TEXT NOT NULL,
            slot           INTEGER NOT NULL DEFAULT 1 CHECK(slot IN (1,2)),
            trigger_code   TEXT,
            activation     TEXT NOT NULL DEFAULT 'press' CHECK(activation IN ('press','tap','hold','double_tap')),
            hold_ms        INTEGER,
            unbound        INTEGER NOT NULL DEFAULT 0 CHECK(unbound IN (0,1))))"),
        QStringLiteral(
            "CREATE UNIQUE INDEX IF NOT EXISTS idx_binding_overrides_scope "
            "ON binding_overrides(device_group, device_profile, action_id, slot)"),
    };

    if (!m_db.transaction()) {
        qCritical() << "DB: cannot start migration transaction";
        return false;
    }
    for (const QString& sql : statements) {
        QSqlQuery q(m_db);
        if (!q.exec(sql)) {
            qCritical() << "DB: migration v2 failed:" << q.lastError().text();
            m_db.rollback();
            return false;
        }
    }
    QSqlQuery(QStringLiteral("PRAGMA user_version = 2"), m_db);
    return m_db.commit();
}

bool CaptureDatabase::applyV3()
{
    // SQLite cannot widen a CHECK constraint in place. Rebuild the sparse
    // override table so extra mouse buttons can use the same canonical model.
    const QStringList statements = {
        QStringLiteral("DROP INDEX IF EXISTS idx_binding_overrides_scope"),
        QStringLiteral(R"(CREATE TABLE binding_overrides_v3 (
            id             INTEGER PRIMARY KEY AUTOINCREMENT,
            device_group   TEXT NOT NULL CHECK(device_group IN ('keyboard','controller','mouse')),
            device_profile TEXT NOT NULL DEFAULT '',
            action_id      TEXT NOT NULL,
            slot           INTEGER NOT NULL DEFAULT 1 CHECK(slot IN (1,2)),
            trigger_code   TEXT,
            activation     TEXT NOT NULL DEFAULT 'press' CHECK(activation IN ('press','tap','hold','double_tap')),
            hold_ms        INTEGER,
            unbound        INTEGER NOT NULL DEFAULT 0 CHECK(unbound IN (0,1))))"),
        QStringLiteral(R"(INSERT INTO binding_overrides_v3
            (id, device_group, device_profile, action_id, slot, trigger_code, activation, hold_ms, unbound)
            SELECT id, device_group, device_profile, action_id, slot, trigger_code, activation, hold_ms, unbound
            FROM binding_overrides)"),
        QStringLiteral("DROP TABLE binding_overrides"),
        QStringLiteral("ALTER TABLE binding_overrides_v3 RENAME TO binding_overrides"),
        QStringLiteral("CREATE UNIQUE INDEX idx_binding_overrides_scope ON binding_overrides(device_group, device_profile, action_id, slot)"),
    };

    if (!m_db.transaction())
        return false;
    for (const QString& sql : statements) {
        QSqlQuery q(m_db);
        if (!q.exec(sql)) {
            qCritical() << "DB: migration v3 failed:" << q.lastError().text();
            m_db.rollback();
            return false;
        }
    }
    QSqlQuery(QStringLiteral("PRAGMA user_version = 3"), m_db);
    return m_db.commit();
}

bool CaptureDatabase::applyV4()
{
    // Tap counts become a column of their own so a triple tap can be stored at
    // all. A plain ADD COLUMN is enough here — unlike v3 this changes no CHECK
    // constraint and no index, and the NOT NULL DEFAULT 1 fills every existing
    // row with "one tap", which is what all of them meant.
    //
    // The second statement retires the `double_tap` spelling: the gesture is
    // now "tap, twice", so the count lives in one place instead of being half
    // in a string and half in a column. The activation CHECK still accepts the
    // old token (rewriting it would mean another table rebuild for no gain), so
    // a database written by an older build keeps opening here; nothing writes
    // it any more.
    const QStringList statements = {
        QStringLiteral("ALTER TABLE binding_overrides "
                       "ADD COLUMN tap_count INTEGER NOT NULL DEFAULT 1"),
        QStringLiteral("UPDATE binding_overrides SET activation = 'tap', tap_count = 2 "
                       "WHERE activation = 'double_tap'"),
    };

    if (!m_db.transaction())
        return false;
    for (const QString& sql : statements) {
        QSqlQuery q(m_db);
        if (!q.exec(sql)) {
            qCritical() << "DB: migration v4 failed:" << q.lastError().text();
            m_db.rollback();
            return false;
        }
    }
    QSqlQuery(QStringLiteral("PRAGMA user_version = 4"), m_db);
    return m_db.commit();
}

bool CaptureDatabase::applyV5()
{
    // XInput's BACK bit was historically mislabeled as the true Capture
    // control. Only legacy slot profiles prove that the row came from XInput,
    // so migrate those and nothing else. Shared profiles and model identities
    // are deliberately untouched: rewriting either from the currently
    // connected controller would make their meaning device-dependent.
    if (!m_db.transaction())
        return false;
    QSqlQuery migrate(m_db);
    migrate.prepare(QStringLiteral(
        "UPDATE binding_overrides "
        "SET trigger_code = REPLACE(trigger_code, 'gamepad.capture', 'gamepad.view_back') "
        "WHERE device_group = 'controller' AND device_profile LIKE 'xinput.slot%' "
        "AND trigger_code LIKE '%gamepad.capture%'"));
    if (!migrate.exec()) {
        qCritical() << "DB: migration v5 failed:" << migrate.lastError().text();
        m_db.rollback();
        return false;
    }
    QSqlQuery(QStringLiteral("PRAGMA user_version = 5"), m_db);
    return m_db.commit();
}

bool CaptureDatabase::applyV6()
{
    if (!m_db.transaction())
        return false;
    QSqlQuery create(m_db);
    if (!create.exec(QStringLiteral(R"(CREATE TABLE IF NOT EXISTS controller_layouts (
            logical_id             TEXT PRIMARY KEY,
            layout_signature       TEXT NOT NULL,
            button_labels_json     TEXT NOT NULL DEFAULT '[]',
            needs_reconfirmation   INTEGER NOT NULL DEFAULT 0 CHECK(needs_reconfirmation IN (0,1))))"))) {
        qCritical() << "DB: migration v6 failed:" << create.lastError().text();
        m_db.rollback();
        return false;
    }
    QSqlQuery(QStringLiteral("PRAGMA user_version = 6"), m_db);
    return m_db.commit();
}

bool CaptureDatabase::applyV7()
{
    // Guide grew a default tap/hold pair (tap = Toggle Overlay, 2 s hold =
    // Show / Hide GameHQ). A persisted Guide *press* override from before that
    // fires on the down edge, which opens the overlay and cancels the pattern
    // recognizer — the new hold can then never arm. Rewriting the overlay
    // toggle to a single tap keeps its meaning (it still toggles the overlay,
    // just on release) while letting the hold coexist.
    if (!m_db.transaction())
        return false;
    QSqlQuery overlayPress(m_db);
    overlayPress.prepare(QStringLiteral(
        "UPDATE binding_overrides "
        "SET activation = 'tap', tap_count = 1, hold_ms = 0 "
        "WHERE device_group = 'controller' "
        "AND action_id = 'global.toggle_overlay' "
        "AND trigger_code = 'gamepad.guide' "
        "AND activation = 'press' "
        "AND unbound = 0"));
    if (!overlayPress.exec()) {
        qCritical() << "DB: migration v7 failed:" << overlayPress.lastError().text();
        m_db.rollback();
        return false;
    }

    // A Guide press the user bound to some *other* action is kept exactly as
    // saved — but then the new default Guide hold would fire on top of it.
    // Suppress the hold for those profiles with an explicit unbound row (the
    // user can rebind it in the editor); never silently run both actions.
    QSqlQuery suppressHold(m_db);
    suppressHold.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO binding_overrides "
        "(device_group, device_profile, action_id, slot, trigger_code, "
        " activation, hold_ms, unbound, tap_count) "
        "SELECT 'controller', device_profile, 'global.toggle_desktop', 1, NULL, "
        "       'hold', 2000, 1, 1 "
        "FROM binding_overrides "
        "WHERE device_group = 'controller' "
        "AND action_id <> 'global.toggle_overlay' "
        "AND trigger_code = 'gamepad.guide' "
        "AND activation = 'press' "
        "AND unbound = 0 "
        "GROUP BY device_profile"));
    if (!suppressHold.exec()) {
        qCritical() << "DB: migration v7 failed:" << suppressHold.lastError().text();
        m_db.rollback();
        return false;
    }
    if (suppressHold.numRowsAffected() > 0) {
        qInfo() << "DB: migration v7 kept a custom Guide press binding and"
                << "disabled the default Guide-hold Show/Hide GameHQ action for"
                << suppressHold.numRowsAffected() << "profile(s); it can be"
                << "re-assigned in Settings -> Input.";
    }
    QSqlQuery(QStringLiteral("PRAGMA user_version = 7"), m_db);
    return m_db.commit();
}

// --- Mapping presets (schema v8, docs/mapping-presets.md) ---------------------
//
// Storage contract:
// - A preset's identity is its opaque id; its device group is immutable and an
//   assignment can never cross groups (the composite foreign key enforces it).
// - A stored key never types itself. Every assignment carries its kind, so a
//   durable `controller` target and a `legacy_slot` fingerprint stay different
//   records even when their text looks identical.
// - A game assignment matches on the executable key only; `game_row_id` is a UI
//   join cache with no foreign key, because `GameRowRepair` may merge or delete
//   game rows and a stale id must never attract an assignment.
// - Historical `controller-…` profile keys live in mapping_preset_sources,
//   which is deliberately not an assignment table: strong and weak ids have the
//   same stored shape, so promotion to a durable controller assignment is a
//   runtime decision (cpo-p03) written through promoteMappingPresetSource, which
//   lands the assignment and the `promoted` flag in one transaction.
// - This migration creates the empty, versioned store. Converting existing
//   binding_overrides rows is cpo-p03's job, not this build's.

namespace
{
QString mappingNow()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
}

bool knownSourceStatus(const QString& status)
{
    return status == QLatin1String("unverified") || status == QLatin1String("promoted")
        || status == QLatin1String("retired");
}

MappingPreset mappingPresetFromQuery(const QSqlQuery& q)
{
    MappingPreset preset;
    preset.id          = q.value(0).toString();
    preset.deviceGroup = q.value(1).toString();
    preset.name        = q.value(2).toString();
    preset.origin      = q.value(3).toString();
    preset.createdAt   = q.value(4).toString();
    preset.updatedAt   = q.value(5).toString();
    return preset;
}

MappingPresetSource mappingSourceFromQuery(const QSqlQuery& q)
{
    MappingPresetSource source;
    source.deviceGroup       = q.value(0).toString();
    source.sourceKey         = q.value(1).toString();
    source.convertedPresetId = q.value(2).toString();
    source.status            = q.value(3).toString();
    source.createdAt         = q.value(4).toString();
    source.promotedAt        = q.value(5).toString();
    source.note              = q.value(6).toString();
    return source;
}

// Schema v9 conversion: the last winning row per (action, slot) across the
// chain layers, in the preset-row shape. Unbound and empty-trigger sentinels
// are kept as explicit unbound rows so the content suppresses exactly the
// defaults the legacy merge suppressed. Rows that cannot be stored as preset
// content are skipped here; they stay in binding_overrides and keep resolving
// exactly as before (which is not at all), so nothing user-visible changes.
QVector<MappingPresetRow> migrationFoldRows(const QVector<BindingOverrideRow>& validated,
                                            const QString& group, const QString& exactProfile)
{
    QStringList layers{QString()};
    if (!exactProfile.isEmpty())
        layers.append(exactProfile);
    QHash<QString, int> slotOf;
    QVector<MappingPresetRow> fold;
    for (const QString& layer : layers) {
        for (const BindingOverrideRow& source : validated) {
            if (source.deviceGroup != group || source.deviceProfile != layer)
                continue;
            MappingPresetRow row;
            row.actionId = source.actionId;
            row.slot = source.slot;
            row.unbound = source.unbound || source.triggerCode.isEmpty();
            row.triggerCode = row.unbound ? QString() : source.triggerCode;
            row.activation = source.activation;
            row.holdMs = source.holdMs;
            row.tapCount = source.tapCount;
            if (!CaptureDatabase::isValidMappingPresetRow(group, row))
                continue;
            const QString key = row.actionId + QLatin1Char('\n') + QString::number(row.slot);
            const auto hit = slotOf.constFind(key);
            if (hit != slotOf.cend()) {
                fold[*hit] = row;
            } else {
                slotOf.insert(key, fold.size());
                fold.append(row);
            }
        }
    }
    std::sort(fold.begin(), fold.end(), [](const MappingPresetRow& a, const MappingPresetRow& b) {
        if (a.actionId != b.actionId)
            return a.actionId < b.actionId;
        return a.slot < b.slot;
    });
    return fold;
}
} // namespace

bool CaptureDatabase::applyV8()
{
    // Additive only. `binding_overrides` and the legacy `bindings` seed data are
    // untouched, and no user row is converted into a preset here — that
    // conversion (and its bounded compatibility phase) belongs to cpo-p03.
    const QStringList statements = {
        QStringLiteral(R"(CREATE TABLE IF NOT EXISTS mapping_presets (
            id            TEXT PRIMARY KEY,
            device_group  TEXT NOT NULL CHECK(device_group IN ('keyboard','controller','mouse')),
            name          TEXT NOT NULL,
            name_key      TEXT NOT NULL,
            origin        TEXT NOT NULL DEFAULT 'user' CHECK(origin IN ('user','migration')),
            created_at    TEXT NOT NULL,
            updated_at    TEXT NOT NULL,
            UNIQUE(device_group, name_key)))"),
        // Parent key for the composite foreign keys below. This index is what
        // makes a cross-group assignment impossible in the database itself
        // instead of relying on every caller to remember the rule.
        QStringLiteral("CREATE UNIQUE INDEX IF NOT EXISTS idx_mapping_presets_identity "
                       "ON mapping_presets(id, device_group)"),
        QStringLiteral(R"(CREATE TABLE IF NOT EXISTS mapping_preset_rows (
            preset_id     TEXT NOT NULL REFERENCES mapping_presets(id) ON DELETE CASCADE,
            action_id     TEXT NOT NULL,
            slot          INTEGER NOT NULL DEFAULT 1 CHECK(slot IN (1,2)),
            trigger_code  TEXT,
            activation    TEXT NOT NULL DEFAULT 'press' CHECK(activation IN ('press','tap','hold','double_tap')),
            hold_ms       INTEGER,
            unbound       INTEGER NOT NULL DEFAULT 0 CHECK(unbound IN (0,1)),
            tap_count     INTEGER NOT NULL DEFAULT 1 CHECK(tap_count BETWEEN 1 AND 3),
            PRIMARY KEY (preset_id, action_id, slot)))"),
        QStringLiteral(R"(CREATE TABLE IF NOT EXISTS mapping_assignments (
            id            INTEGER PRIMARY KEY AUTOINCREMENT,
            device_group  TEXT NOT NULL CHECK(device_group IN ('keyboard','controller','mouse')),
            target_kind   TEXT NOT NULL CHECK(target_kind IN ('controller','legacy_slot','game','group_default')),
            target_key    TEXT NOT NULL DEFAULT '',
            preset_id     TEXT NOT NULL,
            game_row_id   INTEGER,
            created_at    TEXT NOT NULL,
            updated_at    TEXT NOT NULL,
            -- group_default carries no key; every other kind must carry one.
            CHECK((target_kind = 'group_default') = (target_key = '')),
            -- The games row-id cache belongs to `game` targets only: raw SQL
            -- must not be able to park one on a pad target.
            CHECK(target_kind = 'game' OR game_row_id IS NULL),
            -- A pad identity and a slot fingerprint only exist for controllers.
            CHECK(target_kind NOT IN ('controller','legacy_slot') OR device_group = 'controller'),
            UNIQUE(device_group, target_kind, target_key),
            FOREIGN KEY(preset_id, device_group)
                REFERENCES mapping_presets(id, device_group) ON DELETE RESTRICT))"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_mapping_assignments_preset "
                       "ON mapping_assignments(preset_id)"),
        // Unverified migration sources: converted content plus bookkeeping, held
        // apart from mapping_assignments so a historical opaque key can never
        // become an active target just by being migrated.
        QStringLiteral(R"(CREATE TABLE IF NOT EXISTS mapping_preset_sources (
            device_group        TEXT NOT NULL CHECK(device_group IN ('keyboard','controller','mouse')),
            source_key          TEXT NOT NULL,
            converted_preset_id TEXT,
            status              TEXT NOT NULL DEFAULT 'unverified' CHECK(status IN ('unverified','promoted','retired')),
            created_at          TEXT NOT NULL,
            promoted_at         TEXT,
            note                TEXT NOT NULL DEFAULT '',
            PRIMARY KEY (device_group, source_key),
            FOREIGN KEY(converted_preset_id, device_group)
                REFERENCES mapping_presets(id, device_group) ON DELETE RESTRICT))"),
    };

    if (!m_db.transaction()) {
        qCritical() << "DB: cannot start migration transaction";
        return false;
    }
    for (const QString& sql : statements) {
        QSqlQuery q(m_db);
        if (!q.exec(sql)) {
            qCritical() << "DB: migration v8 failed:" << q.lastError().text();
            m_db.rollback();
            return false;
        }
    }
    QSqlQuery(QStringLiteral("PRAGMA user_version = 8"), m_db);
    return m_db.commit();
}

bool CaptureDatabase::isValidBindingOverrideRow(const BindingOverrideRow& row, QString* error)
{
    // A cleared slot deliberately keeps its gesture and has no trigger, so
    // only the gesture half is meaningful. Validating the trigger here would
    // reject every "unbound" sentinel the editor writes.
    if (row.unbound || row.triggerCode.isEmpty()) {
        const auto gesture = GestureSpec::parse(row.activation, row.tapCount, row.holdMs);
        if (error)
            *error = gesture.error;
        return gesture.ok;
    }

    const auto pattern = BindingPattern::parse(row.deviceGroup, row.triggerCode,
                                               row.activation, row.tapCount, row.holdMs);
    if (error)
        *error = pattern.error;
    return pattern.ok;
}

bool CaptureDatabase::isLegacySlotProfileKey(const QString& key)
{
    const auto matches = [](const QString& value, const char* prefix) {
        const QString head = QString::fromLatin1(prefix);
        if (!value.startsWith(head))
            return false;
        bool ok = false;
        const int slot = value.mid(head.size()).toInt(&ok);
        return ok && slot >= 0;
    };
    return matches(key, "xinput.slot") || matches(key, "winmm.slot");
}

bool CaptureDatabase::applyV9()
{
    // Data-only conversion of the legacy rows (docs/mapping-presets.md section
    // 6). One transaction; `user_version = 9` is the completion marker, so a
    // rolled-back attempt retries cleanly and a completed one never reruns.
    // Real database failures roll back and abort the open (fail-closed);
    // invalid rows are a data condition and are skipped/reported exactly as
    // the resolver skips them at reload().
    QVector<BindingOverrideRow> stored;
    QString readError;
    if (!loadLegacyOverridesChecked(&stored, &readError)) {
        // A failing SELECT must never look like "no legacy rows": migrating on
        // top of an unreadable table would stamp `user_version = 9` and skip
        // this installation's data forever (cpo-p03 review, correction C).
        qCritical() << "DB: migration v9 cannot read binding_overrides:" << readError;
        return false;
    }
    QVector<BindingOverrideRow> validated;
    int rejected = 0;
    for (const BindingOverrideRow& row : stored) {
        QString error;
        if (isValidBindingOverrideRow(row, &error)) {
            validated.append(row);
            continue;
        }
        ++rejected;
        qWarning() << "DB: migration v9 skipping stored override" << row.actionId
                   << "slot" << row.slot << "(" << row.deviceGroup << ")";
    }
    if (rejected > 0)
        qInfo() << "DB: migration v9 skipped" << rejected
                << "invalid binding rows; they stay in binding_overrides for recovery";

    if (!m_db.transaction()) {
        qCritical() << "DB: cannot start migration transaction";
        return false;
    }
    const auto abort = [this](const QString& reason) {
        qCritical() << "DB: migration v9 failed:" << reason;
        m_db.rollback();
        return false;
    };

    // 1) Group-wide rows (`device_profile = ''`) become the group's migration
    //    "Default" preset and its group default assignment. A group without
    //    group-wide rows keeps the built-in default and gets no preset.
    //    The controller group default is created exactly like the contract
    //    says, but the runtime bridge never activates a controller from it:
    //    a pad switches to its preset only after its own chain is
    //    materialized and proven (section 6, reviewer correction B).
    for (const QString& group :
         {QStringLiteral("keyboard"), QStringLiteral("controller"), QStringLiteral("mouse")}) {
        const QVector<MappingPresetRow> content = migrationFoldRows(validated, group, QString());
        if (content.isEmpty())
            continue;
        const QString presetId = insertMappingPresetMetadata(
            group, uniqueMigrationPresetName(group, QStringLiteral("Default")),
            QStringLiteral("migration"));
        if (presetId.isEmpty() || !insertMappingPresetRows(presetId, content))
            return abort(QStringLiteral("group default preset for ") + group);
        if (!upsertMappingAssignmentRow(group, QStringLiteral("group_default"), QString(), presetId, -1))
            return abort(QStringLiteral("group default assignment for ") + group);
    }

    // 2) Every stored non-empty profile key is converted to its own content
    //    (`group-wide + own rows`) and classified by shape, never guessed into
    //    a live assignment:
    //    - `xinput.slotN` / `winmm.slotN` keys stay explicitly slot-scoped
    //      (`legacy_slot` target, controller group only);
    //    - everything else (`controller-<hex>`, fingerprints, provider ids)
    //      waits as an *unverified migration source* for runtime proof - the
    //      storage layer never turns a key string into a durable assignment;
    //    - a key whose rows all fail validation is behaviorally inert: it is
    //      recorded as a retired source (recovery evidence) and never gets a
    //      preset or an assignment.
    for (const QString& group :
         {QStringLiteral("keyboard"), QStringLiteral("controller"), QStringLiteral("mouse")}) {
        QSet<QString> keys;
        for (const BindingOverrideRow& row : stored) {
            if (row.deviceGroup == group && !row.deviceProfile.isEmpty())
                keys.insert(row.deviceProfile);
        }
        QStringList sortedKeys(keys.begin(), keys.end());
        std::sort(sortedKeys.begin(), sortedKeys.end());
        for (const QString& key : sortedKeys) {
            const bool slotKey = group == QLatin1String("controller") && isLegacySlotProfileKey(key);
            // A key only becomes a conversion target when the key ITSELF owns
            // at least one valid, storable row. Group-wide rows alone describe
            // inherited behavior, not a device-specific override (cpo-p03
            // review, correction A): a key whose own rows were all rejected is
            // inert recovery evidence, never a per-key preset or assignment
            // that would stop the device from following its group default.
            QVector<BindingOverrideRow> ownRows;
            for (const BindingOverrideRow& row : validated) {
                if (row.deviceGroup == group && row.deviceProfile == key)
                    ownRows.append(row);
            }
            const QVector<MappingPresetRow> ownContent = migrationFoldRows(ownRows, group, key);
            if (ownContent.isEmpty()) {
                MappingPresetSource inert;
                inert.deviceGroup = group;
                inert.sourceKey = key;
                inert.status = QStringLiteral("retired");
                inert.note = QStringLiteral("no valid own rows at v9; inert");
                if (!upsertMappingPresetSource(inert))
                    return abort(QStringLiteral("inert source ") + key);
                continue;
            }
            const QVector<MappingPresetRow> content = migrationFoldRows(validated, group, key);
            if (content.isEmpty()) {
                MappingPresetSource inert;
                inert.deviceGroup = group;
                inert.sourceKey = key;
                inert.status = QStringLiteral("retired");
                inert.note = QStringLiteral("no valid rows at v9; inert");
                if (!upsertMappingPresetSource(inert))
                    return abort(QStringLiteral("inert source ") + key);
                continue;
            }
            const QString baseName = key.size() > 32 ? key.left(29) + QStringLiteral("...") : key;
            const QString presetId = insertMappingPresetMetadata(
                group, uniqueMigrationPresetName(group, QStringLiteral("Migrated ") + baseName),
                QStringLiteral("migration"));
            if (presetId.isEmpty() || !insertMappingPresetRows(presetId, content))
                return abort(QStringLiteral("converted preset for ") + key);
            if (slotKey) {
                if (!upsertMappingAssignmentRow(QStringLiteral("controller"),
                                                QStringLiteral("legacy_slot"), key, presetId, -1))
                    return abort(QStringLiteral("legacy_slot assignment for ") + key);
            } else {
                MappingPresetSource source;
                source.deviceGroup = group;
                source.sourceKey = key;
                source.convertedPresetId = presetId;
                source.status = QStringLiteral("unverified");
                if (!upsertMappingPresetSource(source))
                    return abort(QStringLiteral("migration source for ") + key);
            }
        }
    }

    QSqlQuery versionWrite(m_db);
    if (!versionWrite.exec(QStringLiteral("PRAGMA user_version = 9"))) {
        // The stamp is the migration's completion marker: assuming it landed
        // would mark a half-converted database as current (correction C).
        qCritical() << "DB: migration v9 could not stamp the schema version:"
                    << versionWrite.lastError().text();
        m_db.rollback();
        return false;
    }
    if (!m_db.commit()) {
        qCritical() << "DB: migration v9 commit failed:" << m_db.lastError().text();
        m_db.rollback();
        return false;
    }
    return true;
}

QString CaptureDatabase::mappingPresetNameKey(const QString& name)
{
    // Contract (docs/mapping-presets.md section 7): names are unique within a
    // device group under trimmed, case-insensitive comparison, so the picker
    // can never show two rows a user cannot tell apart.
    return name.trimmed().toCaseFolded();
}

bool CaptureDatabase::isMappingDeviceGroup(const QString& deviceGroup)
{
    return deviceGroup == QLatin1String("keyboard") || deviceGroup == QLatin1String("controller")
        || deviceGroup == QLatin1String("mouse");
}

bool CaptureDatabase::isMappingTargetKind(const QString& targetKind)
{
    return targetKind == QLatin1String("controller") || targetKind == QLatin1String("legacy_slot")
        || targetKind == QLatin1String("game") || targetKind == QLatin1String("group_default");
}

bool CaptureDatabase::isValidMappingPresetRow(const QString& deviceGroup,
                                              const MappingPresetRow& row)
{
    if (row.actionId.trimmed().isEmpty())
        return false;
    if (row.slot < 1 || row.slot > 2)
        return false;
    if (row.holdMs < 0)
        return false;
    // A row is either one explicit trigger or an explicit "no trigger" — a
    // leftover code on an unbound row would make the intent ambiguous.
    if (row.unbound) {
        if (!row.triggerCode.isEmpty())
            return false;
        // No trigger to recognize, but the gesture still has to be one the
        // runtime understands.
        return GestureSpec::parse(row.activation, row.tapCount, row.holdMs).ok;
    }
    if (row.triggerCode.isEmpty())
        return false;
    // Bound rows go through the same parse boundary the runtime binding model
    // uses, so storage can never accept a row BindingPattern would call
    // malformed (press with a hold duration, a chord outside the controller
    // group, a chord layered with a non-press gesture, ...).
    return BindingPattern::parse(deviceGroup, row.triggerCode, row.activation,
                                 row.tapCount, row.holdMs).ok;
}

bool CaptureDatabase::insertMappingPresetRows(const QString& presetId,
                                              const QVector<MappingPresetRow>& rows)
{
    for (const MappingPresetRow& row : rows) {
        QSqlQuery q(m_db);
        q.prepare(QStringLiteral(
            "INSERT INTO mapping_preset_rows "
            "(preset_id, action_id, slot, trigger_code, activation, hold_ms, unbound, tap_count) "
            "VALUES (:preset, :action, :slot, :trigger, :activation, :hold, :unbound, :taps)"));
        q.bindValue(QStringLiteral(":preset"), presetId);
        q.bindValue(QStringLiteral(":action"), row.actionId);
        q.bindValue(QStringLiteral(":slot"), row.slot);
        q.bindValue(QStringLiteral(":trigger"), row.triggerCode.isEmpty() ? QVariant()
                                                                         : row.triggerCode);
        q.bindValue(QStringLiteral(":activation"), row.activation);
        q.bindValue(QStringLiteral(":hold"), row.holdMs > 0 ? QVariant(row.holdMs) : QVariant());
        q.bindValue(QStringLiteral(":unbound"), row.unbound ? 1 : 0);
        q.bindValue(QStringLiteral(":taps"), row.tapCount);
        if (!q.exec()) {
            qWarning() << "DB: mapping preset row insert failed:" << q.lastError().text();
            return false;
        }
    }
    return true;
}

QVector<MappingPreset> CaptureDatabase::listMappingPresets(const QString& deviceGroup) const
{
    QVector<MappingPreset> out;
    QSqlQuery q(m_db);
    if (deviceGroup.isEmpty()) {
        q.prepare(QStringLiteral(
            "SELECT id, device_group, name, origin, created_at, updated_at FROM mapping_presets "
            "ORDER BY device_group, name_key"));
    } else {
        q.prepare(QStringLiteral(
            "SELECT id, device_group, name, origin, created_at, updated_at FROM mapping_presets "
            "WHERE device_group = :group ORDER BY name_key"));
        q.bindValue(QStringLiteral(":group"), deviceGroup);
    }
    if (!q.exec()) {
        qWarning() << "DB: listMappingPresets failed:" << q.lastError().text();
        return out;
    }
    while (q.next())
        out.append(mappingPresetFromQuery(q));
    return out;
}

MappingPreset CaptureDatabase::mappingPreset(const QString& presetId) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT id, device_group, name, origin, created_at, updated_at FROM mapping_presets "
        "WHERE id = :id"));
    q.bindValue(QStringLiteral(":id"), presetId);
    if (!q.exec()) {
        qWarning() << "DB: mappingPreset failed:" << q.lastError().text();
        return MappingPreset{};
    }
    return q.next() ? mappingPresetFromQuery(q) : MappingPreset{};
}

QString CaptureDatabase::insertMappingPresetMetadata(const QString& deviceGroup, const QString& name,
                                                     const QString& origin)
{
    // Runs inside the caller's transaction on purpose: the public create path
    // and the v9 migration must share one normalization + uniqueness rule
    // instead of each owning a second, nested transaction.
    const QString id = QStringLiteral("preset-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString now = mappingNow();
    QSqlQuery insert(m_db);
    insert.prepare(QStringLiteral(
        "INSERT INTO mapping_presets (id, device_group, name, name_key, origin, created_at, updated_at) "
        "VALUES (:id, :group, :name, :key, :origin, :created, :updated)"));
    insert.bindValue(QStringLiteral(":id"), id);
    insert.bindValue(QStringLiteral(":group"), deviceGroup);
    insert.bindValue(QStringLiteral(":name"), name.trimmed());
    insert.bindValue(QStringLiteral(":key"), mappingPresetNameKey(name));
    insert.bindValue(QStringLiteral(":origin"), origin);
    insert.bindValue(QStringLiteral(":created"), now);
    insert.bindValue(QStringLiteral(":updated"), now);
    if (!insert.exec()) {
        qWarning() << "DB: mapping preset metadata insert failed:" << insert.lastError().text();
        return QString();
    }
    return id;
}

QString CaptureDatabase::uniqueMigrationPresetName(const QString& deviceGroup,
                                                   const QString& base) const
{
    // Deterministic and collision-safe (section 6 review, edge case G): a v8
    // database may already hold a user preset whose name collides with a
    // generated one. The user's row is never renamed or overwritten; the
    // migration takes the next free deterministic suffix.
    QSet<QString> taken;
    for (const MappingPreset& preset : listMappingPresets(deviceGroup))
        taken.insert(mappingPresetNameKey(preset.name));
    if (!taken.contains(mappingPresetNameKey(base)))
        return base;
    for (int suffix = 2; suffix < 1000; ++suffix) {
        const QString candidate = QStringLiteral("%1 (%2)").arg(base).arg(suffix);
        if (!taken.contains(mappingPresetNameKey(candidate)))
            return candidate;
    }
    return base + QStringLiteral(" (")
        + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8) + QLatin1Char(')');
}

QString CaptureDatabase::createMappingPreset(const QString& deviceGroup, const QString& name,
                                             const QVector<MappingPresetRow>& rows,
                                             const QString& origin)
{
    if (!isMappingDeviceGroup(deviceGroup))
        return QString();
    if (name.trimmed().isEmpty() || mappingPresetNameKey(name).isEmpty())
        return QString();
    if (origin != QLatin1String("user") && origin != QLatin1String("migration"))
        return QString();
    if (!isValidPresetContent(deviceGroup, rows))
        return QString();

    if (!m_db.transaction()) {
        qWarning() << "DB: could not open mapping preset transaction:" << m_db.lastError().text();
        return QString();
    }
    const QString id = insertMappingPresetMetadata(deviceGroup, name, origin);
    if (id.isEmpty()) {
        qWarning() << "DB: createMappingPreset failed:" << m_db.lastError().text();
        m_db.rollback();
        return QString();
    }
    if (!insertMappingPresetRows(id, rows)) {
        m_db.rollback();
        return QString();
    }
    if (!m_db.commit()) {
        qWarning() << "DB: mapping preset transaction commit failed:" << m_db.lastError().text();
        m_db.rollback();
        return QString();
    }
    return id;
}

bool CaptureDatabase::isValidPresetContent(const QString& deviceGroup,
                                           const QVector<MappingPresetRow>& rows)
{
    QSet<QString> seen;
    for (const MappingPresetRow& row : rows) {
        if (!isValidMappingPresetRow(deviceGroup, row))
            return false;
        const QString key = row.actionId + QLatin1Char('\n') + QString::number(row.slot);
        if (seen.contains(key))
            return false;
        seen.insert(key);
    }
    return true;
}

QString CaptureDatabase::createUnverifiedMigrationSource(const QString& deviceGroup,
                                                         const QString& name,
                                                         const QVector<MappingPresetRow>& rows,
                                                         const QString& sourceKey)
{
    if (!isMappingDeviceGroup(deviceGroup) || sourceKey.isEmpty())
        return QString();
    if (name.trimmed().isEmpty() || mappingPresetNameKey(name).isEmpty())
        return QString();
    if (!isValidPresetContent(deviceGroup, rows))
        return QString();

    // One transaction for all three writes (correction B): a crash after the
    // preset landed but before its source link exists would otherwise leave an
    // orphan preset that every retry duplicates.
    if (!m_db.transaction()) {
        qWarning() << "DB: could not open migration source transaction:"
                   << m_db.lastError().text();
        return QString();
    }
    const QString id = insertMappingPresetMetadata(deviceGroup, name,
                                                   QStringLiteral("migration"));
    if (id.isEmpty()) {
        qWarning() << "DB: createUnverifiedMigrationSource metadata failed:"
                   << m_db.lastError().text();
        m_db.rollback();
        return QString();
    }
    if (!insertMappingPresetRows(id, rows)) {
        m_db.rollback();
        return QString();
    }
    MappingPresetSource source;
    source.deviceGroup = deviceGroup;
    source.sourceKey = sourceKey;
    source.convertedPresetId = id;
    source.status = QStringLiteral("unverified");
    if (!upsertMappingPresetSource(source)) {
        qWarning() << "DB: createUnverifiedMigrationSource source link failed:"
                   << m_db.lastError().text();
        m_db.rollback();
        return QString();
    }
    if (!m_db.commit()) {
        qWarning() << "DB: migration source transaction commit failed:"
                   << m_db.lastError().text();
        m_db.rollback();
        return QString();
    }
    return id;
}

bool CaptureDatabase::renameMappingPreset(const QString& presetId, const QString& name)
{
    if (presetId.isEmpty() || name.trimmed().isEmpty() || mappingPresetNameKey(name).isEmpty())
        return false;

    if (!m_db.transaction()) {
        qWarning() << "DB: could not open mapping preset transaction:" << m_db.lastError().text();
        return false;
    }
    QSqlQuery update(m_db);
    update.prepare(QStringLiteral(
        "UPDATE mapping_presets SET name = :name, name_key = :key, updated_at = :updated "
        "WHERE id = :id"));
    update.bindValue(QStringLiteral(":name"), name.trimmed());
    update.bindValue(QStringLiteral(":key"), mappingPresetNameKey(name));
    update.bindValue(QStringLiteral(":updated"), mappingNow());
    update.bindValue(QStringLiteral(":id"), presetId);
    const bool ok = update.exec() && update.numRowsAffected() == 1;
    if (!ok) {
        qWarning() << "DB: renameMappingPreset failed:" << update.lastError().text();
        m_db.rollback();
        return false;
    }
    if (!m_db.commit()) {
        qWarning() << "DB: renameMappingPreset commit failed:" << m_db.lastError().text();
        m_db.rollback();
        return false;
    }
    return true;
}

bool CaptureDatabase::replaceMappingPresetRows(const QString& presetId,
                                               const QVector<MappingPresetRow>& rows)
{
    if (presetId.isEmpty())
        return false;
    const MappingPreset preset = mappingPreset(presetId);
    if (preset.id.isEmpty())
        return false;
    for (const MappingPresetRow& row : rows) {
        if (!isValidMappingPresetRow(preset.deviceGroup, row))
            return false;
    }
    // Replacing is a whole-set operation: the stored set always equals `rows`.
    // Duplicates would violate the primary key halfway through, so they are a
    // rejected input, not a silent last-one-wins.
    QSet<QString> seen;
    for (const MappingPresetRow& row : rows) {
        const QString key = row.actionId + QLatin1Char('\n') + QString::number(row.slot);
        if (seen.contains(key))
            return false;
        seen.insert(key);
    }

    if (!m_db.transaction()) {
        qWarning() << "DB: could not open mapping preset transaction:" << m_db.lastError().text();
        return false;
    }
    QSqlQuery remove(m_db);
    remove.prepare(QStringLiteral("DELETE FROM mapping_preset_rows WHERE preset_id = :id"));
    remove.bindValue(QStringLiteral(":id"), presetId);
    QSqlQuery touch(m_db);
    touch.prepare(QStringLiteral("UPDATE mapping_presets SET updated_at = :now WHERE id = :id"));
    touch.bindValue(QStringLiteral(":now"), mappingNow());
    touch.bindValue(QStringLiteral(":id"), presetId);
    if (!remove.exec()) {
        qWarning() << "DB: replaceMappingPresetRows failed:" << remove.lastError().text();
        m_db.rollback();
        return false;
    }
    // The metadata touch doubles as the existence check: exactly one row has to
    // be updated, so a preset that vanished mid-call is a failure even when the
    // replacement set is empty (zero rows deleted, zero inserted).
    if (!touch.exec() || touch.numRowsAffected() != 1) {
        qWarning() << "DB: replaceMappingPresetRows on a missing preset:" << presetId;
        m_db.rollback();
        return false;
    }
    if (!insertMappingPresetRows(presetId, rows)) {
        m_db.rollback();
        return false;
    }
    if (!m_db.commit()) {
        qWarning() << "DB: mapping preset transaction commit failed:" << m_db.lastError().text();
        m_db.rollback();
        return false;
    }
    return true;
}

QVector<MappingPresetRow> CaptureDatabase::mappingPresetRows(const QString& presetId) const
{
    QVector<MappingPresetRow> out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT action_id, slot, trigger_code, activation, hold_ms, unbound, tap_count "
        "FROM mapping_preset_rows WHERE preset_id = :id ORDER BY action_id, slot"));
    q.bindValue(QStringLiteral(":id"), presetId);
    if (!q.exec()) {
        qWarning() << "DB: mappingPresetRows failed:" << q.lastError().text();
        return out;
    }
    while (q.next()) {
        MappingPresetRow row;
        row.actionId     = q.value(0).toString();
        row.slot         = q.value(1).toInt();
        row.triggerCode  = q.value(2).toString();
        row.activation   = q.value(3).toString();
        row.holdMs       = q.value(4).isNull() ? 0 : q.value(4).toInt();
        row.unbound      = q.value(5).toInt() != 0;
        row.tapCount     = q.value(6).isNull() ? 1 : q.value(6).toInt();
        out.append(row);
    }
    return out;
}

int CaptureDatabase::mappingPresetReferenceCount(const QString& presetId) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT (SELECT COUNT(*) FROM mapping_assignments WHERE preset_id = :id) "
        "+ (SELECT COUNT(*) FROM mapping_preset_sources WHERE converted_preset_id = :id)"));
    q.bindValue(QStringLiteral(":id"), presetId);
    if (!q.exec()) {
        qWarning() << "DB: mappingPresetReferenceCount failed:" << q.lastError().text();
        return 0;
    }
    return q.next() ? q.value(0).toInt() : 0;
}

bool CaptureDatabase::deleteMappingPreset(const QString& presetId)
{
    if (presetId.isEmpty() || mappingPreset(presetId).id.isEmpty())
        return false;
    // A referenced preset is refused, never half-deleted: every target either
    // keeps pointing at an existing preset or the delete simply does not happen.
    if (mappingPresetReferenceCount(presetId) > 0)
        return false;

    if (!m_db.transaction()) {
        qWarning() << "DB: could not open mapping preset transaction:" << m_db.lastError().text();
        return false;
    }
    QSqlQuery remove(m_db);
    remove.prepare(QStringLiteral("DELETE FROM mapping_presets WHERE id = :id"));
    remove.bindValue(QStringLiteral(":id"), presetId);
    if (!remove.exec() || remove.numRowsAffected() != 1) {
        qWarning() << "DB: deleteMappingPreset failed:" << remove.lastError().text();
        m_db.rollback();
        return false;
    }
    if (!m_db.commit()) {
        qWarning() << "DB: deleteMappingPreset commit failed:" << m_db.lastError().text();
        m_db.rollback();
        return false;
    }
    return true;
}

bool CaptureDatabase::deleteMappingPresetAndReassign(const QString& presetId,
                                                     const QString& keepPresetId)
{
    if (presetId.isEmpty() || keepPresetId.isEmpty() || presetId == keepPresetId)
        return false;
    const MappingPreset victim = mappingPreset(presetId);
    const MappingPreset keep = mappingPreset(keepPresetId);
    if (victim.id.isEmpty() || keep.id.isEmpty() || victim.deviceGroup != keep.deviceGroup)
        return false;

    if (!m_db.transaction()) {
        qWarning() << "DB: could not open mapping preset transaction:" << m_db.lastError().text();
        return false;
    }
    // A migration source is recovery evidence for one specific historical key;
    // it cannot be "reassigned" to another content, so it refuses the delete.
    QSqlQuery sources(m_db);
    sources.prepare(QStringLiteral(
        "SELECT COUNT(*) FROM mapping_preset_sources WHERE converted_preset_id = :id"));
    sources.bindValue(QStringLiteral(":id"), presetId);
    if (!sources.exec() || !sources.next() || sources.value(0).toInt() > 0) {
        m_db.rollback();
        return false;
    }
    QSqlQuery move(m_db);
    move.prepare(QStringLiteral(
        "UPDATE mapping_assignments SET preset_id = :keep, updated_at = :now "
        "WHERE preset_id = :victim"));
    move.bindValue(QStringLiteral(":keep"), keepPresetId);
    move.bindValue(QStringLiteral(":now"), mappingNow());
    move.bindValue(QStringLiteral(":victim"), presetId);
    if (!move.exec()) {
        qWarning() << "DB: reassigning mapping targets failed:" << move.lastError().text();
        m_db.rollback();
        return false;
    }
    QSqlQuery remove(m_db);
    remove.prepare(QStringLiteral("DELETE FROM mapping_presets WHERE id = :id"));
    remove.bindValue(QStringLiteral(":id"), presetId);
    if (!remove.exec() || remove.numRowsAffected() != 1) {
        qWarning() << "DB: deleteMappingPresetAndReassign failed:" << remove.lastError().text();
        m_db.rollback();
        return false;
    }
    if (!m_db.commit()) {
        qWarning() << "DB: mapping preset transaction commit failed:" << m_db.lastError().text();
        m_db.rollback();
        return false;
    }
    return true;
}

QVector<MappingAssignment> CaptureDatabase::listMappingAssignments(
    const QString& deviceGroup) const
{
    QVector<MappingAssignment> out;
    QSqlQuery q(m_db);
    if (deviceGroup.isEmpty()) {
        q.prepare(QStringLiteral(
            "SELECT id, device_group, preset_id, target_kind, target_key, game_row_id "
            "FROM mapping_assignments ORDER BY device_group, target_kind, target_key"));
    } else {
        q.prepare(QStringLiteral(
            "SELECT id, device_group, preset_id, target_kind, target_key, game_row_id "
            "FROM mapping_assignments WHERE device_group = :group "
            "ORDER BY target_kind, target_key"));
        q.bindValue(QStringLiteral(":group"), deviceGroup);
    }
    if (!q.exec()) {
        qWarning() << "DB: listMappingAssignments failed:" << q.lastError().text();
        return out;
    }
    while (q.next()) {
        MappingAssignment assignment;
        assignment.id          = q.value(0).toInt();
        assignment.deviceGroup = q.value(1).toString();
        assignment.presetId    = q.value(2).toString();
        assignment.targetKind  = q.value(3).toString();
        assignment.targetKey   = q.value(4).toString();
        assignment.gameRowId   = q.value(5).isNull() ? -1 : q.value(5).toInt();
        out.append(assignment);
    }
    return out;
}

MappingAssignment CaptureDatabase::mappingAssignment(const QString& deviceGroup,
                                                     const QString& targetKind,
                                                     const QString& targetKey) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT id, device_group, preset_id, target_kind, target_key, game_row_id "
        "FROM mapping_assignments "
        "WHERE device_group = :group AND target_kind = :kind AND target_key = IFNULL(:key, '')"));
    q.bindValue(QStringLiteral(":group"), deviceGroup);
    q.bindValue(QStringLiteral(":kind"), targetKind);
    q.bindValue(QStringLiteral(":key"), targetKey);
    if (!q.exec()) {
        qWarning() << "DB: mappingAssignment failed:" << q.lastError().text();
        return MappingAssignment{};
    }
    if (!q.next())
        return MappingAssignment{};
    MappingAssignment assignment;
    assignment.id          = q.value(0).toInt();
    assignment.deviceGroup = q.value(1).toString();
    assignment.presetId    = q.value(2).toString();
    assignment.targetKind  = q.value(3).toString();
    assignment.targetKey   = q.value(4).toString();
    assignment.gameRowId   = q.value(5).isNull() ? -1 : q.value(5).toInt();
    return assignment;
}

bool CaptureDatabase::upsertMappingAssignmentRow(const QString& deviceGroup,
                                                 const QString& targetKind,
                                                 const QString& targetKey,
                                                 const QString& presetId, int gameRowId)
{
    const QString now = mappingNow();
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO mapping_assignments "
        "(device_group, target_kind, target_key, preset_id, game_row_id, created_at, updated_at) "
        "VALUES (:group, :kind, IFNULL(:key, ''), :preset, :game, :created, :updated) "
        "ON CONFLICT(device_group, target_kind, target_key) DO UPDATE SET "
        "preset_id = excluded.preset_id, game_row_id = excluded.game_row_id, "
        "updated_at = excluded.updated_at"));
    q.bindValue(QStringLiteral(":group"), deviceGroup);
    q.bindValue(QStringLiteral(":kind"), targetKind);
    q.bindValue(QStringLiteral(":key"), targetKey);
    q.bindValue(QStringLiteral(":preset"), presetId);
    // Only a `game` target can carry a row id, and it stays optional: the
    // assignment matches on the executable key alone.
    q.bindValue(QStringLiteral(":game"),
                targetKind == QLatin1String("game") && gameRowId > 0 ? QVariant(gameRowId)
                                                                     : QVariant());
    q.bindValue(QStringLiteral(":created"), now);
    q.bindValue(QStringLiteral(":updated"), now);
    if (!q.exec()) {
        qWarning() << "DB: mapping assignment write failed:" << q.lastError().text();
        return false;
    }
    return true;
}

bool CaptureDatabase::setMappingAssignment(const QString& deviceGroup, const QString& targetKind,
                                           const QString& targetKey, const QString& presetId,
                                           int gameRowId)
{
    if (!isMappingDeviceGroup(deviceGroup) || !isMappingTargetKind(targetKind))
        return false;
    // The kind decides the key rules — the text never does. `controller-…`,
    // `xinput.slotN` and a game executable are all opaque strings here.
    if (targetKind == QLatin1String("group_default")) {
        if (!targetKey.isEmpty())
            return false;
    } else if (targetKey.isEmpty()) {
        return false;
    }
    if ((targetKind == QLatin1String("controller") || targetKind == QLatin1String("legacy_slot"))
        && deviceGroup != QLatin1String("controller")) {
        return false;
    }
    const MappingPreset preset = mappingPreset(presetId);
    if (preset.id.isEmpty() || preset.deviceGroup != deviceGroup)
        return false;

    if (!m_db.transaction()) {
        qWarning() << "DB: could not open mapping assignment transaction:"
                   << m_db.lastError().text();
        return false;
    }
    if (!upsertMappingAssignmentRow(deviceGroup, targetKind, targetKey, presetId, gameRowId)) {
        qWarning() << "DB: setMappingAssignment failed for target" << targetKind << targetKey;
        m_db.rollback();
        return false;
    }
    if (!m_db.commit()) {
        qWarning() << "DB: mapping assignment transaction commit failed:"
                   << m_db.lastError().text();
        m_db.rollback();
        return false;
    }
    return true;
}

bool CaptureDatabase::clearMappingAssignment(const QString& deviceGroup, const QString& targetKind,
                                             const QString& targetKey)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "DELETE FROM mapping_assignments "
        "WHERE device_group = :group AND target_kind = :kind AND target_key = IFNULL(:key, '')"));
    q.bindValue(QStringLiteral(":group"), deviceGroup);
    q.bindValue(QStringLiteral(":kind"), targetKind);
    q.bindValue(QStringLiteral(":key"), targetKey);
    if (!q.exec()) {
        qWarning() << "DB: clearMappingAssignment failed:" << q.lastError().text();
        return false;
    }
    return true;
}

QVector<MappingPresetSource> CaptureDatabase::listMappingPresetSources(
    const QString& deviceGroup) const
{
    QVector<MappingPresetSource> out;
    QSqlQuery q(m_db);
    if (deviceGroup.isEmpty()) {
        q.prepare(QStringLiteral(
            "SELECT device_group, source_key, converted_preset_id, status, created_at, "
            "promoted_at, note FROM mapping_preset_sources ORDER BY device_group, source_key"));
    } else {
        q.prepare(QStringLiteral(
            "SELECT device_group, source_key, converted_preset_id, status, created_at, "
            "promoted_at, note FROM mapping_preset_sources WHERE device_group = :group "
            "ORDER BY source_key"));
        q.bindValue(QStringLiteral(":group"), deviceGroup);
    }
    if (!q.exec()) {
        qWarning() << "DB: listMappingPresetSources failed:" << q.lastError().text();
        return out;
    }
    while (q.next())
        out.append(mappingSourceFromQuery(q));
    return out;
}

MappingPresetSource CaptureDatabase::mappingPresetSource(const QString& deviceGroup,
                                                         const QString& sourceKey) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT device_group, source_key, converted_preset_id, status, created_at, promoted_at, "
        "note FROM mapping_preset_sources WHERE device_group = :group AND source_key = :key"));
    q.bindValue(QStringLiteral(":group"), deviceGroup);
    q.bindValue(QStringLiteral(":key"), sourceKey);
    if (!q.exec()) {
        qWarning() << "DB: mappingPresetSource failed:" << q.lastError().text();
        return MappingPresetSource{};
    }
    return q.next() ? mappingSourceFromQuery(q) : MappingPresetSource{};
}

bool CaptureDatabase::upsertMappingPresetSource(const MappingPresetSource& source)
{
    if (!isMappingDeviceGroup(source.deviceGroup) || source.sourceKey.isEmpty())
        return false;
    const QString status = source.status.isEmpty() ? QStringLiteral("unverified") : source.status;
    if (!knownSourceStatus(status))
        return false;
    // `promoted` is the durable outcome of runtime proof, not bookkeeping: only
    // promoteMappingPresetSource() may write it. A row that already is promoted
    // is terminal here too — relinking or downgrading it would separate the
    // evidence from the assignment it proved.
    if (status == QLatin1String("promoted"))
        return false;
    if (mappingPresetSource(source.deviceGroup, source.sourceKey).status
        == QLatin1String("promoted"))
        return false;
    if (!source.convertedPresetId.isEmpty()) {
        const MappingPreset converted = mappingPreset(source.convertedPresetId);
        if (converted.id.isEmpty() || converted.deviceGroup != source.deviceGroup)
            return false;
    }

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO mapping_preset_sources "
        "(device_group, source_key, converted_preset_id, status, created_at, promoted_at, note) "
        "VALUES (:group, :key, :preset, :status, :created, NULL, IFNULL(:note, '')) "
        "ON CONFLICT(device_group, source_key) DO UPDATE SET "
        "converted_preset_id = excluded.converted_preset_id, status = excluded.status, "
        "note = excluded.note "
        "WHERE mapping_preset_sources.status <> 'promoted'"));
    q.bindValue(QStringLiteral(":group"), source.deviceGroup);
    q.bindValue(QStringLiteral(":key"), source.sourceKey);
    q.bindValue(QStringLiteral(":preset"),
                source.convertedPresetId.isEmpty() ? QVariant() : source.convertedPresetId);
    q.bindValue(QStringLiteral(":status"), status);
    q.bindValue(QStringLiteral(":created"),
                source.createdAt.isEmpty() ? mappingNow() : source.createdAt);
    q.bindValue(QStringLiteral(":note"), source.note);
    if (!q.exec() || q.numRowsAffected() != 1) {
        qWarning() << "DB: upsertMappingPresetSource failed:" << q.lastError().text();
        return false;
    }
    // Deliberately no assignment write here: a migrated key is not an active
    // target until runtime proves it (docs/mapping-presets.md section 6).
    return true;
}

bool CaptureDatabase::setMappingPresetSourceStatus(const QString& deviceGroup,
                                                   const QString& sourceKey,
                                                   const QString& status)
{
    if (!isMappingDeviceGroup(deviceGroup) || sourceKey.isEmpty())
        return false;
    // `promoted` needs the atomic assignment + bookkeeping path, and a promoted
    // row cannot be downgraded through this generic setter either.
    if (!knownSourceStatus(status) || status == QLatin1String("promoted"))
        return false;

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "UPDATE mapping_preset_sources SET status = :status, promoted_at = NULL "
        "WHERE device_group = :group AND source_key = :key AND status <> 'promoted'"));
    q.bindValue(QStringLiteral(":status"), status);
    q.bindValue(QStringLiteral(":group"), deviceGroup);
    q.bindValue(QStringLiteral(":key"), sourceKey);
    if (!q.exec() || q.numRowsAffected() != 1) {
        qWarning() << "DB: setMappingPresetSourceStatus failed:" << q.lastError().text();
        return false;
    }
    return true;
}

bool CaptureDatabase::promoteMappingPresetSource(const QString& deviceGroup,
                                                 const QString& sourceKey,
                                                 const QString& controllerKey)
{
    // A durable pad identity only exists in the controller group, and the
    // transition needs every part of it to make sense.
    if (deviceGroup != QLatin1String("controller") || sourceKey.isEmpty()
        || controllerKey.isEmpty())
        return false;

    const MappingPresetSource source = mappingPresetSource(deviceGroup, sourceKey);
    if (source.sourceKey.isEmpty() || source.status != QLatin1String("unverified"))
        return false;
    const MappingPreset converted = mappingPreset(source.convertedPresetId);
    if (converted.id.isEmpty() || converted.deviceGroup != deviceGroup)
        return false;

    if (!m_db.transaction()) {
        qWarning() << "DB: could not open mapping promotion transaction:"
                   << m_db.lastError().text();
        return false;
    }
    // Half one: the durable assignment the runtime identity proof authorizes.
    if (!upsertMappingAssignmentRow(deviceGroup, QStringLiteral("controller"), controllerKey,
                                    source.convertedPresetId, -1)) {
        qWarning() << "DB: promoteMappingPresetSource assignment failed:" << sourceKey;
        m_db.rollback();
        return false;
    }
    // Half two: the source itself, and only while it is still unverified, so a
    // generic write can never be overwritten into a false promotion.
    QSqlQuery mark(m_db);
    mark.prepare(QStringLiteral(
        "UPDATE mapping_preset_sources SET status = 'promoted', promoted_at = :now "
        "WHERE device_group = :group AND source_key = :key AND status = 'unverified'"));
    mark.bindValue(QStringLiteral(":now"), mappingNow());
    mark.bindValue(QStringLiteral(":group"), deviceGroup);
    mark.bindValue(QStringLiteral(":key"), sourceKey);
    if (!mark.exec() || mark.numRowsAffected() != 1) {
        qWarning() << "DB: promoteMappingPresetSource status failed:" << mark.lastError().text();
        m_db.rollback();
        return false;
    }
    if (!m_db.commit()) {
        qWarning() << "DB: mapping promotion transaction commit failed:"
                   << m_db.lastError().text();
        m_db.rollback();
        return false;
    }
    return true;
}
