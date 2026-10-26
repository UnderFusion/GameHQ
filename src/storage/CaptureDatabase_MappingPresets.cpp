// Mapping preset store of CaptureDatabase (schema v8/v9 tables).
#include "storage/CaptureDatabase.h"
#include "storage/SqlTransaction.h"
#include "core/GameIdentity.h"
#include "input/BindingPattern.h"

#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>
#include <QSet>
#include <QUuid>
#include <QVariant>
#include <QDebug>

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

} // namespace

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
                                                     const QString& origin,
                                                     const QString& explicitId)
{
    // Runs inside the caller's transaction on purpose: the public create path
    // and the v9 migration must share one normalization + uniqueness rule
    // instead of each owning a second, nested transaction. `explicitId` is the
    // reserved Built-in artifact's identity (cpo-p06); user presets always get
    // a generated opaque id.
    const QString id = explicitId.isEmpty()
        ? QStringLiteral("preset-") + QUuid::createUuid().toString(QUuid::WithoutBraces)
        : explicitId;
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
    // The reserved Built-in sentinel is not a name a user can create or copy.
    if (isReservedMappingPresetName(name))
        return QString();
    if (origin != QLatin1String("user") && origin != QLatin1String("migration"))
        return QString();
    if (!isValidPresetContent(deviceGroup, rows))
        return QString();

    SqlTransaction transaction(m_db);
    if (!transaction.begin()) {
        qWarning() << "DB: could not open mapping preset transaction:" << m_db.lastError().text();
        return QString();
    }
    const QString id = insertMappingPresetMetadata(deviceGroup, name, origin);
    if (id.isEmpty()) {
        qWarning() << "DB: createMappingPreset failed:" << m_db.lastError().text();
        return QString();
    }
    if (!insertMappingPresetRows(id, rows)) {
        return QString();
    }
    if (!transaction.commit()) {
        qWarning() << "DB: mapping preset transaction commit failed:" << m_db.lastError().text();
        return QString();
    }
    return id;
}

QString CaptureDatabase::builtinMappingPresetId(const QString& deviceGroup)
{
    // ------------------------------------------------------------------ cpo-p06
    // A reserved IDENTITY, not a name and not an origin value: `origin` is
    // constrained to user|migration by the schema, and a name would be
    // user-visible, translatable and collidable. User preset ids are always
    // `preset-<uuid>`, so this namespace cannot collide with them.
    if (!isMappingDeviceGroup(deviceGroup))
        return QString();
    return QStringLiteral("builtin-") + deviceGroup;
}

bool CaptureDatabase::isBuiltinMappingPresetId(const QString& presetId)
{
    return presetId == QStringLiteral("builtin-keyboard")
        || presetId == QStringLiteral("builtin-controller")
        || presetId == QStringLiteral("builtin-mouse");
}

bool CaptureDatabase::isReservedMappingPresetName(const QString& name)
{
    // The artifact's stored name is a sentinel in a namespace no user input can
    // produce (a leading control character). Nothing ever infers "builtin" from
    // a translated or user-visible label; the id is the only marker.
    return mappingPresetNameKey(name).startsWith(QChar(0x01));
}

QString CaptureDatabase::ensureBuiltinMappingPreset(const QString& deviceGroup)
{
    const QString reservedId = builtinMappingPresetId(deviceGroup);
    if (reservedId.isEmpty())
        return QString();
    if (!mappingPreset(reservedId).id.isEmpty())
        return reservedId;
    SqlTransaction transaction(m_db);
    if (!transaction.begin()) {
        qWarning() << "DB: could not open built-in preset transaction:"
                   << m_db.lastError().text();
        return QString();
    }
    const QString id = insertMappingPresetMetadata(
        deviceGroup, QStringLiteral("\u0001builtin"), QStringLiteral("user"), reservedId);
    if (id.isEmpty() || !transaction.commit()) {
        qWarning() << "DB: ensureBuiltinMappingPreset failed:" << m_db.lastError().text();
        return QString();
    }
    return id;
}

QString CaptureDatabase::createMappingPresetAssigned(const QString& deviceGroup,
                                                     const QString& name,
                                                     const QVector<MappingPresetRow>& rows,
                                                     const QString& targetKind,
                                                     const QString& targetKey, int gameRowId)
{
    // One transaction for metadata + content + the target assignment: a failure
    // can leave neither an orphan preset nor a partial assignment (cpo-p06).
    if (!isMappingDeviceGroup(deviceGroup))
        return QString();
    if (name.trimmed().isEmpty() || mappingPresetNameKey(name).isEmpty()
        || isReservedMappingPresetName(name))
        return QString();
    if (!isValidPresetContent(deviceGroup, rows))
        return QString();
    QString key;
    if (!validateMappingAssignmentTarget(deviceGroup, targetKind, targetKey, &key, nullptr))
        return QString();

    SqlTransaction transaction(m_db);
    if (!transaction.begin()) {
        qWarning() << "DB: could not open preset+assignment transaction:" << m_db.lastError().text();
        return QString();
    }
    const QString id = insertMappingPresetMetadata(deviceGroup, name, QStringLiteral("user"));
    if (id.isEmpty() || !insertMappingPresetRows(id, rows)
        || !upsertMappingAssignmentRow(deviceGroup, targetKind, key, id, gameRowId)) {
        qWarning() << "DB: createMappingPresetAssigned failed:" << m_db.lastError().text();
        return QString();
    }
    if (!transaction.commit()) {
        qWarning() << "DB: preset+assignment transaction commit failed:" << m_db.lastError().text();
        return QString();
    }
    return id;
}

QString CaptureDatabase::assignMappingTargetToBuiltin(const QString& deviceGroup,
                                                      const QString& targetKind,
                                                      const QString& targetKey, int gameRowId)
{
    QString key;
    if (!validateMappingAssignmentTarget(deviceGroup, targetKind, targetKey, &key, nullptr))
        return QString();
    const QString reservedId = builtinMappingPresetId(deviceGroup);
    if (reservedId.isEmpty())
        return QString();

    // Ensure + assign in ONE transaction: a user who selects "Built-in
    // defaults" can never end up with an assignment that points at a preset the
    // same operation failed to create.
    SqlTransaction transaction(m_db);
    if (!transaction.begin()) {
        qWarning() << "DB: could not open built-in assignment transaction:"
                   << m_db.lastError().text();
        return QString();
    }
    QString id = reservedId;
    if (mappingPreset(reservedId).id.isEmpty()) {
        id = insertMappingPresetMetadata(deviceGroup, QStringLiteral("\u0001builtin"),
                                         QStringLiteral("user"), reservedId);
    }
    if (id.isEmpty() || !upsertMappingAssignmentRow(deviceGroup, targetKind, key, id, gameRowId)) {
        qWarning() << "DB: assignMappingTargetToBuiltin failed:" << m_db.lastError().text();
        return QString();
    }
    if (!transaction.commit()) {
        qWarning() << "DB: built-in assignment transaction commit failed:"
                   << m_db.lastError().text();
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
    SqlTransaction transaction(m_db);
    if (!transaction.begin()) {
        qWarning() << "DB: could not open migration source transaction:"
                   << m_db.lastError().text();
        return QString();
    }
    const QString id = insertMappingPresetMetadata(deviceGroup, name,
                                                   QStringLiteral("migration"));
    if (id.isEmpty()) {
        qWarning() << "DB: createUnverifiedMigrationSource metadata failed:"
                   << m_db.lastError().text();
        return QString();
    }
    if (!insertMappingPresetRows(id, rows)) {
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
        return QString();
    }
    if (!transaction.commit()) {
        qWarning() << "DB: migration source transaction commit failed:"
                   << m_db.lastError().text();
        return QString();
    }
    return id;
}

bool CaptureDatabase::renameMappingPreset(const QString& presetId, const QString& name)
{
    if (presetId.isEmpty() || name.trimmed().isEmpty() || mappingPresetNameKey(name).isEmpty())
        return false;
    // The reserved Built-in artifact keeps its identity and its sentinel name:
    // normal CRUD can neither rename it nor steal its name (cpo-p06).
    if (isBuiltinMappingPresetId(presetId) || isReservedMappingPresetName(name))
        return false;

    SqlTransaction transaction(m_db);
    if (!transaction.begin()) {
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
        return false;
    }
    if (!transaction.commit()) {
        qWarning() << "DB: renameMappingPreset commit failed:" << m_db.lastError().text();
        return false;
    }
    return true;
}

bool CaptureDatabase::replaceMappingPresetRows(const QString& presetId,
                                               const QVector<MappingPresetRow>& rows)
{
    if (presetId.isEmpty())
        return false;
    // The Built-in artifact is an explicit EMPTY winner: filling it would turn
    // "built-in defaults" into a second, invisible user preset (cpo-p06).
    if (isBuiltinMappingPresetId(presetId))
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

    SqlTransaction transaction(m_db);
    if (!transaction.begin()) {
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
        return false;
    }
    // The metadata touch doubles as the existence check: exactly one row has to
    // be updated, so a preset that vanished mid-call is a failure even when the
    // replacement set is empty (zero rows deleted, zero inserted).
    if (!touch.exec() || touch.numRowsAffected() != 1) {
        qWarning() << "DB: replaceMappingPresetRows on a missing preset:" << presetId;
        return false;
    }
    if (!insertMappingPresetRows(presetId, rows)) {
        return false;
    }
    if (!transaction.commit()) {
        qWarning() << "DB: mapping preset transaction commit failed:" << m_db.lastError().text();
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
    // The reserved Built-in artifact is not deletable through normal CRUD; the
    // assignment must first move away, and the artifact then stays reusable.
    if (isBuiltinMappingPresetId(presetId))
        return false;

    SqlTransaction transaction(m_db);
    if (!transaction.begin()) {
        qWarning() << "DB: could not open mapping preset transaction:" << m_db.lastError().text();
        return false;
    }
    QSqlQuery remove(m_db);
    remove.prepare(QStringLiteral("DELETE FROM mapping_presets WHERE id = :id"));
    remove.bindValue(QStringLiteral(":id"), presetId);
    if (!remove.exec() || remove.numRowsAffected() != 1) {
        qWarning() << "DB: deleteMappingPreset failed:" << remove.lastError().text();
        return false;
    }
    if (!transaction.commit()) {
        qWarning() << "DB: deleteMappingPreset commit failed:" << m_db.lastError().text();
        return false;
    }
    return true;
}

bool CaptureDatabase::deleteMappingPresetAndReassign(const QString& presetId,
                                                     const QString& keepPresetId)
{
    if (presetId.isEmpty() || keepPresetId.isEmpty() || presetId == keepPresetId)
        return false;
    // Neither side may be the reserved artifact: it is never a delete victim
    // and never the "reassign onto" leftover of a delete (cpo-p06).
    if (isBuiltinMappingPresetId(presetId) || isBuiltinMappingPresetId(keepPresetId))
        return false;
    const MappingPreset victim = mappingPreset(presetId);
    const MappingPreset keep = mappingPreset(keepPresetId);
    if (victim.id.isEmpty() || keep.id.isEmpty() || victim.deviceGroup != keep.deviceGroup)
        return false;

    SqlTransaction transaction(m_db);
    if (!transaction.begin()) {
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
        return false;
    }
    QSqlQuery remove(m_db);
    remove.prepare(QStringLiteral("DELETE FROM mapping_presets WHERE id = :id"));
    remove.bindValue(QStringLiteral(":id"), presetId);
    if (!remove.exec() || remove.numRowsAffected() != 1) {
        qWarning() << "DB: deleteMappingPresetAndReassign failed:" << remove.lastError().text();
        return false;
    }
    if (!transaction.commit()) {
        qWarning() << "DB: mapping preset transaction commit failed:" << m_db.lastError().text();
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
    // Game lookups accept any equivalent spelling of the executable path and
    // match the canonical row the write path stored (see setMappingAssignment).
    const QString key =
        targetKind == QLatin1String("game") ? GameIdentity::executableKey(targetKey) : targetKey;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT id, device_group, preset_id, target_kind, target_key, game_row_id "
        "FROM mapping_assignments "
        "WHERE device_group = :group AND target_kind = :kind AND target_key = IFNULL(:key, '')"));
    q.bindValue(QStringLiteral(":group"), deviceGroup);
    q.bindValue(QStringLiteral(":kind"), targetKind);
    q.bindValue(QStringLiteral(":key"), key);
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

bool CaptureDatabase::validateMappingAssignmentTarget(const QString& deviceGroup,
                                                      const QString& targetKind,
                                                      const QString& targetKey,
                                                      QString* canonicalKey, QString* error) const
{
    // The one target rule every assignment writer shares (cpo-p06): the public
    // set path and the atomic create/ensure paths can never disagree about what
    // a valid target is. Game keys are canonicalized exactly once, here.
    const auto reject = [error](const QString& reason) {
        if (error)
            *error = reason;
        return false;
    };
    if (!isMappingDeviceGroup(deviceGroup))
        return reject(QStringLiteral("unknown device group"));
    if (!isMappingTargetKind(targetKind))
        return reject(QStringLiteral("unknown target kind"));
    const QString key =
        targetKind == QLatin1String("game") ? GameIdentity::executableKey(targetKey) : targetKey;
    if (targetKind == QLatin1String("group_default")) {
        if (!key.isEmpty())
            return reject(QStringLiteral("group_default takes no key"));
    } else if (key.isEmpty()) {
        return reject(QStringLiteral("empty target key"));
    }
    if ((targetKind == QLatin1String("controller") || targetKind == QLatin1String("legacy_slot"))
        && deviceGroup != QLatin1String("controller")) {
        return reject(QStringLiteral("controller targets only exist in the controller group"));
    }
    if (canonicalKey)
        *canonicalKey = key;
    return true;
}

bool CaptureDatabase::setMappingAssignment(const QString& deviceGroup, const QString& targetKind,
                                           const QString& targetKey, const QString& presetId,
                                           int gameRowId)
{
    if (!isMappingDeviceGroup(deviceGroup) || !isMappingTargetKind(targetKind))
        return false;
    // Game targets are persisted in the one canonical executable form
    // (GameIdentity::executableKey), whatever spelling the caller supplies: a
    // writer can never create two canonically equivalent game rows, and the
    // resolver's lookup runs through the same boundary. The text is still never
    // classified here - only `game` targets are normalized; `controller-…`,
    // `xinput.slotN` and provider keys stay opaque.
    QString key;
    if (!validateMappingAssignmentTarget(deviceGroup, targetKind, targetKey, &key, nullptr))
        return false;
    const MappingPreset preset = mappingPreset(presetId);
    if (preset.id.isEmpty() || preset.deviceGroup != deviceGroup)
        return false;

    SqlTransaction transaction(m_db);
    if (!transaction.begin()) {
        qWarning() << "DB: could not open mapping assignment transaction:"
                   << m_db.lastError().text();
        return false;
    }
    if (!upsertMappingAssignmentRow(deviceGroup, targetKind, key, presetId, gameRowId)) {
        qWarning() << "DB: setMappingAssignment failed for target" << targetKind << key;
        return false;
    }
    if (!transaction.commit()) {
        qWarning() << "DB: mapping assignment transaction commit failed:"
                   << m_db.lastError().text();
        return false;
    }
    return true;
}

bool CaptureDatabase::clearMappingAssignment(const QString& deviceGroup, const QString& targetKind,
                                             const QString& targetKey)
{
    // Same canonical boundary as the game write path: any equivalent spelling
    // clears the one canonical game row.
    const QString key =
        targetKind == QLatin1String("game") ? GameIdentity::executableKey(targetKey) : targetKey;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "DELETE FROM mapping_assignments "
        "WHERE device_group = :group AND target_kind = :kind AND target_key = IFNULL(:key, '')"));
    q.bindValue(QStringLiteral(":group"), deviceGroup);
    q.bindValue(QStringLiteral(":kind"), targetKind);
    q.bindValue(QStringLiteral(":key"), key);
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

    SqlTransaction transaction(m_db);
    if (!transaction.begin()) {
        qWarning() << "DB: could not open mapping promotion transaction:"
                   << m_db.lastError().text();
        return false;
    }
    // Half one: the durable assignment the runtime identity proof authorizes.
    if (!upsertMappingAssignmentRow(deviceGroup, QStringLiteral("controller"), controllerKey,
                                    source.convertedPresetId, -1)) {
        qWarning() << "DB: promoteMappingPresetSource assignment failed:" << sourceKey;
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
        return false;
    }
    if (!transaction.commit()) {
        qWarning() << "DB: mapping promotion transaction commit failed:"
                   << m_db.lastError().text();
        return false;
    }
    return true;
}
