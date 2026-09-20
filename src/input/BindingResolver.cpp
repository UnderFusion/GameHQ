#include "input/BindingResolver.h"

#include "input/BindingPattern.h"
#include "input/ContextOverrideCatalog.h"
#include "input/ControlId.h"
#include "input/DefaultBindings.h"
#include "input/OverlayInputPolicy.h"
#include "input/InputDiagnostics.h"
#include "storage/CaptureDatabase.h"

#include <QDebug>
#include <QHash>
#include <QSet>
#include <algorithm>
#include <utility>

namespace {
QString bindingKey(const QString& actionId, int slot)
{
    return actionId + QLatin1Char('#') + QString::number(slot);
}

// The one place a persisted row becomes a live binding, and therefore the one
// place that decides whether it may execute at all. A row that does not form a
// valid pattern — a chord serialization this build does not know, two of the
// same control, a gesture whose parts contradict each other, a value corrupted
// outside the app — is skipped here, so the action falls back to its default
// instead of firing something nobody asked for.
bool validateRow(const BindingOverrideRow& row, QString* error)
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
}

BindingResolver::BindingResolver(CaptureDatabase* database)
    : m_database(database)
{
}

void BindingResolver::setDefaultHoldMs(int milliseconds)
{
    m_defaultHoldMs = qBound(250, milliseconds, 10000);
}

bool BindingResolver::setProfileAlias(const QString& profile, const QString& legacyProfile)
{
    return setProfileAliases(profile, legacyProfile.isEmpty() ? QStringList{}
                                                              : QStringList{legacyProfile});
}

bool BindingResolver::setProfileAliases(const QString& profile,
                                        const QStringList& legacyProfiles)
{
    if (profile.isEmpty())
        return false;
    QStringList aliases;
    for (const QString& alias : legacyProfiles) {
        if (!alias.isEmpty() && alias != profile && !aliases.contains(alias))
            aliases.append(alias);
    }
    // Report whether the effective view actually moved: callers re-observe the
    // same attachment on every press, and pretending that changed anything
    // would let them invalidate in-flight gestures on each mirrored event.
    if (m_profileAliases.value(profile) == aliases)
        return false;
    if (aliases.isEmpty())
        m_profileAliases.remove(profile);
    else
        m_profileAliases.insert(profile, aliases);
    return true;
}

void BindingResolver::reload()
{
    m_overrides.clear();
    if (!m_database)
        return;
    for (const BindingOverrideRow& row : m_database->listBindingOverrides()) {
        QString error;
        if (!validateRow(row, &error)) {
            const QString subject = QStringLiteral("%1 slot %2 (%3)")
                                        .arg(row.actionId).arg(row.slot).arg(row.deviceGroup);
            qWarning() << "Bindings: skipping stored override" << subject << "—" << error;
            InputDiagnostics::instance().noteRejectedBinding(subject, error);
            continue;
        }
        m_overrides.append({row.deviceGroup, row.deviceProfile, row.actionId,
                            row.slot, row.triggerCode, row.activation,
                            row.holdMs, row.unbound, row.tapCount});
    }
}

// A capture into an empty slot must not invent semantics. The slot's meaning is
// whatever the user or the defaults last gave it, resolved in a fixed order:
// the slot's own override row (a cleared row keeps carrying its old gesture),
// the default binding for that exact slot, another live slot of the same
// action, any default of the same action, and only then plain press. Without
// this, every empty-slot capture landed as "press", which the relation policy
// rightly treats as clashing with any tap/hold on the same trigger — so valid
// assignments were blocked by conflicts that only existed because of the guess.
BindingResolver::Gesture BindingResolver::inheritedGesture(
    const QString& deviceGroup, const QString& deviceProfile,
    const QString& actionId, int slot) const
{
    // 1) The slot's own override row, unbound included — a device-specific row
    //    wins over a group-wide one, same precedence as effectiveBindings().
    //    An unbound press/0 row is the pre-0.7.3 clear sentinel, written when
    //    clearing still erased the gesture; it carries no information, so it
    //    falls through instead of resurrecting "press".
    const Binding* overrideRow = nullptr;
    for (const Binding& binding : m_overrides) {
        if (binding.deviceGroup != deviceGroup || binding.actionId != actionId
            || binding.slot != slot)
            continue;
        if (!binding.deviceProfile.isEmpty() && binding.deviceProfile != deviceProfile)
            continue;
        if (!overrideRow || (overrideRow->deviceProfile.isEmpty()
                             && !binding.deviceProfile.isEmpty()))
            overrideRow = &binding;
    }
    if (overrideRow
        && !(overrideRow->unbound && overrideRow->activation == QLatin1String("press")
             && overrideRow->holdMs == 0))
        return {overrideRow->activation, overrideRow->holdMs, overrideRow->tapCount};

    // 2) The default binding for this exact slot.
    const QVector<Binding> defaults = defaultBindings();
    for (const Binding& binding : defaults) {
        if (binding.deviceGroup == deviceGroup && binding.actionId == actionId
            && binding.slot == slot)
            return {binding.activation, binding.holdMs, binding.tapCount};
    }

    // 3) Another live slot of the same action, lowest slot first.
    const QVector<Binding> effective = effectiveBindings(deviceGroup, deviceProfile);
    const Binding* sibling = nullptr;
    for (const Binding& binding : effective) {
        if (binding.actionId != actionId || binding.slot == slot)
            continue;
        if (!sibling || binding.slot < sibling->slot)
            sibling = &binding;
    }
    if (sibling)
        return {sibling->activation, sibling->holdMs, sibling->tapCount};

    // 4) The action's default semantics: this device group first, then any.
    for (const Binding& binding : defaults) {
        if (binding.deviceGroup == deviceGroup && binding.actionId == actionId)
            return {binding.activation, binding.holdMs, binding.tapCount};
    }
    for (const Binding& binding : defaults) {
        if (binding.actionId == actionId)
            return {binding.activation, binding.holdMs, binding.tapCount};
    }

    // 5) Nothing anywhere has ever given this slot a meaning.
    return {};
}

// The shipped table lives in DefaultBindings.cpp (pure, database-free) so the
// overlay/desktop/playback routing contract can be audited without a device;
// this is the only production reader.
QVector<BindingResolver::Binding> BindingResolver::defaultBindings()
{
    return gamehqDefaultBindings();
}

QVector<BindingResolver::Binding> BindingResolver::effectiveBindings(
    const QString& deviceGroup, const QString& deviceProfile) const
{
    QHash<QString, Binding> merged;
    for (const Binding& binding : defaultBindings()) {
        if (binding.deviceGroup == deviceGroup)
            merged.insert(bindingKey(binding.actionId, binding.slot), binding);
    }

    // Precedence, later wins: group-wide < legacy alias (pre-identity
    // "xinput.slotN" rows kept alive for this profile) < device-specific.
    QStringList profiles{QString()};
    if (!deviceProfile.isEmpty()) {
        profiles.append(m_profileAliases.value(deviceProfile));
        profiles.append(deviceProfile);
    }
    for (const QString& profile : profiles) {
        for (const Binding& binding : m_overrides) {
            if (binding.deviceGroup != deviceGroup || binding.deviceProfile != profile)
                continue;
            const auto* action = ActionCatalog::find(binding.actionId);
            if (!action || !action->bindable)
                continue;
            merged.insert(bindingKey(binding.actionId, binding.slot), binding);
        }
    }

    QVector<Binding> result;
    for (const Binding& binding : std::as_const(merged)) {
        if (!binding.unbound && !binding.triggerCode.isEmpty())
            result.append(binding);
    }
    return result;
}

QVector<BindingResolver::Binding> BindingResolver::baselineBindings(
    const QString& deviceGroup, const QString& deviceProfile) const
{
    QHash<QString, Binding> merged;
    for (const Binding& binding : defaultBindings()) {
        if (binding.deviceGroup == deviceGroup)
            merged.insert(bindingKey(binding.actionId, binding.slot), binding);
    }

    // A shared profile compares directly with code-owned defaults. A specific
    // controller compares with everything below its own identity: group-wide
    // rows first, then its legacy slot alias when one exists.
    if (!deviceProfile.isEmpty()) {
        QStringList inheritedProfiles{QString()};
        inheritedProfiles.append(m_profileAliases.value(deviceProfile));
        for (const QString& profile : inheritedProfiles) {
            for (const Binding& binding : m_overrides) {
                if (binding.deviceGroup != deviceGroup || binding.deviceProfile != profile)
                    continue;
                const auto* action = ActionCatalog::find(binding.actionId);
                if (!action || !action->bindable)
                    continue;
                merged.insert(bindingKey(binding.actionId, binding.slot), binding);
            }
        }
    }

    QVector<Binding> result;
    for (const Binding& binding : std::as_const(merged)) {
        if (!binding.unbound && !binding.triggerCode.isEmpty())
            result.append(binding);
    }
    return result;
}

QVector<BindingResolver::Binding> BindingResolver::matching(
    const QString& deviceGroup, const QString& deviceProfile,
    const QString& triggerCode, const GestureSpec& gesture,
    ActionCatalog::Scope primaryScope, ActionCatalog::Scope fallbackScope) const
{
    // Rows that can fire for the pressed trigger, with their action resolved
    // once. The scope/gesture/substitution arbitration itself lives in
    // OverlayInput::select (input/OverlayInputPolicy.cpp) so the overlay's
    // routing contract is testable without a database or a device.
    const QVector<Binding> bindings = effectiveBindings(deviceGroup, deviceProfile);
    QVector<const Binding*> rows;
    QVector<OverlayInput::Candidate> candidates;
    for (const Binding& binding : bindings) {
        if (binding.triggerCode != triggerCode)
            continue;
        const ActionCatalog::Action* action = ActionCatalog::find(binding.actionId);
        if (!action)
            continue;
        rows.append(&binding);
        candidates.append({binding.actionId, action->scope, binding.gesture()});
    }

    const OverlayInput::Selection selection =
        OverlayInput::select(candidates, gesture, primaryScope, fallbackScope);

    QVector<Binding> result;
    result.reserve(selection.deliveredCount());
    for (int index : selection.globalIndices)
        result.append(*rows.at(index));
    for (int index : selection.contextualIndices)
        result.append(*rows.at(index));
    return result;
}
