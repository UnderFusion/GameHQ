// BindingEditorModel: persistence hooks and the write path (hotkey/row/preset
// sinks, atomic row writes, applying a pending change, clear/reset, conflict
// confirmation and the post-write refresh). Split out of BindingEditorModel.cpp;
// the class itself is declared in BindingEditorModel.h.
#include "input/BindingEditorModel.h"

#include "input/ActionCatalog.h"
#include "input/BindingRelation.h"
#include "input/BindingRuntime.h"
#include "input/ControllerIdentity.h"
#include "localization/NativeText.h"
#include "storage/CaptureDatabase.h"

#include <QHash>
#include <QSet>
#include <QVariantMap>
#include <utility>

void BindingEditorModel::setHotkeyApply(HotkeyApply apply)
{
    m_hotkeyApply = std::move(apply);
}

void BindingEditorModel::setPersistRow(PersistRow persist)
{
    m_persistRow = std::move(persist);
}

void BindingEditorModel::setPresetSink(PresetSink sink)
{
    m_presetSink = std::move(sink);
}

void BindingEditorModel::setEditSourceProvider(EditSourceProvider provider)
{
    m_editSourceProvider = std::move(provider);
}

bool BindingEditorModel::persist(const BindingOverrideRow& row)
{
    if (m_presetSink) {
        // cpo-p06: the write lands in a named preset (adopt-and-mask on the way
        // when nothing user-owned serves this target yet), so normal editing no
        // longer creates binding_overrides rows.
        PresetEdit edit;
        edit.actionId = row.actionId;
        edit.slot = row.slot;
        edit.triggerCode = row.triggerCode;
        edit.activation = row.activation;
        edit.holdMs = row.holdMs;
        edit.unbound = row.unbound;
        edit.tapCount = row.tapCount;
        return m_presetSink({edit});
    }
    return m_persistRow ? m_persistRow(row) : m_database->upsertBindingOverride(row);
}

bool BindingEditorModel::clearRow(const QString& deviceGroup, const QString& deviceProfile,
                                  const QString& actionId, int slot)
{
    if (m_presetSink) {
        PresetEdit edit;
        edit.actionId = actionId;
        edit.slot = slot;
        edit.remove = true;
        return m_presetSink({edit});
    }
    return m_database->clearBindingOverride(deviceGroup, deviceProfile, actionId, slot);
}

bool BindingEditorModel::isGlobalHotkey(const BindingResolver::Binding& binding) const
{
    if (binding.deviceGroup != QLatin1String("keyboard"))
        return false;
    if (binding.activation != QLatin1String("press"))
        return false;
    const auto* action = ActionCatalog::find(binding.actionId);
    return action && action->scope == ActionCatalog::Scope::Global;
}

bool BindingEditorModel::persistRowsAtomically(const QVector<BindingOverrideRow>& rows)
{
    if (m_presetSink) {
        // One batch = one preset transaction: the whole set lands in exactly
        // one preset, or in one adopted preset, or nowhere.
        QVector<PresetEdit> edits;
        edits.reserve(rows.size());
        for (const BindingOverrideRow& row : rows) {
            PresetEdit edit;
            edit.actionId = row.actionId;
            edit.slot = row.slot;
            edit.triggerCode = row.triggerCode;
            edit.activation = row.activation;
            edit.holdMs = row.holdMs;
            edit.unbound = row.unbound;
            edit.tapCount = row.tapCount;
            edits.append(edit);
        }
        return m_presetSink(edits);
    }
    if (!m_persistRow)
        return m_database->upsertBindingOverridesAtomically(rows);

    // The injectable per-row seam is retained for deterministic failure tests.
    // Production uses CaptureDatabase's real SQL transaction above; the seam
    // emulates the same all-or-nothing result and restores exact previous rows.
    const auto keyFor = [](const BindingOverrideRow& row) {
        return row.deviceGroup + QLatin1Char('|') + row.deviceProfile + QLatin1Char('|')
             + row.actionId + QLatin1Char('#') + QString::number(row.slot);
    };
    QHash<QString, BindingOverrideRow> before;
    for (const BindingOverrideRow& row : m_database->listBindingOverrides())
        before.insert(keyFor(row), row);

    for (const BindingOverrideRow& row : rows) {
        if (persist(row))
            continue;
        for (const BindingOverrideRow& affected : rows) {
            const auto previous = before.constFind(keyFor(affected));
            if (previous != before.cend())
                m_database->upsertBindingOverride(previous.value());
            else
                m_database->clearBindingOverride(affected.deviceGroup, affected.deviceProfile,
                                                 affected.actionId, affected.slot);
        }
        return false;
    }
    return true;
}

// Commits the OS registration and the database write together, or leaves both
// exactly as they were. Previously the DB was written first and the hotkey layer
// was reconciled afterwards from InputEngine, whose return value was discarded:
// a chord Windows refused was still saved, so Settings showed a shortcut that
// did nothing until the next restart.
bool BindingEditorModel::applyChange(const PendingChange& change)
{
    const auto& target = change.target;
    const QString profile = selectedProfile();

    if (change.hasConversion) {
        const BindingOverrideRow converted{
            change.conversionTarget.deviceGroup, profile,
            change.conversionTarget.actionId, change.conversionTarget.slot,
            change.conversionTarget.triggerCode, change.conversionTarget.activation,
            change.conversionTarget.holdMs, false, change.conversionTarget.tapCount};
        const BindingOverrideRow requested{
            target.deviceGroup, profile, target.actionId, target.slot,
            target.triggerCode, target.activation, target.holdMs, false, target.tapCount};
        if (!persistRowsAtomically({converted, requested})) {
            setRelationNotice(QStringLiteral("persistence_error"),
                              QStringLiteral("These assignments could not be saved. Both "
                                             "previous assignments are still active."));
            refreshAfterWrite();
            return false;
        }
        refreshAfterWrite();
        return true;
    }

    // A preset that is open but not served changes nothing live, so Windows is
    // neither asked for its chord nor told to drop the live one.
    const bool liveEdit = editTargetIsLive();
    const bool ownsHotkey = liveEdit && m_hotkeyApply && isGlobalHotkey(target);

    // 1) Remember what is live now, so step 4 can put it back.
    QString previousChord;
    if (ownsHotkey) {
        for (const auto& binding : m_runtime->effectiveBindings(target.deviceGroup, profile)) {
            if (binding.actionId == target.actionId && binding.slot == target.slot) {
                previousChord = binding.triggerCode;
                break;
            }
        }
    }

    // 2) Claim the chord from Windows before anything is persisted. On refusal
    //    the previous registration stays live and nothing is written.
    if (ownsHotkey) {
        QString reason;
        if (!m_hotkeyApply(target.actionId, target.slot, target.triggerCode, &reason)) {
            setRelationNotice(
                QStringLiteral("hotkey_unavailable"),
                reason.isEmpty()
                    ? QStringLiteral("This shortcut is already used by Windows or another application.")
                    : reason);
            return false;
        }
    }

    // 3) Persist: the displaced bindings and the requested one are ONE batch.
    //    Both backends are all-or-nothing per batch — a preset batch lands in
    //    exactly one preset transaction (adopt-and-mask included), the legacy
    //    path uses the atomic upsert — so a failure can never leave an earlier
    //    conflict row committed, and no hand-rolled rollback is needed (cpo-p06
    //    review blocker 4: the old loop committed each displaced row in its own
    //    transaction and then tried to undo them through binding_overrides,
    //    which is not a rollback of preset content). A displaced row keeps its
    //    gesture but loses the trigger.
    QVector<BindingOverrideRow> batch;
    batch.reserve(change.conflicts.size() + 1);
    for (const auto& conflict : change.conflicts) {
        batch.append(BindingOverrideRow{conflict.deviceGroup, profile, conflict.actionId,
                                        conflict.slot, {}, conflict.activation, conflict.holdMs,
                                        true, conflict.tapCount});
    }
    batch.append(BindingOverrideRow{target.deviceGroup, profile, target.actionId, target.slot,
                                    target.triggerCode, target.activation, target.holdMs, false,
                                    target.tapCount});
    const bool persisted = persistRowsAtomically(batch);

    // 4) A failed transaction rolls the OS back to the chord that was live
    //    before, so the three views cannot disagree.
    if (!persisted) {
        if (ownsHotkey) {
            QString reason;
            m_hotkeyApply(target.actionId, target.slot, previousChord, &reason);
        }
        setRelationNotice(QStringLiteral("persistence_error"),
                          QStringLiteral("This shortcut could not be saved. The previous "
                                         "assignment is still active."));
        refreshAfterWrite();
        return false;
    }

    // A displaced global hotkey must lose its Win32 registration too. The legacy
    // path gets that from reloadBindings()' sweep; a preset-sink batch never
    // runs that sweep, so release those slots here (an empty chord means
    // "release the slot" — the same contract the rollback path uses).
    if (liveEdit && m_presetSink && m_hotkeyApply) {
        for (const auto& conflict : change.conflicts) {
            if (!isGlobalHotkey(conflict))
                continue;
            QString reason;
            m_hotkeyApply(conflict.actionId, conflict.slot, QString(), &reason);
        }
    }

    // 5) Only a fully committed change refreshes the editor.
    refreshAfterWrite();
    return true;
}

void BindingEditorModel::clearBinding(const QString& actionId, int slot)
{
    const auto* action = ActionCatalog::find(actionId);
    if (!action || !action->bindable)
        return;
    // The unbound row keeps the slot's gesture. Writing press/0 here (as this
    // did before 0.7.3) silently degraded a tap/hold/double_tap slot to press,
    // so the next capture into it collided with every timed gesture sharing
    // the trigger.
    const BindingResolver::Gesture gesture =
        editedGesture(m_deviceGroup, selectedProfile(), actionId, slot);
    BindingOverrideRow row{m_deviceGroup, selectedProfile(), actionId, slot, {},
                           gesture.activation, gesture.holdMs, true, gesture.tapCount};
    persist(row);
    refreshAfterWrite();
}

void BindingEditorModel::resetBinding(const QString& actionId, int slot)
{
    const auto* action = ActionCatalog::find(actionId);
    if (!action || !action->bindable || slot < 1 || slot > 2)
        return;
    clearRow(m_deviceGroup, selectedProfile(), actionId, slot);
    refreshAfterWrite();
}

void BindingEditorModel::resetAction(const QString& actionId)
{
    for (int slot = 1; slot <= 2; ++slot)
        clearRow(m_deviceGroup, selectedProfile(), actionId, slot);
    refreshAfterWrite();
}

void BindingEditorModel::resetCurrentProfile()
{
    // Genuine legacy-row operation: it writes binding_overrides directly, so it
    // keeps the broad reload even in the preset era (the record's preset-era
    // semantics are an open decision, see docs/mapping-presets.md).
    m_database->clearBindingOverridesForProfile(m_deviceGroup, selectedProfile());
    reloadAndRefresh();
}

void BindingEditorModel::resetAllBindings()
{
    m_database->clearAllBindingOverrides();
    reloadAndRefresh();
}

void BindingEditorModel::confirmConflict()
{
    if (!m_conflictPending)
        return;
    const PendingChange change = m_pending;
    dismissConflict();
    applyChange(change);
}

void BindingEditorModel::dismissConflict()
{
    if (!m_conflictPending)
        return;
    m_conflictPending = false;
    m_conflictMessage.clear();
    m_pending = {};
    emit conflictChanged();
}

void BindingEditorModel::reloadAndRefresh()
{
    if (m_reloadRuntime)
        m_reloadRuntime();
    rebuildRows();
}

void BindingEditorModel::refreshAfterWrite()
{
    if (m_presetSink) {
        // The sink's transaction published the change and asked the engine for
        // the route-scoped cpo-p05 switch. Nothing here reads binding_overrides,
        // so the editor only rebuilds its view (review blocker 3).
        rebuildRows();
        return;
    }
    reloadAndRefresh();
}
