#include "input/BindingEditorModel.h"

#include "input/ActionCatalog.h"
#include "input/BindingRelation.h"
#include "input/BindingRuntime.h"
#include "input/ControllerIdentity.h"
#include "storage/CaptureDatabase.h"

#include <QHash>
#include <QSet>
#include <QVariantMap>
#include <utility>

namespace {
QString slotKey(const QString& actionId, int slot)
{
    return actionId + QLatin1Char('#') + QString::number(slot);
}

// A new binding can relate to several existing ones at once. effectiveBindings()
// comes out of a QHash, so "first match wins" would show a different notice on
// different runs. Rank the non-blocking tiers by how much the user needs to
// know: a context override changes what the button does, a redundant binding
// means their edit did nothing, and a shared gesture is merely informational.
int noticeSeverity(BindingRelation::Kind kind)
{
    switch (kind) {
    case BindingRelation::Kind::ContextOverride: return 3;
    case BindingRelation::Kind::Redundant:       return 2;
    case BindingRelation::Kind::SharedGesture:   return 1;
    case BindingRelation::Kind::ConversionRequired:     // owns its compatibility modal
    case BindingRelation::Kind::HardConflict:            // owns the modal instead
    case BindingRelation::Kind::None:            return 0;
    }
    return 0;
}

bool sameBindingValue(const BindingResolver::Binding& left,
                      const BindingResolver::Binding& right)
{
    return left.triggerCode == right.triggerCode
        && left.activation == right.activation
        && left.holdMs == right.holdMs
        && left.tapCount == right.tapCount;
}

QString slotChangeState(const BindingResolver::Binding* baseline,
                        const BindingResolver::Binding* effective,
                        const BindingOverrideRow* local)
{
    if (!local)
        return QStringLiteral("default");
    const bool hadBaseline = baseline && !baseline->triggerCode.isEmpty();
    const bool isAssigned = effective && !effective->triggerCode.isEmpty();
    if (hadBaseline && (local->unbound || !isAssigned))
        return QStringLiteral("removed");
    if (!hadBaseline && isAssigned)
        return QStringLiteral("added");
    if (hadBaseline && isAssigned && !sameBindingValue(*baseline, *effective))
        return QStringLiteral("modified");
    return QStringLiteral("default");
}

QString statusLabel(const QString& state)
{
    if (state == QLatin1String("added")) return QStringLiteral("Added");
    if (state == QLatin1String("modified")) return QStringLiteral("Modified");
    if (state == QLatin1String("removed")) return QStringLiteral("Removed");
    return {};
}
}

BindingEditorModel::BindingEditorModel(CaptureDatabase* database, BindingRuntime* runtime,
                                       std::function<void()> reloadRuntime, QObject* parent)
    : QObject(parent)
    , m_database(database)
    , m_runtime(runtime)
    , m_reloadRuntime(std::move(reloadRuntime))
{
    rebuildRows();
}

void BindingEditorModel::setDeviceGroup(const QString& group)
{
    if (group != QLatin1String("controller") && group != QLatin1String("keyboard")
        && group != QLatin1String("mouse"))
        return;
    if (m_deviceGroup == group)
        return;
    cancelCapture();
    m_deviceGroup = group;
    emit deviceGroupChanged();
    rebuildRows();
}

void BindingEditorModel::setControllerSpecific(bool specific)
{
    specific = specific && controllerSpecificAvailable();
    if (m_controllerSpecific == specific)
        return;
    cancelCapture();
    m_controllerSpecific = specific;
    emit controllerSpecificChanged();
    rebuildRows();
}

QString BindingEditorModel::selectedProfile() const
{
    return m_deviceGroup == QLatin1String("controller") && m_controllerSpecific
        ? m_controllerFingerprint : QString();
}

QString BindingEditorModel::scopeLabel(ActionCatalog::Scope scope)
{
    switch (scope) {
    case ActionCatalog::Scope::Global: return QStringLiteral("Global");
    case ActionCatalog::Scope::Overlay: return QStringLiteral("Overlay");
    case ActionCatalog::Scope::Desktop: return QStringLiteral("Gallery");
    case ActionCatalog::Scope::Playback: return QStringLiteral("Playback");
    }
    return {};
}

QString BindingEditorModel::formatTrigger(const BindingResolver::Binding& binding) const
{
    if (binding.triggerCode.isEmpty())
        return {};
    const TriggerSpec trigger = binding.trigger();
    if (m_deviceGroup == QLatin1String("controller"))
        return trigger.label(m_controllerFamily);
    return controlLabel(trigger.firstControl());
}

QString BindingEditorModel::formatBinding(const BindingResolver::Binding& binding) const
{
    if (binding.triggerCode.isEmpty())
        return QStringLiteral("Unassigned");
    QString label = formatTrigger(binding);
    const GestureSpec gesture = binding.gesture();
    if (gesture.kind != GestureSpec::Kind::Press)
        label += QStringLiteral(" · %1").arg(gesture.label());
    return label;
}

QString BindingEditorModel::formatGestureBadge(const BindingResolver::Binding& binding)
{
    if (binding.trigger().isChord())
        return QStringLiteral("Combination");
    const GestureSpec gesture = binding.gesture();
    if (gesture.kind == GestureSpec::Kind::Hold) {
        if (gesture.holdMs == 0)
            return QStringLiteral("Hold · Default");
        const bool wholeSeconds = gesture.holdMs % 1000 == 0;
        return QStringLiteral("Hold · %1 s")
            .arg(gesture.holdMs / 1000.0, 0, 'f', wholeSeconds ? 0 : 1);
    }
    return gesture.label();
}

void BindingEditorModel::rebuildRows()
{
    const QString profile = selectedProfile();
    QHash<QString, BindingResolver::Binding> bindings;
    for (const auto& binding : m_runtime->effectiveBindings(m_deviceGroup, profile))
        bindings.insert(slotKey(binding.actionId, binding.slot), binding);

    QHash<QString, BindingResolver::Binding> baselines;
    for (const auto& binding : m_runtime->baselineBindings(m_deviceGroup, profile))
        baselines.insert(slotKey(binding.actionId, binding.slot), binding);

    QHash<QString, BindingOverrideRow> localOverrides;
    for (const BindingOverrideRow& row : m_database->listBindingOverrides()) {
        if (row.deviceGroup == m_deviceGroup && row.deviceProfile == profile)
            localOverrides.insert(slotKey(row.actionId, row.slot), row);
    }

    QVariantList next;
    for (const auto& action : ActionCatalog::all()) {
        QVariantMap row;
        row.insert(QStringLiteral("actionId"), action.id);
        row.insert(QStringLiteral("label"), action.label);
        row.insert(QStringLiteral("description"), action.description);
        row.insert(QStringLiteral("scope"), scopeLabel(action.scope));
        row.insert(QStringLiteral("bindable"), action.bindable);
        bool actionModified = false;
        for (int slot = 1; slot <= 2; ++slot) {
            const QString key = slotKey(action.id, slot);
            const auto binding = bindings.value(key);
            const auto baselineIt = baselines.constFind(key);
            const auto localIt = localOverrides.constFind(key);
            const BindingResolver::Binding* baseline = baselineIt == baselines.cend()
                ? nullptr : &baselineIt.value();
            const BindingOverrideRow* local = localIt == localOverrides.cend()
                ? nullptr : &localIt.value();
            const QString prefix = slot == 1 ? QStringLiteral("primary")
                                             : QStringLiteral("secondary");
            const bool assigned = !binding.actionId.isEmpty();
            const QString changeState = slotChangeState(
                baseline, assigned ? &binding : nullptr, local);
            actionModified = actionModified || changeState != QLatin1String("default");
            row.insert(prefix, assigned ? formatBinding(binding) : QStringLiteral("Unassigned"));
            row.insert(prefix + QStringLiteral("Assigned"), assigned);
            row.insert(prefix + QStringLiteral("Trigger"),
                       assigned ? formatTrigger(binding) : QString());
            row.insert(prefix + QStringLiteral("Gesture"),
                       assigned ? formatGestureBadge(binding) : QString());
            row.insert(prefix + QStringLiteral("ChangeState"), changeState);
            row.insert(prefix + QStringLiteral("StatusLabel"), statusLabel(changeState));
        }
        row.insert(QStringLiteral("modified"), actionModified);
        next.append(row);
    }
    m_rows = next;
    emit rowsChanged();
}

void BindingEditorModel::retranslate()
{
    rebuildRows();
}

void BindingEditorModel::beginCapture(const QString& actionId, int slot)
{
    const auto* action = ActionCatalog::find(actionId);
    if (!action || !action->bindable || slot < 1 || slot > 2)
        return;
    m_captureActionId = actionId;
    m_captureSlot = slot;
    m_captureActive = true;
    const QString device = m_deviceGroup == QLatin1String("controller") ? QStringLiteral("controller button")
                         : m_deviceGroup == QLatin1String("keyboard") ? QStringLiteral("key or shortcut")
                                                                      : QStringLiteral("middle, Back, or Forward mouse button");
    m_capturePrompt = QStringLiteral("Press a %1 for %2 · Slot %3")
                          .arg(device, action->label).arg(slot);
    // Controllers assign a gesture, not just a button — say which one, so
    // "capture Screenshot" reads as the Tap it will actually become. Keyboard
    // and mouse bindings are always plain presses; no suffix there.
    if (m_deviceGroup == QLatin1String("controller")) {
        const BindingResolver::Gesture gesture =
            m_runtime->inheritedGesture(m_deviceGroup, selectedProfile(), actionId, slot);
        m_capturePrompt += QStringLiteral(" · %1").arg(gesture.spec().label());
    }
    emit captureChanged();
}

void BindingEditorModel::cancelCapture()
{
    if (!m_captureActive)
        return;
    m_captureActive = false;
    m_captureActionId.clear();
    m_capturePrompt.clear();
    emit captureChanged();
}

void BindingEditorModel::setRelationNotice(const QString& kindId, const QString& text)
{
    // Three distinct failures, three kinds — they are never the same problem:
    // unsupported_input  = the controller/backend cannot expose this control;
    // hotkey_unavailable = Windows or another app owns the chord;
    // persistence_error  = the write itself failed.
    // All three populate validationError so QML can style them as errors
    // without parsing the message.
    const bool blocking = kindId == QLatin1String("unsupported_input")
                       || kindId == QLatin1String("hotkey_unavailable")
                       || kindId == QLatin1String("persistence_error");
    const QString error = blocking ? text : QString();
    if (m_relationKind == kindId && m_relationNotice == text && m_validationError == error)
        return;
    m_relationKind = kindId;
    m_relationNotice = text;
    m_validationError = error;
    emit relationNoticeChanged();
}

void BindingEditorModel::reportUnsupportedInput(const QString& displayLabel)
{
    setRelationNotice(QStringLiteral("unsupported_input"),
                      displayLabel.isEmpty()
                          ? QStringLiteral("This controller button is not exposed by the active input backend.")
                          : QStringLiteral("%1 is not exposed by the active input backend.")
                                .arg(displayLabel));
}

void BindingEditorModel::dismissRelationNotice()
{
    setRelationNotice(QStringLiteral("none"), {});
}

bool BindingEditorModel::conversionCanApply(
    const BindingResolver::Binding& target,
    const BindingResolver::Binding& press,
    const QVector<BindingResolver::Binding>& effective,
    BindingResolver::Binding* converted) const
{
    const GestureSpec targetGesture = target.gesture();
    if (press.gesture().kind != GestureSpec::Kind::Press)
        return false;
    if (targetGesture.kind != GestureSpec::Kind::Hold
        && !(targetGesture.kind == GestureSpec::Kind::Tap && targetGesture.tapCount >= 2))
        return false;

    BindingResolver::Binding candidate = press;
    candidate.deviceProfile = target.deviceProfile;
    candidate.activation = QStringLiteral("tap");
    candidate.tapCount = 1;
    candidate.holdMs = 0;

    if (BindingRelation::classify(candidate, target) == BindingRelation::Kind::HardConflict)
        return false;
    for (const auto& binding : effective) {
        if ((binding.actionId == press.actionId && binding.slot == press.slot)
            || (binding.actionId == target.actionId && binding.slot == target.slot))
            continue;
        if (BindingRelation::classify(candidate, binding) == BindingRelation::Kind::HardConflict)
            return false;
    }
    if (converted)
        *converted = candidate;
    return true;
}

BindingEditorModel::PendingChange BindingEditorModel::pendingChangeFor(
    const BindingResolver::Binding& target) const
{
    PendingChange change;
    change.target = target;
    const auto effective = m_runtime->effectiveBindings(target.deviceGroup, target.deviceProfile);
    for (const auto& binding : effective) {
        if (binding.actionId == target.actionId && binding.slot == target.slot)
            continue;
        const BindingRelation::Kind kind = BindingRelation::classify(target, binding);
        if (kind == BindingRelation::Kind::HardConflict) {
            change.conflicts.append(binding);
            continue;
        }
        if (kind != BindingRelation::Kind::ConversionRequired)
            continue;
        BindingResolver::Binding converted;
        if (!change.hasConversion
            && conversionCanApply(target, binding, effective, &converted)) {
            change.hasConversion = true;
            change.conversionSource = binding;
            change.conversionTarget = converted;
        } else {
            // A second candidate or a conversion that would create another
            // conflict remains destructive; never guess which Press to alter.
            change.conflicts.append(binding);
        }
    }
    if (change.hasConversion && !change.conflicts.isEmpty()) {
        change.conflicts.append(change.conversionSource);
        change.hasConversion = false;
        change.conversionSource = {};
        change.conversionTarget = {};
    }
    return change;
}

QString BindingEditorModel::conflictMessageFor(
    const BindingResolver::Binding& target,
    const BindingResolver::Binding& partner,
    const QString& displayLabel) const
{
    const auto* other = ActionCatalog::find(partner.actionId);
    const QString otherLabel = other ? other->label : partner.actionId;
    const GestureSpec targetGesture = target.gesture();
    const GestureSpec partnerGesture = partner.gesture();
    if (targetGesture.kind == GestureSpec::Kind::Press
        || partnerGesture.kind == GestureSpec::Kind::Press) {
        const QString timed = targetGesture.kind == GestureSpec::Kind::Press
            ? partnerGesture.label() : targetGesture.label();
        return QStringLiteral("%1 uses Press for %2. It cannot be distinguished from %3 "
                              "in this context without changing its button-down behavior.")
            .arg(displayLabel, otherLabel, timed);
    }
    return QStringLiteral("%1 · %2 is already assigned to %3 in this context.")
        .arg(displayLabel, targetGesture.label(), otherLabel);
}

QString BindingEditorModel::compatibilityMessageFor(
    const PendingChange& change, const QString& displayLabel) const
{
    const auto* existingAction = ActionCatalog::find(change.conversionSource.actionId);
    const QString existingLabel = existingAction ? existingAction->label
                                                  : change.conversionSource.actionId;
    const GestureSpec requested = change.target.gesture();
    QString consequence = QStringLiteral("%1 will then activate after the button is released")
                              .arg(existingLabel);
    if (requested.kind == GestureSpec::Kind::Tap && requested.tapCount >= 2) {
        consequence += QStringLiteral(" and may wait up to %1 ms to rule out the %2 action")
                           .arg(m_runtime->timing().multiTapIntervalMs)
                           .arg(requested.label());
    }
    consequence += QLatin1Char('.');
    return QStringLiteral(
        "%1 currently activates %2 immediately when the button is pressed.\n\n"
        "To also use %1 for %3, GameHQ must change %2 from Press to Single tap.\n\n%4")
        .arg(displayLabel, existingLabel, requested.label(), consequence);
}

bool BindingEditorModel::captureInput(const QString& deviceGroup, const QString& triggerCode,
                                      const QString& displayLabel)
{
    if (!m_captureActive || deviceGroup != m_deviceGroup || triggerCode.isEmpty())
        return false;
    // The dialog records into its draft rather than saving on the spot: a
    // combination needs two buttons and a gesture needs choosing.
    if (m_editorOpen)
        return editorCaptureInput(deviceGroup, triggerCode);

    const QString profile = selectedProfile();
    const auto effective = m_runtime->effectiveBindings(deviceGroup, profile);
    // The gesture belongs to the slot, not to the trigger being captured: an
    // empty secondary Screenshot is still a tap, a cleared Save Replay is
    // still a hold. Inheriting it here keeps the relation classification below
    // honest — a guessed "press" manufactured HardConflicts on any trigger
    // that already carried a timed gesture.
    const BindingResolver::Gesture gesture =
        m_runtime->inheritedGesture(deviceGroup, profile, m_captureActionId, m_captureSlot);
    BindingResolver::Binding target{deviceGroup, profile, m_captureActionId, m_captureSlot,
                                    triggerCode, gesture.activation, gesture.holdMs, false,
                                    gesture.tapCount};
    if (deviceGroup != QLatin1String("controller")) {
        target.activation = QStringLiteral("press");
        target.holdMs = 0;
        target.tapCount = 1;
    }

    // One shared policy decides what the new binding means next to every
    // existing one. Only HardConflict blocks; the softer tiers are recorded so
    // the editor can explain the result without refusing a valid assignment.
    PendingChange change = pendingChangeFor(target);
    BindingRelation::Kind notice = BindingRelation::Kind::None;
    BindingResolver::Binding noticePartner;
    for (const auto& binding : effective) {
        if (binding.actionId == target.actionId && binding.slot == target.slot)
            continue;
        const BindingRelation::Kind kind = BindingRelation::classify(target, binding);
        if (noticeSeverity(kind) > noticeSeverity(notice)) {
            notice = kind;
            noticePartner = binding;
        }
    }

    m_captureActive = false;
    m_capturePrompt.clear();
    emit captureChanged();
    if (!change.conflicts.isEmpty()) {
        m_pending = change;
        m_conflictPending = true;
        m_conflictMessage = conflictMessageFor(target, change.conflicts.first(), displayLabel);
        setRelationNotice(BindingRelation::kindId(BindingRelation::Kind::HardConflict),
                          m_conflictMessage);
        emit conflictChanged();
        return true;
    }
    if (change.hasConversion) {
        m_pending = change;
        m_compatibilityPending = true;
        m_compatibilityMessage = compatibilityMessageFor(change, displayLabel);
        setRelationNotice(BindingRelation::kindId(BindingRelation::Kind::ConversionRequired),
                          m_compatibilityMessage);
        emit compatibilityChanged();
        return true;
    }

    // A rejected chord or a failed write leaves its own explanation in place;
    // do not overwrite it with a success notice.
    if (!applyChange(change))
        return true;
    setRelationNotice(BindingRelation::kindId(notice),
                      noticeTextFor(notice, target, noticePartner, displayLabel));
    return true;
}

QString BindingEditorModel::noticeTextFor(BindingRelation::Kind kind,
                                          const BindingResolver::Binding& target,
                                          const BindingResolver::Binding& partner,
                                          const QString& displayLabel) const
{
    if (kind == BindingRelation::Kind::None)
        return {};
    const auto* targetAction = ActionCatalog::find(target.actionId);
    const auto* partnerAction = ActionCatalog::find(partner.actionId);
    if (!targetAction || !partnerAction)
        return {};

    switch (kind) {
    case BindingRelation::Kind::ContextOverride:
        // Saved, but the user should know one of the two only runs in context.
        return QStringLiteral("%1 replaces %2 while %3 is active. Both are saved.")
            .arg(targetAction->label, partnerAction->label,
                 scopeLabel(targetAction->scope == ActionCatalog::Scope::Global
                                ? partnerAction->scope : targetAction->scope));
    case BindingRelation::Kind::Redundant:
        return QStringLiteral("%1 already does the same thing here. The duplicate has no effect.")
            .arg(partnerAction->label);
    case BindingRelation::Kind::SharedGesture:
        return QStringLiteral("%1 is shared: %2 = %3, %4 = %5.")
            .arg(displayLabel,
                 gestureLabel(target), targetAction->label,
                 gestureLabel(partner), partnerAction->label);
    case BindingRelation::Kind::ConversionRequired:
    case BindingRelation::Kind::HardConflict:
    case BindingRelation::Kind::None:
        break;
    }
    return {};
}

QString BindingEditorModel::gestureLabel(const BindingResolver::Binding& binding)
{
    return binding.gesture().label();
}

bool BindingEditorModel::legacyCopyAvailable() const
{
    if (m_deviceGroup != QLatin1String("controller") || !m_controllerSpecific)
        return false;
    const QString profile = selectedProfile();
    if (profile.isEmpty() || ControllerIdentity::isLegacySlotFingerprint(profile))
        return false;
    for (const BindingOverrideRow& row : m_database->listBindingOverrides()) {
        if (row.deviceGroup == QLatin1String("controller")
            && ControllerIdentity::isLegacySlotFingerprint(row.deviceProfile))
            return true;
    }
    return false;
}

void BindingEditorModel::copyLegacyOverridesToController()
{
    if (!legacyCopyAvailable())
        return;
    const QString profile = selectedProfile();
    const auto rows = m_database->listBindingOverrides();
    QSet<QString> owned;   // keys this controller already has its own row for
    for (const BindingOverrideRow& row : rows) {
        if (row.deviceGroup == QLatin1String("controller") && row.deviceProfile == profile)
            owned.insert(row.actionId + QLatin1Char('#') + QString::number(row.slot));
    }
    for (BindingOverrideRow row : rows) {
        if (row.deviceGroup != QLatin1String("controller")
            || !ControllerIdentity::isLegacySlotFingerprint(row.deviceProfile))
            continue;
        // Copy, never move — the slot rows keep covering pads that cannot be
        // identified — and never clobber a row already saved for this pad.
        if (owned.contains(row.actionId + QLatin1Char('#') + QString::number(row.slot)))
            continue;
        row.deviceProfile = profile;
        m_database->upsertBindingOverride(row);
    }
    reloadAndRefresh();
}

void BindingEditorModel::retryConflictCapture()
{
    if (!m_conflictPending)
        return;
    const QString actionId = m_pending.target.actionId;
    const int slot = m_pending.target.slot;
    const bool dialogDraft = m_editorOpen && m_editorActionId == actionId && m_editorSlot == slot;
    dismissConflict();
    if (dialogDraft)
        beginTriggerCapture(1);
    else
        beginCapture(actionId, slot);
}

void BindingEditorModel::confirmCompatibility()
{
    if (!m_compatibilityPending)
        return;
    const PendingChange change = m_pending;
    const QString triggerLabel = formatTrigger(change.conversionSource);
    const auto* convertedAction = ActionCatalog::find(change.conversionSource.actionId);
    const auto* targetAction = ActionCatalog::find(change.target.actionId);
    dismissCompatibility();
    if (!applyChange(change))
        return;
    setRelationNotice(
        QStringLiteral("shared_gesture"),
        QStringLiteral("%1 now uses Single tap for %2 and %3 for %4.")
            .arg(triggerLabel,
                 convertedAction ? convertedAction->label : change.conversionSource.actionId,
                 change.target.gesture().label(),
                 targetAction ? targetAction->label : change.target.actionId));
    closeAssignmentEditor();
}

void BindingEditorModel::dismissCompatibility()
{
    if (!m_compatibilityPending)
        return;
    m_compatibilityPending = false;
    m_compatibilityMessage.clear();
    m_pending = {};
    emit compatibilityChanged();
}

void BindingEditorModel::retryCompatibilityCapture()
{
    if (!m_compatibilityPending)
        return;
    const QString actionId = m_pending.target.actionId;
    const int slot = m_pending.target.slot;
    const bool dialogDraft = m_editorOpen && m_editorActionId == actionId && m_editorSlot == slot;
    dismissCompatibility();
    if (dialogDraft)
        beginTriggerCapture(1);
    else
        beginCapture(actionId, slot);
}

void BindingEditorModel::setHotkeyApply(HotkeyApply apply)
{
    m_hotkeyApply = std::move(apply);
}

void BindingEditorModel::setPersistRow(PersistRow persist)
{
    m_persistRow = std::move(persist);
}

bool BindingEditorModel::persist(const BindingOverrideRow& row)
{
    return m_persistRow ? m_persistRow(row) : m_database->upsertBindingOverride(row);
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
            reloadAndRefresh();
            return false;
        }
        reloadAndRefresh();
        return true;
    }

    const bool ownsHotkey = m_hotkeyApply && isGlobalHotkey(target);

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

    // 3) Persist: the displaced bindings first, then the new one. Remember what
    //    each displaced key held before the transaction — undoing a write must
    //    put that row back, not delete it: a displaced action that already had
    //    a custom override would otherwise roll back to the shipped default.
    const auto overrideKey = [](const BindingOverrideRow& row) {
        return row.deviceGroup + QLatin1Char('|') + row.deviceProfile + QLatin1Char('|')
             + row.actionId + QLatin1Char('#') + QString::number(row.slot);
    };
    QHash<QString, BindingOverrideRow> before;
    if (!change.conflicts.isEmpty()) {
        for (const BindingOverrideRow& row : m_database->listBindingOverrides())
            before.insert(overrideKey(row), row);
    }
    QVector<BindingOverrideRow> written;
    bool persisted = true;
    for (const auto& conflict : change.conflicts) {
        BindingOverrideRow row{conflict.deviceGroup, profile, conflict.actionId,
                               conflict.slot, {}, conflict.activation, conflict.holdMs, true,
                               conflict.tapCount};
        if (!persist(row)) {
            persisted = false;
            break;
        }
        written.append(row);
    }
    if (persisted) {
        BindingOverrideRow row{target.deviceGroup, profile, target.actionId, target.slot,
                               target.triggerCode, target.activation, target.holdMs, false,
                               target.tapCount};
        persisted = persist(row);
    }

    // 4) A failed write rolls the OS back to the chord that was live before, and
    //    undoes the rows that did land, so the three views cannot disagree.
    if (!persisted) {
        for (const auto& row : written) {
            const auto it = before.constFind(overrideKey(row));
            if (it != before.cend())
                persist(*it);
            else
                m_database->clearBindingOverride(row.deviceGroup, row.deviceProfile,
                                                 row.actionId, row.slot);
        }
        if (ownsHotkey) {
            QString reason;
            m_hotkeyApply(target.actionId, target.slot, previousChord, &reason);
        }
        setRelationNotice(QStringLiteral("persistence_error"),
                          QStringLiteral("This shortcut could not be saved. The previous "
                                         "assignment is still active."));
        reloadAndRefresh();
        return false;
    }

    // 5) Only a fully committed change refreshes the editor.
    reloadAndRefresh();
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
        m_runtime->inheritedGesture(m_deviceGroup, selectedProfile(), actionId, slot);
    BindingOverrideRow row{m_deviceGroup, selectedProfile(), actionId, slot, {},
                           gesture.activation, gesture.holdMs, true, gesture.tapCount};
    m_database->upsertBindingOverride(row);
    reloadAndRefresh();
}

void BindingEditorModel::resetBinding(const QString& actionId, int slot)
{
    const auto* action = ActionCatalog::find(actionId);
    if (!action || !action->bindable || slot < 1 || slot > 2)
        return;
    m_database->clearBindingOverride(m_deviceGroup, selectedProfile(), actionId, slot);
    reloadAndRefresh();
}

void BindingEditorModel::resetAction(const QString& actionId)
{
    for (int slot = 1; slot <= 2; ++slot)
        m_database->clearBindingOverride(m_deviceGroup, selectedProfile(), actionId, slot);
    reloadAndRefresh();
}

void BindingEditorModel::resetCurrentProfile()
{
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

void BindingEditorModel::setControllerProfile(const ControlId::DeviceProfile& profile)
{
    const bool availabilityChanged = m_controllerFingerprint.isEmpty() != profile.fingerprint.isEmpty();
    m_controllerFingerprint = profile.fingerprint;
    m_controllerName = profile.displayName;
    m_controllerFamily = profile.family;
    if (m_controllerSpecific && m_controllerFingerprint.isEmpty()) {
        m_controllerSpecific = false;
        emit controllerSpecificChanged();
    }
    emit controllerProfileChanged();
    if (m_deviceGroup == QLatin1String("controller") || availabilityChanged)
        rebuildRows();
}

void BindingEditorModel::setLastFiredAction(const QString& actionId)
{
    const auto* action = ActionCatalog::find(actionId);
    const QString text = action ? action->label : actionId;
    if (m_lastFiredAction == text)
        return;
    m_lastFiredAction = text;
    emit lastFiredActionChanged();
}

// ---------------------------------------------------------------------------
// Edit Assignment dialog
//
// Everything below manipulates a DRAFT. A gesture and a combination cannot be
// captured — they have to be chosen — so the old "press a button and we save
// it" flow could never express them. The draft holds the whole pattern, the
// dialog edits it, and only saveAssignment() touches the database.
// ---------------------------------------------------------------------------

QString BindingEditorModel::controlLabel(const QString& controlId) const
{
    if (controlId.isEmpty())
        return {};
    if (m_deviceGroup == QLatin1String("controller"))
        return ControlId::label(controlId, m_controllerFamily);
    if (controlId == QLatin1String("mouse.button4")) return QStringLiteral("Mouse Back");
    if (controlId == QLatin1String("mouse.button5")) return QStringLiteral("Mouse Forward");
    if (controlId == QLatin1String("mouse.middle"))  return QStringLiteral("Middle Mouse");
    return controlId;
}

TriggerSpec BindingEditorModel::editorTrigger() const
{
    if (m_editorTriggerKind == QLatin1String("combination"))
        return TriggerSpec::orderedChord(m_editorFirstControl, m_editorSecondControl);
    return TriggerSpec::single(m_editorFirstControl);
}

BindingResolver::Binding BindingEditorModel::editorBinding() const
{
    return {m_deviceGroup, selectedProfile(), m_editorActionId, m_editorSlot,
            editorTrigger().serialize(), m_editorGesture.activationCode(),
            m_editorGesture.holdMs, false, m_editorGesture.tapCount};
}

QString BindingEditorModel::editorTriggerLabel() const
{
    if (m_editorFirstControl.isEmpty())
        return QStringLiteral("Not set");
    if (m_editorTriggerKind != QLatin1String("combination"))
        return controlLabel(m_editorFirstControl);
    if (m_editorSecondControl.isEmpty())
        return QStringLiteral("%1 + ...").arg(controlLabel(m_editorFirstControl));
    return QStringLiteral("%1 + %2").arg(controlLabel(m_editorFirstControl),
                                          controlLabel(m_editorSecondControl));
}

QString BindingEditorModel::editorTriggerHint() const
{
    if (m_editorCaptureStep == QLatin1String("first")) {
        return m_editorTriggerKind == QLatin1String("combination")
            ? QStringLiteral("Press the button you want to HOLD first.")
            : QStringLiteral("Press the button you want to assign.");
    }
    if (m_editorCaptureStep == QLatin1String("second"))
        return QStringLiteral("Now press the second button.");
    if (m_editorTriggerKind == QLatin1String("combination") && !m_editorSecondControl.isEmpty()) {
        // Say what to do, not just what it is called: "View + Guide" alone does
        // not tell anyone that one button is held and the other tapped.
        return QStringLiteral("Hold %1, then press %2.")
            .arg(controlLabel(m_editorFirstControl), controlLabel(m_editorSecondControl));
    }
    return {};
}

bool BindingEditorModel::editorCanSave() const
{
    if (!m_editorOpen || m_editorFirstControl.isEmpty())
        return false;
    if (m_editorTriggerKind == QLatin1String("combination")
        && (m_editorSecondControl.isEmpty() || m_editorSecondControl == m_editorFirstControl))
        return false;
    QString error;
    return BindingPattern{editorTrigger(), m_editorGesture}.isValid(m_deviceGroup, &error);
}

void BindingEditorModel::openAssignmentEditor(const QString& actionId, int slot)
{
    const auto* action = ActionCatalog::find(actionId);
    if (!action || !action->bindable || slot < 1 || slot > 2)
        return;
    cancelCapture();
    m_editorOpen = true;
    m_editorActionId = actionId;
    m_editorActionLabel = action->label;
    m_editorScopeLabel = scopeLabel(action->scope);
    m_editorSlot = slot;
    m_editorCaptureStep = QStringLiteral("idle");
    m_editorFirstControl.clear();
    m_editorSecondControl.clear();
    m_editorTriggerKind = QStringLiteral("single");

    // Seed from what the slot holds today, so opening the dialog on a bound
    // slot shows that binding instead of an empty form.
    for (const auto& binding : m_runtime->effectiveBindings(m_deviceGroup, selectedProfile())) {
        if (binding.actionId != actionId || binding.slot != slot)
            continue;
        const TriggerSpec trigger = binding.trigger();
        m_editorFirstControl = trigger.firstControl();
        m_editorSecondControl = trigger.secondControl();
        m_editorTriggerKind = trigger.isChord() ? QStringLiteral("combination")
                                                : QStringLiteral("single");
        m_editorGesture = binding.gesture();
        refreshEditorNotice();
        emit editorChanged();
        return;
    }
    // Empty slot: inherit the gesture the slot means, exactly as a plain
    // capture does, so a second Screenshot button still starts out as a tap.
    m_editorGesture = m_runtime->inheritedGesture(m_deviceGroup, selectedProfile(),
                                                  actionId, slot).spec();
    if (m_deviceGroup != QLatin1String("controller"))
        m_editorGesture = GestureSpec::press();
    refreshEditorNotice();
    emit editorChanged();
}

void BindingEditorModel::closeAssignmentEditor()
{
    if (!m_editorOpen)
        return;
    cancelTriggerCapture();
    m_editorOpen = false;
    m_editorNotice.clear();
    m_editorNoticeKind = QStringLiteral("none");
    emit editorChanged();
}

void BindingEditorModel::setEditorTriggerKind(const QString& kind)
{
    if (!m_editorOpen)
        return;
    const bool combination = kind == QLatin1String("combination");
    if (combination && !editorCombinationAvailable())
        return;
    m_editorTriggerKind = combination ? QStringLiteral("combination")
                                      : QStringLiteral("single");
    if (combination) {
        // Version 1 chords fire on the second button's down edge. Layering a
        // tap count or a hold on an already two-stage trigger multiplies the
        // waiting windows, so the gesture is fixed here rather than offered
        // and then rejected on save.
        m_editorGesture = GestureSpec::press();
    } else {
        m_editorSecondControl.clear();
    }
    refreshEditorNotice();
    emit editorChanged();
}

void BindingEditorModel::beginTriggerCapture(int step)
{
    if (!m_editorOpen)
        return;
    // Capture is an explicit mode. Until it is on, the pad still navigates the
    // dialog; once it is on, captureInput() consumes every control so pressing
    // Share records Share instead of taking a screenshot.
    m_captureActive = true;
    m_editorCaptureStep = step >= 2 ? QStringLiteral("second") : QStringLiteral("first");
    if (m_editorCaptureStep == QLatin1String("first"))
        m_editorFirstControl.clear();
    m_editorSecondControl.clear();
    emit captureChanged();
    emit editorChanged();
}

void BindingEditorModel::cancelTriggerCapture()
{
    if (m_editorCaptureStep == QLatin1String("idle") && !m_captureActive)
        return;
    m_captureActive = false;
    m_editorCaptureStep = QStringLiteral("idle");
    emit captureChanged();
    emit editorChanged();
}

void BindingEditorModel::setEditorGesture(const QString& kind, int tapCount, int holdMs)
{
    if (!m_editorOpen || editorGestureLocked())
        return;
    const auto parsed = GestureSpec::parse(kind, qBound(1, tapCount, GestureSpec::kMaxTapCount),
                                           qMax(0, holdMs));
    if (!parsed.ok)
        return;
    m_editorGesture = parsed.gesture;
    refreshEditorNotice();
    emit editorChanged();
}

bool BindingEditorModel::controlWasObserved(const QString& controlId) const
{
    return m_observedControls.contains(controlId);
}

void BindingEditorModel::noteObservedControl(const QString& controlId)
{
    if (controlId.isEmpty() || m_observedControls.contains(controlId))
        return;
    m_observedControls.insert(controlId);
    if (m_editorOpen) {
        refreshEditorNotice();
        emit editorChanged();
    }
}

// Fills the draft's trigger instead of saving, while the dialog is capturing.
bool BindingEditorModel::editorCaptureInput(const QString& deviceGroup, const QString& triggerCode)
{
    if (deviceGroup != m_deviceGroup || triggerCode.isEmpty())
        return false;
    if (m_editorCaptureStep == QLatin1String("first")) {
        m_editorFirstControl = triggerCode;
        // A combination is captured stepwise: recording both buttons at once
        // would need them pressed simultaneously, which is exactly the
        // unordered chord v1 deliberately does not support.
        if (m_editorTriggerKind == QLatin1String("combination")) {
            m_editorCaptureStep = QStringLiteral("second");
        } else {
            m_captureActive = false;
            m_editorCaptureStep = QStringLiteral("idle");
        }
    } else if (m_editorCaptureStep == QLatin1String("second")) {
        if (triggerCode == m_editorFirstControl) {
            // Same button twice is not a combination. Stay in capture rather
            // than saving something that would fail validation later.
            m_editorNoticeKind = QStringLiteral("invalid_pattern");
            m_editorNotice = QStringLiteral("A combination needs two different buttons.");
            emit editorChanged();
            return true;
        }
        m_editorSecondControl = triggerCode;
        m_captureActive = false;
        m_editorCaptureStep = QStringLiteral("idle");
    } else {
        return false;
    }
    refreshEditorNotice();
    emit captureChanged();
    emit editorChanged();
    return true;
}

// Everything the user should know before saving: what the draft collides with,
// why it might feel slow, and whether the pad has ever produced the button.
void BindingEditorModel::refreshEditorNotice()
{
    m_editorNotice.clear();
    m_editorNoticeKind = QStringLiteral("none");
    if (!m_editorOpen || m_editorFirstControl.isEmpty())
        return;

    const TriggerSpec trigger = editorTrigger();
    if (m_deviceGroup == QLatin1String("controller")) {
        for (const QString& control : trigger.controls) {
            if (control.isEmpty() || controlWasObserved(control))
                continue;
            // Only said about buttons the pad genuinely never delivered — the
            // Guide button is the usual casualty, intercepted by Steam or the
            // Game Bar before it reaches any application.
            m_editorNoticeKind = QStringLiteral("unsupported_input");
            m_editorNotice = QStringLiteral(
                "GameHQ has not received %1 yet. Another application may be intercepting it.")
                                     .arg(controlLabel(control));
            return;
        }
    }

    const BindingResolver::Binding target = editorBinding();
    const auto timing = m_runtime->timing();
    const PendingChange pending = pendingChangeFor(target);
    if (!pending.conflicts.isEmpty()) {
        m_editorNoticeKind = BindingRelation::kindId(BindingRelation::Kind::HardConflict);
        m_editorNotice = conflictMessageFor(target, pending.conflicts.first(),
                                            editorTriggerLabel());
        return;
    }
    if (pending.hasConversion) {
        m_editorNoticeKind = BindingRelation::kindId(BindingRelation::Kind::ConversionRequired);
        m_editorNotice = compatibilityMessageFor(pending, editorTriggerLabel());
        return;
    }
    BindingRelation::Kind worst = BindingRelation::Kind::None;
    BindingRelation::Notice notice = BindingRelation::Notice::None;
    BindingResolver::Binding partner;
    for (const auto& binding : m_runtime->effectiveBindings(m_deviceGroup, selectedProfile())) {
        if (binding.actionId == target.actionId && binding.slot == target.slot)
            continue;
        const auto kind = BindingRelation::classify(target, binding);
        if (kind == BindingRelation::Kind::HardConflict) {
            worst = kind;
            partner = binding;
            break;
        }
        if (notice == BindingRelation::Notice::None)
            notice = BindingRelation::noticeFor(target, binding);
        if (noticeSeverity(kind) > noticeSeverity(worst)) {
            worst = kind;
            partner = binding;
        }
    }

    if (worst == BindingRelation::Kind::HardConflict)
        return;
    if (notice != BindingRelation::Notice::None) {
        m_editorNoticeKind = BindingRelation::noticeId(notice);
        m_editorNotice = BindingRelation::noticeText(notice, timing.chordWindowMs,
                                                     timing.multiTapIntervalMs);
        return;
    }
    if (worst != BindingRelation::Kind::None) {
        m_editorNoticeKind = BindingRelation::kindId(worst);
        m_editorNotice = noticeTextFor(worst, target, partner, editorTriggerLabel());
    }
}

void BindingEditorModel::saveAssignment()
{
    if (!editorCanSave())
        return;
    const BindingResolver::Binding target = editorBinding();

    PendingChange change = pendingChangeFor(target);

    if (!change.conflicts.isEmpty()) {
        // The dialog already said this would replace something; the modal is
        // still where the user confirms it, so both paths stay identical.
        m_pending = change;
        m_conflictPending = true;
        m_conflictMessage = conflictMessageFor(target, change.conflicts.first(),
                                               editorTriggerLabel());
        setRelationNotice(BindingRelation::kindId(BindingRelation::Kind::HardConflict),
                          m_conflictMessage);
        emit conflictChanged();
        return;
    }
    if (change.hasConversion) {
        m_pending = change;
        m_compatibilityPending = true;
        m_compatibilityMessage = compatibilityMessageFor(change, editorTriggerLabel());
        setRelationNotice(BindingRelation::kindId(BindingRelation::Kind::ConversionRequired),
                          m_compatibilityMessage);
        emit compatibilityChanged();
        return;
    }
    if (!applyChange(change))
        return;
    closeAssignmentEditor();
}
