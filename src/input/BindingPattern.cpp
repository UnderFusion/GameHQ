#include "input/BindingPattern.h"

#include "localization/NativeText.h"

namespace {

const QLatin1String kPress("press");
const QLatin1String kTap("tap");
const QLatin1String kDoubleTap("double_tap");
const QLatin1String kHold("hold");
const QLatin1Char kChordSeparator('>');

void fail(QString* error, const QString& text)
{
    if (error)
        *error = text;
}

} // namespace

// ---------------------------------------------------------------- GestureSpec

GestureSpec GestureSpec::press()
{
    return {};
}

GestureSpec GestureSpec::tap(int count)
{
    return {Kind::Tap, count, 0};
}

GestureSpec GestureSpec::hold(int milliseconds)
{
    return {Kind::Hold, 1, milliseconds};
}

bool GestureSpec::isValid(QString* error) const
{
    switch (kind) {
    case Kind::Press:
        if (tapCount != 1 || holdMs != 0) {
            fail(error, QStringLiteral("press carries no tap count or hold duration"));
            return false;
        }
        return true;
    case Kind::Tap:
        if (tapCount < 1 || tapCount > kMaxTapCount) {
            fail(error, QStringLiteral("tap count %1 is outside 1..%2")
                            .arg(tapCount).arg(kMaxTapCount));
            return false;
        }
        if (holdMs != 0) {
            fail(error, QStringLiteral("tap carries no hold duration"));
            return false;
        }
        return true;
    case Kind::Hold:
        if (tapCount != 1) {
            fail(error, QStringLiteral("hold carries no tap count"));
            return false;
        }
        if (holdMs < 0) {
            fail(error, QStringLiteral("hold duration %1 is negative").arg(holdMs));
            return false;
        }
        return true;
    }
    fail(error, QStringLiteral("unknown gesture kind"));
    return false;
}

QString GestureSpec::activationCode() const
{
    switch (kind) {
    case Kind::Press: return kPress;
    case Kind::Tap:   return kTap;
    case Kind::Hold:  return kHold;
    }
    return kPress;
}

bool GestureSpec::hasLegacyActivation() const
{
    return kind != Kind::Tap || tapCount <= 2;
}

QString GestureSpec::legacyActivationCode() const
{
    if (kind == Kind::Tap && tapCount == 2)
        return kDoubleTap;
    return activationCode();
}

QString GestureSpec::label() const
{
    switch (kind) {
    case Kind::Press:
        //: Name of the immediate button-down gesture in shortcut summaries.
        //% "Press"
        return NativeText::get(QT_TRID_NOOP("gamehq.input.gesture.press"), "Press");
    case Kind::Tap:
        if (tapCount == 2) {
            //: Name of the two-tap gesture in shortcut summaries.
            //% "Double tap"
            return NativeText::get(QT_TRID_NOOP("gamehq.input.gesture.double_tap"),
                                   "Double tap");
        }
        if (tapCount == 3) {
            //: Name of the three-tap gesture in shortcut summaries.
            //% "Triple tap"
            return NativeText::get(QT_TRID_NOOP("gamehq.input.gesture.triple_tap"),
                                   "Triple tap");
        }
        //: Name of the single-tap gesture in shortcut summaries.
        //% "Tap"
        return NativeText::get(QT_TRID_NOOP("gamehq.input.gesture.tap"), "Tap");
    case Kind::Hold:
        //: Name of the held-button gesture in shortcut summaries.
        //% "Hold"
        return NativeText::get(QT_TRID_NOOP("gamehq.input.gesture.hold"), "Hold");
    }
    //: Name of the immediate button-down gesture in shortcut summaries.
    //% "Press"
    return NativeText::get(QT_TRID_NOOP("gamehq.input.gesture.press"), "Press");
}

GestureSpec::ParseResult GestureSpec::parse(const QString& activation, int tapCount, int holdMs)
{
    ParseResult result;
    GestureSpec gesture;

    if (activation.isEmpty() || activation == kPress) {
        gesture = {Kind::Press, tapCount, holdMs};
    } else if (activation == kTap) {
        gesture = {Kind::Tap, tapCount, holdMs};
    } else if (activation == kDoubleTap) {
        // A row from a schema without the tap-count column reports 1; that is
        // "unset", not "one tap", and double_tap already says the count.
        if (tapCount != 1 && tapCount != 2) {
            result.error = QStringLiteral("double_tap cannot carry tap count %1").arg(tapCount);
            return result;
        }
        gesture = {Kind::Tap, 2, holdMs};
    } else if (activation == kHold) {
        gesture = {Kind::Hold, tapCount, holdMs};
    } else {
        result.error = QStringLiteral("unknown activation '%1'").arg(activation);
        return result;
    }

    if (!gesture.isValid(&result.error))
        return result;
    result.ok = true;
    result.gesture = gesture;
    return result;
}

// ---------------------------------------------------------------- TriggerSpec

QString TriggerSpec::chordNamespace()
{
    return QStringLiteral("chord:");
}

QString TriggerSpec::chordPrefixV1()
{
    return QStringLiteral("chord:v1:");
}

TriggerSpec TriggerSpec::single(const QString& control)
{
    return {Kind::Single, {control}};
}

TriggerSpec TriggerSpec::orderedChord(const QString& first, const QString& second)
{
    return {Kind::OrderedChord, {first, second}};
}

QString TriggerSpec::firstControl() const
{
    return controls.isEmpty() ? QString() : controls.first();
}

QString TriggerSpec::secondControl() const
{
    return controls.size() > 1 ? controls.at(1) : QString();
}

bool TriggerSpec::isValid(QString* error) const
{
    switch (kind) {
    case Kind::Single:
        if (controls.size() != 1 || controls.first().isEmpty()) {
            fail(error, QStringLiteral("a single trigger needs exactly one control"));
            return false;
        }
        // Round-trip safety: serializing a control that starts with "chord:"
        // would produce a string this parser reads back as a chord.
        if (controls.first().startsWith(chordNamespace())) {
            fail(error, QStringLiteral("'%1' uses the reserved chord namespace")
                            .arg(controls.first()));
            return false;
        }
        return true;
    case Kind::OrderedChord:
        if (controls.size() != 2) {
            fail(error, QStringLiteral("an ordered chord needs exactly two controls"));
            return false;
        }
        if (controls.at(0) == controls.at(1)) {
            fail(error, QStringLiteral("an ordered chord needs two different controls"));
            return false;
        }
        for (const QString& control : controls) {
            // Chord recognition works off canonical control ids, which is what
            // makes one saved chord behave the same on Sony HID, XInput and
            // WinMM. An opaque code has no such guarantee.
            if (!ControlId::isCanonical(control)) {
                fail(error, QStringLiteral("'%1' is not a canonical controller control")
                                .arg(control.isEmpty() ? QStringLiteral("(empty)") : control));
                return false;
            }
        }
        return true;
    }
    fail(error, QStringLiteral("unknown trigger kind"));
    return false;
}

QString TriggerSpec::serialize() const
{
    if (kind == Kind::OrderedChord && controls.size() == 2)
        return chordPrefixV1() + controls.at(0) + kChordSeparator + controls.at(1);
    return firstControl();
}

QString TriggerSpec::label(ControlId::ControllerFamily family) const
{
    if (kind == Kind::OrderedChord && controls.size() == 2) {
        return QStringLiteral("%1 + %2").arg(ControlId::label(controls.at(0), family),
                                             ControlId::label(controls.at(1), family));
    }
    return ControlId::label(firstControl(), family);
}

TriggerSpec::ParseResult TriggerSpec::parse(const QString& triggerCode)
{
    ParseResult result;
    if (triggerCode.isEmpty()) {
        result.error = QStringLiteral("empty trigger code");
        return result;
    }

    TriggerSpec trigger;
    if (triggerCode.startsWith(chordNamespace())) {
        // Fail closed on the whole namespace: a build that does not know
        // "chord:v2:" must reject it, never approximate it.
        if (!triggerCode.startsWith(chordPrefixV1())) {
            result.error = QStringLiteral("unsupported trigger serialization '%1'")
                               .arg(triggerCode.left(16));
            return result;
        }
        const QString payload = triggerCode.mid(chordPrefixV1().size());
        const QStringList parts = payload.split(kChordSeparator);
        if (parts.size() != 2) {
            result.error = QStringLiteral("chord needs exactly two controls, got %1")
                               .arg(parts.size());
            return result;
        }
        trigger = orderedChord(parts.at(0), parts.at(1));
    } else {
        // Everything written before patterns existed lands here verbatim.
        trigger = single(triggerCode);
    }

    if (!trigger.isValid(&result.error))
        return result;
    result.ok = true;
    result.trigger = trigger;
    return result;
}

// -------------------------------------------------------------- BindingPattern

bool BindingPattern::isValid(const QString& deviceGroup, QString* error) const
{
    if (!trigger.isValid(error) || !gesture.isValid(error))
        return false;
    if (!trigger.isChord())
        return true;

    if (deviceGroup != QLatin1String("controller")) {
        fail(error, QStringLiteral("chords are controller-only, not '%1'").arg(deviceGroup));
        return false;
    }
    // Version 1 chords fire on the second control's down edge. Layering a tap
    // count or a hold on top of an already two-stage trigger multiplies the
    // waiting windows, so it stays out until there is a reason for it.
    if (gesture.kind != GestureSpec::Kind::Press) {
        fail(error, QStringLiteral("a chord only supports the press gesture"));
        return false;
    }
    return true;
}

BindingPattern::ParseResult BindingPattern::parse(const QString& deviceGroup,
                                                  const QString& triggerCode,
                                                  const QString& activation,
                                                  int tapCount, int holdMs)
{
    ParseResult result;
    const TriggerSpec::ParseResult trigger = TriggerSpec::parse(triggerCode);
    if (!trigger.ok) {
        result.error = trigger.error;
        return result;
    }
    const GestureSpec::ParseResult gesture = GestureSpec::parse(activation, tapCount, holdMs);
    if (!gesture.ok) {
        result.error = gesture.error;
        return result;
    }

    BindingPattern pattern{trigger.trigger, gesture.gesture};
    if (!pattern.isValid(deviceGroup, &result.error))
        return result;
    result.ok = true;
    result.pattern = pattern;
    return result;
}
