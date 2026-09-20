#include "input/MappingAssignmentResolver.h"

#include "core/GameIdentity.h"

#include <QDebug>

namespace {

const QString kControllerGroup = QStringLiteral("controller");

QString staleKey(const QString& deviceGroup, const QString& targetKind, const QString& targetKey,
                 const QString& presetId)
{
    return deviceGroup + QLatin1Char('\x1f') + targetKind + QLatin1Char('\x1f') + targetKey
           + QLatin1Char('\x1f') + presetId;
}

QString ruleLabel(const QString& targetKind, const QString& targetKey)
{
    return targetKey.isEmpty() ? targetKind : targetKind + QLatin1Char(':') + targetKey;
}

} // namespace

MappingAssignmentResolver::MappingAssignmentResolver(CaptureDatabase* database)
    : m_database(database)
{
}

QString MappingAssignmentResolver::canonicalGameKey(const QString& executablePathOrKey)
{
    // One normalization, shared with the storage write path: nothing here may
    // diverge from what rows are stored with.
    return GameIdentity::executableKey(executablePathOrKey);
}

QString MappingAssignmentResolver::usablePresetId(const QString& deviceGroup,
                                                  const MappingAssignment& hit,
                                                  Resolution& resolution)
{
    if (hit.presetId.isEmpty())
        return QString();   // no assignment for this rule: a normal fall-through

    const QString rule = ruleLabel(hit.targetKind, hit.targetKey);
    const MappingPreset preset = m_database->mappingPreset(hit.presetId);

    QString reason;
    QString detail;
    if (preset.id.isEmpty()) {
        reason = QStringLiteral("missing_preset");
        detail = QStringLiteral("%1 -> missing preset %2").arg(rule, hit.presetId);
    } else if (preset.deviceGroup != deviceGroup) {
        reason = QStringLiteral("wrong_group");
        detail = QStringLiteral("%1 -> preset %2 belongs to group %3")
                     .arg(rule, hit.presetId, preset.deviceGroup);
    }
    if (reason.isEmpty())
        return preset.id;

    resolution.steppedDown.append(detail);
    const QString dedup = staleKey(deviceGroup, hit.targetKind, hit.targetKey, hit.presetId);
    if (!m_staleSeen.contains(dedup)) {
        m_staleSeen.insert(dedup);
        m_stale.append({deviceGroup, hit.targetKind, hit.targetKey, hit.presetId, reason});
        qWarning() << "Mapping presets: stepping down," << detail
                   << "- reporting this broken assignment once.";
    }
    return QString();
}

MappingAssignmentResolver::Resolution MappingAssignmentResolver::resolve(
    const QString& deviceGroup, const QVector<IdentityCandidate>& identityChain,
    const QString& gameExecutableKey)
{
    Resolution resolution;
    resolution.source = QStringLiteral("builtin");
    if (!m_database || deviceGroup.isEmpty())
        return resolution;

    const auto tryHit = [&](const QString& targetKind, const QString& targetKey) -> QString {
        return usablePresetId(deviceGroup,
                              m_database->mappingAssignment(deviceGroup, targetKind, targetKey),
                              resolution);
    };

    // 1. Game rule: the top rule while that game runs, per group it names.
    const QString gameKey = canonicalGameKey(gameExecutableKey);
    if (!gameKey.isEmpty()) {
        const QString presetId = tryHit(QStringLiteral("game"), gameKey);
        if (!presetId.isEmpty()) {
            resolution.presetId = presetId;
            resolution.source = QStringLiteral("game");
            resolution.targetKind = QStringLiteral("game");
            resolution.targetKey = gameKey;
            return resolution;
        }
    }

    // 2. Controller rule: the caller's TYPED chain in precedence order (most
    //    specific key first). Each candidate is matched against its declared
    //    kind only - a durable candidate never reads a slot row and a slot
    //    candidate never reads a controller row - so no row can shadow the
    //    other kind's legitimate hit. Keyboard and mouse skip this step: they
    //    have no per-instance identity.
    if (deviceGroup == kControllerGroup) {
        for (const IdentityCandidate& candidate : identityChain) {
            if (candidate.targetKey.isEmpty())
                continue;
            // The typed chain carries controller targets only. Anything else
            // (untyped, session-local, a game key smuggled in) is not a
            // persisted target a pad can match by string in this step.
            if (candidate.targetKind != QLatin1String("controller")
                && candidate.targetKind != QLatin1String("legacy_slot")) {
                continue;
            }
            const QString presetId = tryHit(candidate.targetKind, candidate.targetKey);
            if (!presetId.isEmpty()) {
                resolution.presetId = presetId;
                resolution.source = QStringLiteral("controller");
                resolution.targetKind = candidate.targetKind;
                resolution.targetKey = candidate.targetKey;
                return resolution;
            }
        }
    }

    // 3. Group default.
    {
        const QString presetId = tryHit(QStringLiteral("group_default"), QString());
        if (!presetId.isEmpty()) {
            resolution.presetId = presetId;
            resolution.source = QStringLiteral("group_default");
            resolution.targetKind = QStringLiteral("group_default");
            return resolution;
        }
    }

    // 4. Built-in defaults: nothing assigned, or everything assigned is broken.
    return resolution;
}

QVector<MappingAssignmentResolver::StaleAssignment> MappingAssignmentResolver::staleReports() const
{
    return m_stale;
}

void MappingAssignmentResolver::clearStaleReports()
{
    m_stale.clear();
    m_staleSeen.clear();
}
