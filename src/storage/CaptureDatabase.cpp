#include "storage/CaptureDatabase.h"
#include "core/GameIdentity.h"
#include "storage/GameIconCache.h"
#include "storage/CaptureQueries.h"
#include "storage/GameRowRepair.h"
#include "config/Paths.h"

#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QSet>
#include <QStringList>
#include <QVariant>
#include <QDebug>

// Core: open, captures, games, controller layouts and watched folders. Schema
// migrations and the startup repair pass live in CaptureDatabase_Migrations.cpp,
// the binding store in CaptureDatabase_Bindings.cpp and the mapping preset
// store in CaptureDatabase_MappingPresets.cpp.

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
    // Session-only: the capture listings exclude these paths, so the table
    // must exist before the first query. TEMP never touches the database file.
    QSqlQuery missing(m_db);
    if (!missing.exec(QStringLiteral(
            "CREATE TEMP TABLE IF NOT EXISTS missing_captures (file_path TEXT PRIMARY KEY)"))) {
        qCritical() << "DB: could not create the missing-captures table:"
                    << missing.lastError().text();
        return false;
    }
    return migrate();
}

int CaptureDatabase::schemaVersion() const
{
    QSqlQuery q(QStringLiteral("PRAGMA user_version"), m_db);
    return q.next() ? q.value(0).toInt() : 0;
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

QSet<QString> CaptureDatabase::missingCaptureKeys() const
{
    QSet<QString> out;
    QSqlQuery q(QStringLiteral("SELECT file_path FROM temp.missing_captures"), m_db);
    while (q.next())
        out.insert(q.value(0).toString());
    return out;
}

int CaptureDatabase::setCapturesMissing(const QStringList& storedKeys, bool missing)
{
    if (storedKeys.isEmpty())
        return 0;
    QSqlQuery q(m_db);
    q.prepare(missing
        ? QStringLiteral("INSERT OR IGNORE INTO temp.missing_captures (file_path) VALUES (:p)")
        : QStringLiteral("DELETE FROM temp.missing_captures WHERE file_path = :p"));
    int changed = 0;
    m_db.transaction();
    for (const QString& key : storedKeys) {
        q.bindValue(QStringLiteral(":p"), key);
        if (q.exec())
            changed += q.numRowsAffected() > 0 ? 1 : 0;
        else
            qWarning() << "DB: setCapturesMissing failed:" << q.lastError().text();
    }
    m_db.commit();
    return changed;
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
