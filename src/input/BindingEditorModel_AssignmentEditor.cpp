// BindingEditorModel: the Edit Assignment dialog (draft trigger/gesture
// editing, trigger capture, notices and saveAssignment()). Split out of
// BindingEditorModel.cpp; the class itself is declared in BindingEditorModel.h.
#include "input/BindingEditorModel.h"
#include "input/BindingEditorModelInternal.h"

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

using BindingEditorInternal::noticeSeverity;

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
    if (m_editorActionId == QLatin1String("global.save_replay")
        && m_editorGesture.kind == GestureSpec::Kind::Hold && !m_editorFirstControl.isEmpty()) {
        const int holdMs = m_editorGesture.holdMs > 0
            ? m_editorGesture.holdMs : m_runtime->timing().defaultHoldMs;
        return NativeText::get(
            //: Save-replay binding guidance; %1 is a controller button, %2 the effective hold duration in milliseconds.
            //% "Hold %1 for at least %2 ms to save a replay."
            QT_TRID_NOOP("gamehq.input.assignment.replay_hold_hint"),
            "Hold %1 for at least %2 ms to save a replay.").arg(controlLabel(m_editorFirstControl)).arg(holdMs);
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
    for (const auto& binding : editedBindings(m_deviceGroup, selectedProfile())) {
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
    m_editorGesture = editedGesture(m_deviceGroup, selectedProfile(), actionId, slot).spec();
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
    for (const auto& binding : editedBindings(m_deviceGroup, selectedProfile())) {
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
