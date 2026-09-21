#include "input/MappingPresetModel.h"

#include "input/ActionCatalog.h"
#include "localization/NativeText.h"

#include <QHash>
#include <QSet>
#include <utility>

namespace {
// Translation inventory for cpo-p06 (semantic pass and 16-locale acceptance
// stay with cpo-x02; every id below has its source string and a translator
// comment so the extraction pipeline can pick it up).
QString noticeWriteFailed()
{
    //: Shown when a preset write failed and nothing was changed.
    //% "That change could not be saved. Nothing was modified."
    return NativeText::get(QT_TRID_NOOP("gamehq.settings.presets.notice.write_failed"),
                           "That change could not be saved. Nothing was modified.");
}
QString noticeNameTaken()
{
    //: Shown when the entered preset name is already used in this device group.
    //% "That name is already used in this device group."
    return NativeText::get(QT_TRID_NOOP("gamehq.settings.presets.notice.name_taken"),
                           "That name is already used in this device group.");
}
QString noticeNothingSelected()
{
    //: Shown when a preset action was attempted with no preset selected.
    //% "Select a preset first."
    return NativeText::get(QT_TRID_NOOP("gamehq.settings.presets.notice.nothing_selected"),
                           "Select a preset first.");
}
QString noticePendingEdit()
{
    //: Shown while the binding editor holds an unsaved draft or an active capture.
    //% "Finish or cancel the edit in progress first."
    return NativeText::get(QT_TRID_NOOP("gamehq.settings.presets.notice.pending_edit"),
                           "Finish or cancel the edit in progress first.");
}
QString noticeTargetUnavailable()
{
    //: Shown when the editing target has no stable controller identity to save under.
    //% "This controller has no stable identity yet, so a per-controller preset cannot be saved."
    return NativeText::get(QT_TRID_NOOP("gamehq.settings.presets.notice.target_unavailable"),
                           "This controller has no stable identity yet, so a per-controller "
                           "preset cannot be saved.");
}
QString noticeDeleteInUse()
{
    //: Shown when a referenced preset is deleted without choosing where its users go.
    //% "This preset is still in use. Pick a preset to move its users to, then delete it."
    return NativeText::get(QT_TRID_NOOP("gamehq.settings.presets.notice.delete_in_use"),
                           "This preset is still in use. Pick a preset to move its users to, "
                           "then delete it.");
}
QString noticeDeleteFailed()
{
    //: Shown when a delete failed and nothing changed.
    //% "That preset could not be deleted. Nothing was changed."
    return NativeText::get(QT_TRID_NOOP("gamehq.settings.presets.notice.delete_failed"),
                           "That preset could not be deleted. Nothing was changed.");
}
QString noticeRenameFailed()
{
    //: Shown when a rename failed; the previous name stays active.
    //% "That preset could not be renamed. The previous name is still active."
    return NativeText::get(QT_TRID_NOOP("gamehq.settings.presets.notice.rename_failed"),
                           "That preset could not be renamed. The previous name is still active.");
}
QString noticeUnknownPreset()
{
    //: Shown when a preset id does not exist in the current device group.
    //% "That preset is not available in this device group."
    return NativeText::get(QT_TRID_NOOP("gamehq.settings.presets.notice.unknown_preset"),
                           "That preset is not available in this device group.");
}
QString noticeBuiltinReadOnly()
{
    //: Shown when the reserved Built-in defaults entry is targeted by rename/delete.
    //% "Built-in defaults cannot be renamed or deleted."
    return NativeText::get(QT_TRID_NOOP("gamehq.settings.presets.notice.builtin_read_only"),
                           "Built-in defaults cannot be renamed or deleted.");
}
QString noticeGameUnavailable()
{
    //: Shown when a game assignment was requested while no game is in session.
    //% "Start a game first: a game assignment needs to know which game it is for."
    return NativeText::get(QT_TRID_NOOP("gamehq.settings.presets.notice.game_unavailable"),
                           "Start a game first: a game assignment needs to know which game "
                           "it is for.");
}
QString noticeEditInvalid()
{
    //: Shown when an edited binding cannot be stored as preset content.
    //% "That assignment cannot be stored as a preset row."
    return NativeText::get(QT_TRID_NOOP("gamehq.settings.presets.notice.edit_invalid"),
                           "That assignment cannot be stored as a preset row.");
}
QString noticeInvalidName()
{
    //: Shown when a new preset name is empty or reserved.
    //% "Enter a name for the new preset."
    return NativeText::get(QT_TRID_NOOP("gamehq.settings.presets.notice.invalid_name"),
                           "Enter a name for the new preset.");
}
QString builtinChoiceLabel()
{
    //: The virtual "Built-in defaults" picker entry; localized UI text, never a stored name.
    //% "Built-in defaults"
    return NativeText::get(QT_TRID_NOOP("gamehq.settings.presets.builtin_label"),
                           "Built-in defaults");
}
} // namespace

MappingPresetModel::MappingPresetModel(CaptureDatabase* database, QObject* parent)
    : QObject(parent)
    , m_database(database)
{
    rebuildPresets();
    rebuildAssignment();
    rebuildUses();
}

MappingPresetModel::Target MappingPresetModel::targetFromProvenance(
    const QString& deviceGroup, const QString& pinnedProfile, const Provenance& provenance)
{
    Target target;
    if (deviceGroup != QLatin1String("controller")) {
        // Keyboard and mouse have exactly one route per device: their group
        // default IS the editing scope (the group-wide layer the legacy editor
        // wrote into).
        target.kind = QStringLiteral("group_default");
        target.label = deviceGroup == QLatin1String("keyboard") ? QStringLiteral("Keyboard")
                                                                : QStringLiteral("Mouse");
        return target;
    }
    if (pinnedProfile.isEmpty()) {
        target.kind = QStringLiteral("group_default");
        target.label = QStringLiteral("Controller");
        return target;
    }
    target.profile = pinnedProfile;
    if (!provenance.known) {
        target.reason = QStringLiteral("unknown_controller");
        return target;
    }
    target.label = provenance.displayName.isEmpty() ? pinnedProfile : provenance.displayName;
    if (provenance.durable) {
        target.kind = QStringLiteral("controller");
        target.key = pinnedProfile;
        return target;
    }
    // Weak or session-local identity: the logical id itself is NEVER persisted
    // just because it was renamed to a legacy-looking kind. Only a real
    // persistable slot key (the same shape the v9 migration classifies as
    // slot-scoped) may carry a durable write.
    for (const QString& alias : provenance.slotAliases) {
        if (CaptureDatabase::isLegacySlotProfileKey(alias)) {
            target.kind = QStringLiteral("legacy_slot");
            target.key = alias;
            return target;
        }
    }
    target.reason = QStringLiteral("weak_identity");
    return target;
}

void MappingPresetModel::setRuntimeRefresh(std::function<void()> refresh)
{
    m_refreshRuntime = std::move(refresh);
}

void MappingPresetModel::setEffectiveTableProvider(
    std::function<QVector<BindingResolver::Binding>(const QString&, const QString&)> provider)
{
    m_effectiveTable = std::move(provider);
}

void MappingPresetModel::setTargetProvider(TargetProvider provider)
{
    m_targetProvider = std::move(provider);
    emit targetChanged();
    rebuildAssignment();
}

void MappingPresetModel::setPinnedProfileProvider(PinProvider provider)
{
    m_pinProvider = std::move(provider);
}

void MappingPresetModel::setGameTargetProvider(GameTargetProvider provider)
{
    m_gameTargetProvider = std::move(provider);
    rebuildGameAssignment();
}

MappingPresetModel::GameTarget MappingPresetModel::gameTarget() const
{
    if (m_gameTargetProvider) {
        const GameTarget target = m_gameTargetProvider();
        if (!target.key.isEmpty())
            return target;
    }
    return GameTarget();
}

bool MappingPresetModel::gameAvailable() const
{
    return !m_gameKey.isEmpty();
}

void MappingPresetModel::refreshGameTarget()
{
    rebuildGameAssignment();
}

void MappingPresetModel::rebuildGameAssignment()
{
    const GameTarget target = gameTarget();
    const bool targetChanged = target.key != m_gameKey || target.label != m_gameLabel
        || target.rowId != m_gameRowId;
    m_gameKey = target.key;
    m_gameLabel = target.label;
    m_gameRowId = target.rowId;

    QString id;
    QString name;
    bool missing = false;
    bool wrongGroup = false;
    if (!m_gameKey.isEmpty()) {
        const MappingAssignment assignment =
            m_database->mappingAssignment(m_deviceGroup, QStringLiteral("game"), m_gameKey);
        if (!assignment.presetId.isEmpty()) {
            if (CaptureDatabase::isBuiltinMappingPresetId(assignment.presetId)) {
                id = builtinChoiceToken();
            } else {
                const MappingPreset preset = m_database->mappingPreset(assignment.presetId);
                if (preset.id.isEmpty()) {
                    // The resolver steps down and reports this once; Settings must
                    // say it out loud instead of showing a silent fallback.
                    missing = true;
                    id = assignment.presetId;
                } else if (preset.deviceGroup != m_deviceGroup) {
                    wrongGroup = true;
                    id = assignment.presetId;
                } else {
                    id = assignment.presetId;
                    name = preset.name;
                }
            }
        }
    }
    const bool changed = id != m_gameAssignedPresetId || name != m_gameAssignedPresetName
        || missing != m_gameAssignedMissing || wrongGroup != m_gameAssignedWrongGroup;
    m_gameAssignedPresetId = id;
    m_gameAssignedPresetName = name;
    m_gameAssignedMissing = missing;
    m_gameAssignedWrongGroup = wrongGroup;
    if (targetChanged)
        emit gameTargetChanged();
    if (changed)
        emit gameAssignmentChanged();
}

void MappingPresetModel::setPendingEditProvider(std::function<bool()> provider)
{
    m_pendingEditProvider = std::move(provider);
    emit pendingEditChanged();
}

MappingPresetModel::ContentEditSink MappingPresetModel::contentEditSink()
{
    // The editor's whole integration surface: a batch of (action, slot) changes
    // that must land together. The preset decision - which preset owns this
    // target, or adopt-and-mask - stays here, never in the editor.
    return [this](const QVector<ContentEdit>& edits) { return applyContentEdits(edits); };
}

void MappingPresetModel::setDeviceGroup(const QString& group)
{
    if (group != QLatin1String("controller") && group != QLatin1String("keyboard")
        && group != QLatin1String("mouse"))
        return;
    if (m_deviceGroup == group)
        return;
    m_deviceGroup = group;
    m_selectedPresetId.clear();
    emit deviceGroupChanged();
    refresh();
}

void MappingPresetModel::refresh()
{
    rebuildPresets();
    rebuildAssignment();
    rebuildGameAssignment();
    rebuildUses();
}

QString MappingPresetModel::selectedPresetName() const
{
    if (selectedIsBuiltin())
        return builtinChoiceLabel();
    if (m_selectedPresetId.isEmpty())
        return QString();
    const MappingPreset preset = m_database->mappingPreset(m_selectedPresetId);
    return preset.name;
}

bool MappingPresetModel::selectedIsBuiltin() const
{
    return CaptureDatabase::isBuiltinMappingPresetId(m_selectedPresetId);
}

bool MappingPresetModel::selectedEditable() const
{
    if (m_selectedPresetId.isEmpty() || selectedIsBuiltin())
        return false;
    const MappingPreset preset = m_database->mappingPreset(m_selectedPresetId);
    return !preset.id.isEmpty() && preset.deviceGroup == m_deviceGroup;
}

MappingPresetModel::Target MappingPresetModel::target() const
{
    const QString profile = pinnedProfile();
    if (m_targetProvider)
        return m_targetProvider(m_deviceGroup, profile);
    // Conservative default when no engine is wired (tests, headless tools):
    // shared editing only. Controller-specific targets are never guessed here.
    Target fallback;
    fallback.kind = QStringLiteral("group_default");
    fallback.label = m_deviceGroup;
    return fallback;
}

QString MappingPresetModel::assignedPresetId() const
{
    return m_assignedPresetId;
}

QString MappingPresetModel::assignedPresetName() const
{
    return m_assignedPresetName;
}

bool MappingPresetModel::assignedToFallback() const
{
    return m_assignedPresetId.isEmpty();
}

bool MappingPresetModel::assignedToBuiltin() const
{
    return m_assignedPresetId == builtinChoiceToken();
}

bool MappingPresetModel::assignedPresetMissing() const
{
    return m_assignedMissing;
}

bool MappingPresetModel::assignedPresetWrongGroup() const
{
    return m_assignedWrongGroup;
}

bool MappingPresetModel::selectedShared() const
{
    if (m_selectedPresetId.isEmpty() || selectedIsBuiltin())
        return false;
    const int targets = m_uses.controllerUses + m_uses.groupDefaultUses + m_uses.gameUses;
    return targets > 1 || m_uses.migrationUses > 0;
}

bool MappingPresetModel::guarded()
{
    if (!pendingEdit())
        return false;
    setNotice(QStringLiteral("pending_edit"), noticePendingEdit());
    return true;
}

bool MappingPresetModel::setNotice(const QString& kind, const QString& text)
{
    m_noticeKind = kind;
    m_notice = text;
    emit noticeChanged();
    return false;
}

void MappingPresetModel::clearNotice()
{
    if (m_notice.isEmpty() && m_noticeKind.isEmpty())
        return;
    m_notice.clear();
    m_noticeKind.clear();
    emit noticeChanged();
}

void MappingPresetModel::rebuildPresets()
{
    QVariantList list;
    QSet<QString> known;
    for (const MappingPreset& preset : m_database->listMappingPresets(m_deviceGroup)) {
        // The reserved Built-in artifact is never an ordinary library row: the
        // user meets it only as the virtual "Built-in defaults" choice.
        if (CaptureDatabase::isBuiltinMappingPresetId(preset.id))
            continue;
        QVariantMap entry;
        entry.insert(QStringLiteral("id"), preset.id);
        entry.insert(QStringLiteral("name"), preset.name);
        entry.insert(QStringLiteral("origin"), preset.origin);
        list.append(entry);
        known.insert(preset.id);
    }
    m_presetList = list;

    // The selection only survives while its preset still exists in this group.
    if (!m_selectedPresetId.isEmpty() && !CaptureDatabase::isBuiltinMappingPresetId(m_selectedPresetId)
        && !known.contains(m_selectedPresetId)) {
        m_selectedPresetId.clear();
    }
    if (m_selectedPresetId.isEmpty() && !known.isEmpty())
        m_selectedPresetId = list.first().toMap().value(QStringLiteral("id")).toString();
    emit presetsChanged();
    emit selectionChanged();
}

void MappingPresetModel::rebuildAssignment()
{
    QString id;
    QString name;
    bool missing = false;
    bool wrongGroup = false;
    const Target t = target();
    if (!t.kind.isEmpty()) {
        const MappingAssignment assignment =
            m_database->mappingAssignment(m_deviceGroup, t.kind, t.key);
        if (!assignment.presetId.isEmpty()) {
            if (CaptureDatabase::isBuiltinMappingPresetId(assignment.presetId)) {
                id = builtinChoiceToken();
            } else {
                const MappingPreset preset = m_database->mappingPreset(assignment.presetId);
                if (preset.id.isEmpty()) {
                    // The resolver steps down and reports this once; Settings
                    // must say it out loud instead of showing a silent fallback.
                    missing = true;
                    id = assignment.presetId;
                } else if (preset.deviceGroup != m_deviceGroup) {
                    wrongGroup = true;
                    id = assignment.presetId;
                } else {
                    id = assignment.presetId;
                    name = preset.name;
                }
            }
        }
    }
    const bool changed = id != m_assignedPresetId || name != m_assignedPresetName
        || missing != m_assignedMissing || wrongGroup != m_assignedWrongGroup;
    m_assignedPresetId = id;
    m_assignedPresetName = name;
    m_assignedMissing = missing;
    m_assignedWrongGroup = wrongGroup;
    if (changed)
        emit assignmentChanged();
}

void MappingPresetModel::rebuildUses()
{
    Uses uses;
    if (!m_selectedPresetId.isEmpty()) {
        for (const MappingAssignment& assignment : m_database->listMappingAssignments()) {
            if (assignment.presetId != m_selectedPresetId)
                continue;
            if (assignment.targetKind == QLatin1String("group_default"))
                ++uses.groupDefaultUses;
            else if (assignment.targetKind == QLatin1String("game"))
                ++uses.gameUses;
            else
                ++uses.controllerUses;   // controller | legacy_slot
        }
        for (const MappingPresetSource& source : m_database->listMappingPresetSources()) {
            if (source.convertedPresetId == m_selectedPresetId)
                ++uses.migrationUses;
        }
    }
    const bool changed = uses.controllerUses != m_uses.controllerUses
        || uses.groupDefaultUses != m_uses.groupDefaultUses || uses.gameUses != m_uses.gameUses
        || uses.migrationUses != m_uses.migrationUses;
    m_uses = uses;
    if (changed) {
        emit usesChanged();
        emit selectionChanged();
    }
}

void MappingPresetModel::notifyRuntimeChange()
{
    // The ONE runtime seam for preset mutations: assignment/content/delete can
    // change an effective table, so the engine re-resolves the winners and
    // switches them at a safe input boundary (cpo-p05). Library-only changes
    // (create, duplicate-unused, rename) never call this.
    if (m_refreshRuntime)
        m_refreshRuntime();
}

bool MappingPresetModel::selectPreset(const QString& presetId)
{
    if (presetId == m_selectedPresetId)
        return true;
    if (guarded())
        return false;
    if (presetId.isEmpty() || CaptureDatabase::isBuiltinMappingPresetId(presetId)) {
        m_selectedPresetId = presetId;
        emit selectionChanged();
        rebuildUses();
        return true;
    }
    const MappingPreset preset = m_database->mappingPreset(presetId);
    if (preset.id.isEmpty() || preset.deviceGroup != m_deviceGroup)
        return setNotice(QStringLiteral("unknown_preset"),
                         noticeUnknownPreset());
    m_selectedPresetId = presetId;
    emit selectionChanged();
    rebuildUses();
    return true;
}

bool MappingPresetModel::createPreset(const QString& name)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || CaptureDatabase::isReservedMappingPresetName(trimmed)
        || trimmed == builtinChoiceToken()) {
        return setNotice(QStringLiteral("invalid_name"),
                         noticeInvalidName());
    }
    const QString id = m_database->createMappingPreset(m_deviceGroup, trimmed);
    if (id.isEmpty()) {
        // Duplicate names, storage failures: nothing was written either way, so
        // the list and the editor keep showing exactly what they showed before.
        return setNotice(QStringLiteral("write_failed"),
                         noticeNameTaken());
    }
    rebuildPresets();
    rebuildAssignment();
    // Library-only: an unused preset cannot change any effective table, so this
    // must not invalidate an in-flight gesture.
    return true;
}

bool MappingPresetModel::duplicateSelected(const QString& name)
{
    const QString source = m_selectedPresetId;
    if (source.isEmpty() || selectedIsBuiltin() || !selectedEditable())
        return setNotice(QStringLiteral("nothing_selected"),
                         noticeNothingSelected());
    const QString trimmed = name.trimmed();
    const QString id = m_database->createMappingPreset(
        m_deviceGroup, trimmed.isEmpty() ? uniquePresetName(QStringLiteral("Copy")) : trimmed,
        m_database->mappingPresetRows(source), QStringLiteral("user"));
    if (id.isEmpty())
        return setNotice(QStringLiteral("write_failed"),
                         noticeNameTaken());
    m_selectedPresetId = id;
    rebuildPresets();
    rebuildAssignment();
    emit selectionChanged();
    return true;
}

bool MappingPresetModel::renameSelected(const QString& name)
{
    const QString id = m_selectedPresetId;
    if (id.isEmpty() || !selectedEditable())
        return setNotice(QStringLiteral("nothing_selected"),
                         noticeNothingSelected());
    if (CaptureDatabase::isBuiltinMappingPresetId(id))
        return setNotice(QStringLiteral("builtin_read_only"),
                         noticeBuiltinReadOnly());
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()
        || !m_database->renameMappingPreset(id, trimmed)) {
        return setNotice(QStringLiteral("rename_failed"),
                         noticeRenameFailed());
    }
    // Rename is display-only: the id is stable, so every assignment and every
    // effective table is untouched - no runtime refresh, and an open draft
    // keeps its meaning.
    rebuildPresets();
    rebuildAssignment();
    return true;
}

bool MappingPresetModel::deleteSelected(const QString& reassignToId)
{
    const QString id = m_selectedPresetId;
    if (id.isEmpty())
        return setNotice(QStringLiteral("nothing_selected"),
                         noticeNothingSelected());
    if (CaptureDatabase::isBuiltinMappingPresetId(id))
        return setNotice(QStringLiteral("builtin_read_only"),
                         noticeBuiltinReadOnly());
    if (guarded())
        return false;

    const int references = m_database->mappingPresetReferenceCount(id);
    bool ok = false;
    if (references > 0) {
        if (reassignToId.isEmpty()) {
            return setNotice(
                QStringLiteral("delete_in_use"),
                noticeDeleteInUse()
                    );
        }
        ok = m_database->deleteMappingPresetAndReassign(id, reassignToId);
    } else {
        ok = m_database->deleteMappingPreset(id);
    }
    if (!ok) {
        return setNotice(QStringLiteral("delete_failed"),
                         noticeDeleteFailed());
    }
    m_selectedPresetId.clear();
    const bool movedReferences = references > 0;
    refresh();
    if (movedReferences) {
        // Whoever pointed at the deleted preset now points somewhere else (or
        // at built-in defaults), so the winners must be re-resolved - but an
        // unused preset never invalidates a running gesture.
        notifyRuntimeChange();
    }
    return true;
}

bool MappingPresetModel::applyAssignment(const QString& presetId)
{
    const Target t = target();
    if (t.kind.isEmpty()) {
        return setNotice(
            QStringLiteral("target_unavailable"),
            noticeTargetUnavailable());
    }
    if (guarded())
        return false;

    bool ok = false;
    if (presetId.isEmpty()) {
        // "Remove assignment / follow fallback": the target goes back to the
        // precedence chain and may reveal retained local rows.
        ok = m_database->clearMappingAssignment(m_deviceGroup, t.kind, t.key);
    } else if (presetId == builtinChoiceToken()) {
        // "Built-in defaults": the explicit empty winner that masks every
        // compatibility layer below it. The reserved preset is created lazily
        // inside the same transaction and reused forever after.
        ok = !m_database->assignMappingTargetToBuiltin(m_deviceGroup, t.kind, t.key).isEmpty();
    } else {
        const MappingPreset preset = m_database->mappingPreset(presetId);
        if (preset.id.isEmpty() || preset.deviceGroup != m_deviceGroup)
            return setNotice(QStringLiteral("unknown_preset"),
                             noticeUnknownPreset());
        ok = m_database->setMappingAssignment(m_deviceGroup, t.kind, t.key, presetId);
    }
    if (!ok) {
        return setNotice(QStringLiteral("write_failed"),
                         noticeWriteFailed());
    }
    if (!presetId.isEmpty() && presetId != builtinChoiceToken())
        m_selectedPresetId = presetId;
    refresh();
    notifyRuntimeChange();
    return true;
}

bool MappingPresetModel::applyGameAssignment(const QString& presetId)
{
    if (m_gameKey.isEmpty()) {
        return setNotice(QStringLiteral("game_unavailable"), noticeGameUnavailable());
    }
    if (guarded())
        return false;

    // The session can have moved on between the click and the write; the row is
    // written for the game the model currently shows, and the key is the
    // canonical one storage normalizes with.
    const GameTarget t = gameTarget();
    if (t.key.isEmpty())
        return setNotice(QStringLiteral("game_unavailable"), noticeGameUnavailable());

    bool ok = false;
    if (presetId.isEmpty()) {
        // "Follow the chain below the game": drop the game row and let the next
        // winner serve.
        ok = m_database->clearMappingAssignment(m_deviceGroup, QStringLiteral("game"), t.key);
    } else if (presetId == builtinChoiceToken()) {
        // "Built-in defaults for this game": the explicit empty winner, created
        // lazily inside the same transaction as the assignment.
        ok = !m_database->assignMappingTargetToBuiltin(m_deviceGroup, QStringLiteral("game"),
                                                       t.key, t.rowId)
                  .isEmpty();
    } else {
        const MappingPreset preset = m_database->mappingPreset(presetId);
        if (preset.id.isEmpty() || preset.deviceGroup != m_deviceGroup)
            return setNotice(QStringLiteral("unknown_preset"),
                             noticeUnknownPreset());
        ok = m_database->setMappingAssignment(m_deviceGroup, QStringLiteral("game"), t.key,
                                              preset.id, t.rowId);
    }
    if (!ok) {
        return setNotice(QStringLiteral("write_failed"),
                         noticeWriteFailed());
    }
    refresh();
    // A game row is the highest precedence for a running game, so this can change
    // an effective table: re-resolve at a safe input boundary.
    notifyRuntimeChange();
    return true;
}

bool MappingPresetModel::duplicateSelectedForTarget(const QString& name)
{
    const QString source = m_selectedPresetId;
    if (source.isEmpty() || selectedIsBuiltin() || !selectedEditable())
        return setNotice(QStringLiteral("nothing_selected"),
                         noticeNothingSelected());
    const Target t = target();
    if (t.kind.isEmpty()) {
        return setNotice(
            QStringLiteral("target_unavailable"),
            noticeTargetUnavailable());
    }
    if (guarded())
        return false;

    const QString trimmed = name.trimmed();
    const QString id = m_database->createMappingPresetAssigned(
        m_deviceGroup, trimmed.isEmpty() ? uniquePresetName(m_database->mappingPreset(source).name)
                                         : trimmed,
        m_database->mappingPresetRows(source), t.kind, t.key);
    if (id.isEmpty()) {
        return setNotice(QStringLiteral("write_failed"),
                         noticeWriteFailed());
    }
    // Duplicate + assign landed together; the original preset's other users are
    // untouched, and only this target moved.
    m_selectedPresetId = id;
    refresh();
    notifyRuntimeChange();
    return true;
}

bool MappingPresetModel::applyEditToRows(const QString& deviceGroup,
                                         QVector<MappingPresetRow>& rows,
                                         const ContentEdit& edit)
{
    for (int index = 0; index < rows.size(); ++index) {
        if (rows.at(index).actionId == edit.actionId && rows.at(index).slot == edit.slot) {
            rows.removeAt(index);
            break;
        }
    }
    if (edit.remove)
        return true;
    MappingPresetRow edited = edit.row;
    edited.actionId = edit.actionId;
    edited.slot = edit.slot;
    // One canonical row rule: the same validator the runtime binding grammar
    // applies, so a stored preset can never hold a row the binding layer would
    // call malformed (group-aware for key chords vs control ids).
    if (!CaptureDatabase::isValidMappingPresetRow(deviceGroup, edited))
        return false;
    rows.append(edited);
    return true;
}

QVector<MappingPresetRow>
MappingPresetModel::rowsFromTable(const QString& deviceGroup,
                                  const QVector<BindingResolver::Binding>& table)
{
    // The same conversion the cpo-p03 fold uses (BindingResolver::foldChainRows):
    // a row with no trigger is stored as an explicit "unbound" so it keeps
    // shadowing the default it shadowed before. Rows the preset grammar cannot
    // store are skipped, exactly like the fold skips them.
    QVector<MappingPresetRow> rows;
    for (const BindingResolver::Binding& binding : table) {
        const auto* action = ActionCatalog::find(binding.actionId);
        if (!action || !action->bindable)
            continue;
        MappingPresetRow row;
        row.actionId = binding.actionId;
        row.slot = binding.slot;
        row.unbound = binding.unbound || binding.triggerCode.isEmpty();
        row.triggerCode = row.unbound ? QString() : binding.triggerCode;
        row.activation = binding.activation;
        row.holdMs = binding.holdMs;
        row.tapCount = binding.tapCount;
        if (!CaptureDatabase::isValidMappingPresetRow(deviceGroup, row))
            continue;
        rows.append(row);
    }
    return rows;
}

QString MappingPresetModel::uniquePresetName(const QString& base) const
{
    // Deterministic and collision-safe; the same rule the migration uses, so a
    // generated name can never fail on the unique-name index.
    return m_database->uniqueMigrationPresetName(m_deviceGroup, base);
}

bool MappingPresetModel::applyContentEdit(const QString& actionId, int slot,
                                          const MappingPresetRow& row, bool remove)
{
    ContentEdit edit;
    edit.actionId = actionId;
    edit.slot = slot;
    edit.row = row;
    edit.remove = remove;
    return applyContentEdits({edit});
}

bool MappingPresetModel::applyContentEdits(const QVector<ContentEdit>& edits)
{
    if (edits.isEmpty())
        return true;
    const Target t = target();

    // 1) A real user preset already owns this target: the edit lands there, and
    //    everyone else using that preset changes too - which is exactly what
    //    the "used by N" copy and the duplicate-for-this-controller path exist
    //    to make explicit in the UI.
    if (!t.kind.isEmpty()) {
        const MappingAssignment assignment =
            m_database->mappingAssignment(m_deviceGroup, t.kind, t.key);
        const MappingPreset owner = m_database->mappingPreset(assignment.presetId);
        if (!owner.id.isEmpty() && owner.origin == QLatin1String("user")
            && owner.deviceGroup == m_deviceGroup) {
            QVector<MappingPresetRow> rows = m_database->mappingPresetRows(owner.id);
            for (const ContentEdit& edit : edits) {
                if (!applyEditToRows(m_deviceGroup, rows, edit)) {
                    return setNotice(QStringLiteral("edit_invalid"), noticeEditInvalid());
                }
            }
            if (!m_database->replaceMappingPresetRows(owner.id, rows)) {
                return setNotice(QStringLiteral("write_failed"), noticeWriteFailed());
            }
            m_selectedPresetId = owner.id;
            refresh();
            notifyRuntimeChange();
            return true;
        }
    }

    // 2) Adopt-and-mask. Nothing user-owned owns this target yet (it may be
    //    built-in defaults, the legacy chain, a materialized bridge, or a
    //    migration preset whose content belongs to the cpo-p03 proof path).
    //    Snapshot the behavior the target serves TODAY, apply the requested
    //    edit on top, then create + assign in one transaction. Retained legacy
    //    rows are never deleted: they stay dormant recovery evidence.
    if (t.kind.isEmpty())
        return setNotice(
            QStringLiteral("target_unavailable"),
            noticeTargetUnavailable());

    QVector<BindingResolver::Binding> table;
    if (m_effectiveTable)
        table = m_effectiveTable(m_deviceGroup, t.profile);
    if (table.isEmpty()) {
        // No engine wired (tests/headless): the built-in table for this route
        // is the honest snapshot - it is exactly what a `builtin` route serves.
        table = BindingResolver::chainTableFromRows(BindingResolver::defaultBindings(),
                                                    m_deviceGroup, t.profile, {});
    }
    QVector<MappingPresetRow> rows = rowsFromTable(m_deviceGroup, table);
    for (const ContentEdit& edit : edits) {
        if (!applyEditToRows(m_deviceGroup, rows, edit)) {
            return setNotice(QStringLiteral("edit_invalid"), noticeEditInvalid());
        }
    }
    const QString base = t.label.isEmpty() ? QStringLiteral("My mappings")
                                           : QStringLiteral("%1 mappings").arg(t.label);
    const QString id = m_database->createMappingPresetAssigned(
        m_deviceGroup, uniquePresetName(base), rows, t.kind, t.key);
    if (id.isEmpty()) {
        return setNotice(QStringLiteral("write_failed"),
                         noticeWriteFailed());
    }
    m_selectedPresetId = id;
    refresh();
    notifyRuntimeChange();
    emit selectionChanged();
    return true;
}
