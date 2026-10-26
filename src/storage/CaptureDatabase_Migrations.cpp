// Schema migrations (V1-V9) and the startup repair pass of CaptureDatabase.
#include "storage/CaptureDatabase.h"
#include "storage/GameIconCache.h"
#include "storage/GameMetadataBackfill.h"
#include "storage/GameRowRepair.h"
#include "config/Paths.h"

#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QSqlError>
#include <QSqlQuery>
#include <QSet>
#include <QStringList>
#include <QVariant>
#include <QDebug>
#include <algorithm>

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
        // file_path is UNIQUE (case-sensitive), and the canonical spelling can
        // still be held by a tombstone: a duplicate tombstoned just above, a
        // case variant, or a capture deleted earlier. The live winner is the
        // capture's current state and already carries the merged favorite and
        // thumbnail, so such a tombstone is superseded and goes. A live row
        // cannot hold it: it would have been grouped and tombstoned here.
        QSqlQuery clearTombstone(m_db);
        clearTombstone.prepare(QStringLiteral(
            "DELETE FROM captures WHERE file_path = :path AND id != :id AND deleted_at IS NOT NULL"));
        clearTombstone.bindValue(QStringLiteral(":path"), canonicalPath);
        clearTombstone.bindValue(QStringLiteral(":id"), rows[winnerIndex].id);
        if (!clearTombstone.exec())
            return abortRepair(QStringLiteral("could not clear a superseded capture tombstone: ")
                               + clearTombstone.lastError().text());
        if (clearTombstone.numRowsAffected() > 0)
            qInfo() << "DB: startup repair merged" << clearTombstone.numRowsAffected()
                    << "superseded tombstone(s) into" << canonicalPath;
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
            // folders.path is UNIQUE: when the repaired path is already
            // registered, this row is the same folder under its old spelling.
            // Keep the existing row (watch state included) and drop this one.
            if (qstrcmp(spec.table, "folders") == 0) {
                QSqlQuery existing(m_db);
                existing.prepare(QStringLiteral("SELECT id, is_watched FROM folders WHERE path = :path AND id != :id"));
                existing.bindValue(QStringLiteral(":path"), newValue);
                existing.bindValue(QStringLiteral(":id"), select.value(0));
                if (!existing.exec())
                    return abortRepair(QStringLiteral("could not check a repaired folder: ")
                                       + existing.lastError().text());
                if (existing.next()) {
                    const QVariant keeperId = existing.value(0);
                    existing.finish();
                    QSqlQuery merge(m_db);
                    merge.prepare(QStringLiteral(
                        "UPDATE folders SET is_watched = MAX(is_watched, "
                        "(SELECT is_watched FROM folders WHERE id = :old)) WHERE id = :keep"));
                    merge.bindValue(QStringLiteral(":old"), select.value(0));
                    merge.bindValue(QStringLiteral(":keep"), keeperId);
                    QSqlQuery drop(m_db);
                    drop.prepare(QStringLiteral("DELETE FROM folders WHERE id = :id"));
                    drop.bindValue(QStringLiteral(":id"), select.value(0));
                    if (!merge.exec() || !drop.exec())
                        return abortRepair(QStringLiteral("could not merge a repaired folder: ")
                                           + merge.lastError().text() + drop.lastError().text());
                    qInfo() << "DB: startup repair merged folder" << oldValue << "into" << newValue;
                    continue;
                }
            }
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

namespace
{
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
