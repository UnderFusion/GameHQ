#include "input/BindingRelation.h"

#include "input/ContextOverrideCatalog.h"
#include "localization/NativeText.h"

#include <QLocale>

namespace {

// Every control a trigger physically involves: one for a single, two for a
// chord. Two bindings that share none of them can never interact.
bool sharesAnyControl(const TriggerSpec& left, const TriggerSpec& right)
{
    for (const QString& control : left.controls) {
        if (right.controls.contains(control))
            return true;
    }
    return false;
}

bool repeatsWhileHeld(const ActionCatalog::Action* action)
{
    return action && action->repeats;
}

// A binding that acts the moment the button goes down, with no window in which
// a chord could still form: a plain press, or a navigation step that repeats
// while held.
bool actsImmediately(const GestureSpec& gesture, const ActionCatalog::Action* action)
{
    return gesture.kind == GestureSpec::Kind::Press || repeatsWhileHeld(action);
}

bool canConvertPress(const ActionCatalog::Action* action)
{
    return action && action->bindable && !action->repeats && !action->requiresImmediatePress;
}

bool isConvertibleTimedGesture(const GestureSpec& gesture)
{
    return gesture.kind == GestureSpec::Kind::Hold
        || (gesture.kind == GestureSpec::Kind::Tap && gesture.tapCount >= 2);
}

} // namespace

bool BindingRelation::scopesOverlap(ActionCatalog::Scope left, ActionCatalog::Scope right)
{
    using Scope = ActionCatalog::Scope;
    // Global is always live, so it unions with whatever context is active.
    if (left == Scope::Global || right == Scope::Global)
        return true;
    // Two different contextual scopes never dispatch together. The resolver
    // takes the primary scope's matches and falls back to the secondary only
    // when primary produced none, so Playback-over-Desktop (gallery lightbox)
    // and Playback-over-Overlay are priority chains, not collisions: binding
    // D-pad Left to both playback.seek_back and desktop.navigate_left is the
    // shipped default and is correct.
    return left == right;
}

bool BindingRelation::gesturesCompatible(const GestureSpec& left, const GestureSpec& right)
{
    // Press resolves on the down edge, before any timed gesture is known, so it
    // never shares a button.
    if (left.kind == GestureSpec::Kind::Press || right.kind == GestureSpec::Kind::Press)
        return false;
    // Two taps coexist only when they need a different number of them: x1 and
    // x2 are separated in time, x2 and x2 are the same event. Hold durations do
    // not separate anything — the shorter one always fires first — so two holds
    // on one button stay a conflict regardless of their thresholds.
    if (left.kind == right.kind)
        return left.kind == GestureSpec::Kind::Tap && left.tapCount != right.tapCount;
    // Tap and hold: the runtime tells them apart by how long the button is down.
    return true;
}

BindingRelation::Kind BindingRelation::classify(const BindingResolver::Binding& left,
                                                const BindingResolver::Binding& right)
{
    // 0) Preconditions. A keyboard chord and a controller button never compete,
    //    and neither do two triggers with no control in common — the same
    //    action reached from both Ctrl+Shift+S and F12 is a deliberate second
    //    slot, not a fault.
    if (left.deviceGroup != right.deviceGroup)
        return Kind::None;
    if (left.triggerCode.isEmpty() || right.triggerCode.isEmpty())
        return Kind::None;
    if (left.unbound || right.unbound)
        return Kind::None;

    const TriggerSpec leftTrigger = left.trigger();
    const TriggerSpec rightTrigger = right.trigger();
    if (!sharesAnyControl(leftTrigger, rightTrigger))
        return Kind::None;

    const ActionCatalog::Action* leftAction = ActionCatalog::find(left.actionId);
    const ActionCatalog::Action* rightAction = ActionCatalog::find(right.actionId);
    if (!leftAction || !rightAction)
        return Kind::None;

    // 1) Scopes that are never active together cannot collide.
    if (!scopesOverlap(leftAction->scope, rightAction->scope))
        return Kind::None;

    const GestureSpec leftGesture = left.gesture();
    const GestureSpec rightGesture = right.gesture();

    // 2) A declared substitution is expected behavior, not a conflict. Both
    //    directions are checked because argument order is the caller's choice.
    if (ContextOverrideCatalog::shadows(left.actionId, right.actionId, leftGesture)
        || ContextOverrideCatalog::shadows(right.actionId, left.actionId, rightGesture)) {
        return Kind::ContextOverride;
    }

    if (left.actionId == right.actionId) {
        // 3) Truly duplicated work: the same action reached twice the same way.
        if (leftTrigger == rightTrigger && leftGesture.kind == rightGesture.kind
            && leftGesture.tapCount == rightGesture.tapCount
            && left.deviceProfile == right.deviceProfile)
            return Kind::Redundant;
        // A device-specific row layered over a group-wide row for the same
        // action is the override mechanism working as designed; the resolver
        // merges them and only one survives.
        if (left.deviceProfile != right.deviceProfile)
            return Kind::None;
    }

    // 4) Chord rules. Appended to the frozen order, never interleaved with it.
    const bool leftIsChord = leftTrigger.isChord();
    const bool rightIsChord = rightTrigger.isChord();
    if (leftIsChord || rightIsChord) {
        if (leftIsChord && rightIsChord) {
            // The same combination twice is a straight collision; two different
            // chords sharing a first control are fine, because the second
            // control is what selects between them.
            return leftTrigger == rightTrigger ? Kind::HardConflict : Kind::None;
        }

        const TriggerSpec& chord = leftIsChord ? leftTrigger : rightTrigger;
        const bool singleIsLeft = rightIsChord;
        const GestureSpec& singleGesture = singleIsLeft ? leftGesture : rightGesture;
        const ActionCatalog::Action* singleAction = singleIsLeft ? leftAction : rightAction;
        const QString& singleControl = singleIsLeft ? leftTrigger.firstControl()
                                                    : rightTrigger.firstControl();

        if (singleControl == chord.firstControl()) {
            // Deterministic latency rule, frozen: the first control of a chord
            // has to be holdable. A press that acts on the down edge, or a
            // navigation step that repeats while held, would fire on the way to
            // the second button — so this is a hard conflict in v1, with no
            // "we'll just delay it" escape hatch.
            if (actsImmediately(singleGesture, singleAction))
                return Kind::HardConflict;
            // A tap or hold on the same button is allowed: the chord consumes
            // the components when it completes, and the constituent is replayed
            // when it does not. It does cost the chord window, which is what
            // noticeFor() reports.
            return Kind::SharedGesture;
        }
        // The second control keeps its ordinary behaviour unless the chord
        // actually completes, in which case the chord consumes it.
        return Kind::SharedGesture;
    }

    // 5) Complementary gestures share one button on purpose.
    if (leftTrigger == rightTrigger && gesturesCompatible(leftGesture, rightGesture))
        return Kind::SharedGesture;

    // A non-repeating, editable Press can be made compatible with a timed
    // gesture by explicitly converting it to Tap x1. This verdict is only the
    // pairwise candidate: the editor still preflights the converted row against
    // the complete effective set before offering the operation.
    if (leftTrigger == rightTrigger && !leftTrigger.isChord()) {
        if (leftGesture.kind == GestureSpec::Kind::Press
            && isConvertibleTimedGesture(rightGesture) && canConvertPress(leftAction))
            return Kind::ConversionRequired;
        if (rightGesture.kind == GestureSpec::Kind::Press
            && isConvertibleTimedGesture(leftGesture) && canConvertPress(rightAction))
            return Kind::ConversionRequired;
    }

    // Either the same gesture drives two different actions, or a press
    //    binding sits on a button that also carries a timed gesture. Press
    //    fires on the down edge, so it always beats and doubles up with the
    //    gesture that would have resolved later.
    return Kind::HardConflict;
}

BindingRelation::Notice BindingRelation::noticeFor(const BindingResolver::Binding& left,
                                                   const BindingResolver::Binding& right)
{
    if (classify(left, right) == Kind::None)
        return Notice::None;

    const TriggerSpec leftTrigger = left.trigger();
    const TriggerSpec rightTrigger = right.trigger();
    const GestureSpec leftGesture = left.gesture();
    const GestureSpec rightGesture = right.gesture();

    // Chord-start delay outranks the tap notice: it is the longer wait and the
    // more surprising one ("why is my screenshot button slow now?").
    const bool leftIsChord = leftTrigger.isChord();
    const bool rightIsChord = rightTrigger.isChord();
    if (leftIsChord != rightIsChord) {
        const TriggerSpec& chord = leftIsChord ? leftTrigger : rightTrigger;
        const TriggerSpec& single = leftIsChord ? rightTrigger : leftTrigger;
        const GestureSpec& singleGesture = leftIsChord ? rightGesture : leftGesture;
        if (single.firstControl() == chord.firstControl()
            && singleGesture.kind != GestureSpec::Kind::Press)
            return Notice::ChordStartDelay;
        return Notice::None;
    }

    // A lower tap count on the same button waits out the multi-tap window only
    // because a higher count exists — that is exactly what to tell the user.
    if (!leftIsChord && leftTrigger == rightTrigger
        && leftGesture.kind == GestureSpec::Kind::Tap
        && rightGesture.kind == GestureSpec::Kind::Tap
        && leftGesture.tapCount != rightGesture.tapCount) {
        return Notice::HigherTapCountDelay;
    }
    return Notice::None;
}

QString BindingRelation::noticeText(Notice notice, int chordWindowMs, int multiTapIntervalMs)
{
    switch (notice) {
    case Notice::HigherTapCountDelay:
        return NativeText::get(
            //: Binding notice; %1 is a locale-formatted millisecond count.
            //% "This action waits up to %1 ms because the same button also has an assignment that needs more taps."
            QT_TRID_NOOP("gamehq.input.relation.higher_tap_delay"),
            "This action waits up to %1 ms because the same button also has an assignment that needs more taps.")
            .arg(QLocale().toString(multiTapIntervalMs));
    case Notice::ChordStartDelay:
        return NativeText::get(
            //: Binding notice; %1 is a locale-formatted millisecond count.
            //% "This action waits up to %1 ms because this button starts a combination."
            QT_TRID_NOOP("gamehq.input.relation.chord_start_delay"),
            "This action waits up to %1 ms because this button starts a combination.")
            .arg(QLocale().toString(chordWindowMs));
    case Notice::None:
        break;
    }
    return {};
}

QString BindingRelation::kindId(Kind kind)
{
    switch (kind) {
    case Kind::None: return QStringLiteral("none");
    case Kind::ContextOverride: return QStringLiteral("context_override");
    case Kind::Redundant: return QStringLiteral("redundant");
    case Kind::SharedGesture: return QStringLiteral("shared_gesture");
    case Kind::ConversionRequired: return QStringLiteral("conversion_required");
    case Kind::HardConflict: return QStringLiteral("hard_conflict");
    }
    return QStringLiteral("none");
}

QString BindingRelation::noticeId(Notice notice)
{
    switch (notice) {
    case Notice::None: return QStringLiteral("none");
    case Notice::HigherTapCountDelay: return QStringLiteral("higher_tap_count_delay");
    case Notice::ChordStartDelay: return QStringLiteral("chord_start_delay");
    }
    return QStringLiteral("none");
}
