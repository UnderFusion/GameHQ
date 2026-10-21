// Legacy input binding store of CaptureDatabase (bindings, binding_overrides).
#include "storage/CaptureDatabase.h"
#include "input/BindingPattern.h"
#include "storage/SqlTransaction.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <QDebug>

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
    SqlTransaction transaction(m_db);
    if (!transaction.begin()) {
        qWarning() << "DB: could not open binding override transaction:"
                   << m_db.lastError().text();
        return false;
    }
    for (const BindingOverrideRow& row : rows) {
        if (upsertBindingOverride(row))
            continue;
        return false;
    }
    if (!transaction.commit()) {
        qWarning() << "DB: binding override transaction commit failed:"
                   << m_db.lastError().text();
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
