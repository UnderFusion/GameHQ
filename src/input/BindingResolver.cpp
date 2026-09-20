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
    // Delegates to the storage-side canonical validator so the v9 migration
    // and the runtime judge a stored row at exactly one parse boundary.
    return CaptureDatabase::isValidBindingOverrideRow(row, error);
}
}

bool BindingResolver::validateOverrideRow(const BindingOverrideRow& row, QString* error)
{
    return validateRow(row, error);
}

QStringList BindingResolver::chainLayers(const QString& profile, const QStringList& aliases)
{
    QStringList layers{QString()};
    if (profile.isEmpty())
        return layers;
    for (const QString& alias : aliases) {
        if (!alias.isEmpty() && alias != profile && !layers.contains(alias))
            layers.append(alias);
    }
    layers.append(profile);
    return layers;
}

QVector<BindingResolver::Binding> BindingResolver::mergedTable(const QVector<Binding>& defaults,
                                                               const QVector<Binding>& overrides,
                                                               const QString& deviceGroup,
                                                               const QStringList& layers)
{
    QHash<QString, Binding> merged;
    for (const Binding& binding : defaults) {
        if (binding.deviceGroup == deviceGroup)
            merged.insert(bindingKey(binding.actionId, binding.slot), binding);
    }
    for (const QString& profile : layers) {
        for (const Binding& binding : overrides) {
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

QVector<MappingPresetRow> BindingResolver::foldChainRows(const QVector<Binding>& overrides,
                                                         const QString& deviceGroup,
                                                         const QString& profile,
                                                         const QStringList& aliases,
                                                         QStringList* contributingLayers)
{
    const QStringList layers = chainLayers(profile, aliases);
    QHash<QString, int> slotOf;   // action#slot -> index into `fold`
    QVector<MappingPresetRow> fold;
    QSet<QString> contributors;   // layers with at least one storable row

    for (const QString& layer : layers) {
        for (const Binding& binding : overrides) {
            if (binding.deviceGroup != deviceGroup || binding.deviceProfile != layer)
                continue;
            const auto* action = ActionCatalog::find(binding.actionId);
            if (!action || !action->bindable)
                continue;
            const QString key = bindingKey(binding.actionId, binding.slot);
            MappingPresetRow row;
            row.actionId = binding.actionId;
            row.slot = binding.slot;
            // A row with no trigger means "no trigger" in the preset model too:
            // it must keep suppressing the default it shadowed. The old model
            // allowed an empty trigger with unbound still false (the clear
            // sentinel); the stored content always spells that out as unbound.
            row.unbound = binding.unbound || binding.triggerCode.isEmpty();
            row.triggerCode = row.unbound ? QString() : binding.triggerCode;
            row.activation = binding.activation;
            row.holdMs = binding.holdMs;
            row.tapCount = binding.tapCount;
            if (!CaptureDatabase::isValidMappingPresetRow(deviceGroup, row)) {
                // Cannot be stored as preset content; the row stays in
                // binding_overrides and keeps legacy resolution. Equality will
                // simply never prove for this chain.
                qWarning() << "Bindings: migration cannot store override"
                           << bindingKey(binding.actionId, binding.slot) << "for profile"
                           << (layer.isEmpty() ? QStringLiteral("<group>") : layer);
                continue;
            }
            contributors.insert(layer);
            const auto existing = slotOf.constFind(key);
            if (existing != slotOf.cend())
                fold[*existing] = row;
            else {
                slotOf.insert(key, fold.size());
                fold.append(row);
            }
        }
    }

    std::sort(fold.begin(), fold.end(), [](const MappingPresetRow& a, const MappingPresetRow& b) {
        if (a.actionId != b.actionId)
            return a.actionId < b.actionId;
        return a.slot < b.slot;
    });

    if (contributingLayers) {
        contributingLayers->clear();
        for (const QString& layer : layers) {
            if (contributors.contains(layer))
                contributingLayers->append(layer);
        }
    }
    return fold;
}

QVector<BindingResolver::Binding> BindingResolver::chainTableFromRows(
    const QVector<Binding>& defaults, const QString& deviceGroup, const QString& profile,
    const QVector<MappingPresetRow>& rows)
{
    // Mirrors mergedTable()'s semantics exactly (defaults first, rows after,
    // bindable actions only, unbound rows suppress their default, bound rows
    // only) - but the rows are profile-less, so they are labeled with the
    // chain key instead of a stored layer name.
    QHash<QString, Binding> merged;
    for (const Binding& binding : defaults) {
        if (binding.deviceGroup == deviceGroup)
            merged.insert(bindingKey(binding.actionId, binding.slot), binding);
    }
    for (const MappingPresetRow& row : rows) {
        if (!CaptureDatabase::isValidMappingPresetRow(deviceGroup, row))
            continue;
        const auto* action = ActionCatalog::find(row.actionId);
        if (!action || !action->bindable)
            continue;
        Binding binding;
        binding.deviceGroup = deviceGroup;
        binding.deviceProfile = profile;
        binding.actionId = row.actionId;
        binding.slot = row.slot;
        binding.triggerCode = row.triggerCode;
        binding.activation = row.activation;
        binding.holdMs = row.holdMs;
        binding.unbound = row.unbound;
        binding.tapCount = row.tapCount;
        merged.insert(bindingKey(binding.actionId, binding.slot), binding);
    }

    QVector<Binding> result;
    for (const Binding& binding : std::as_const(merged)) {
        if (!binding.unbound && !binding.triggerCode.isEmpty())
            result.append(binding);
    }
    return result;
}

bool BindingResolver::chainTablesEqual(const QVector<Binding>& a, const QVector<Binding>& b)
{
    if (a.size() != b.size())
        return false;
    QHash<QString, const Binding*> index;
    for (const Binding& binding : b)
        index.insert(bindingKey(binding.actionId, binding.slot), &binding);
    for (const Binding& binding : a) {
        const auto hit = index.constFind(bindingKey(binding.actionId, binding.slot));
        if (hit == index.cend())
            return false;
        const Binding& other = *hit.value();
        if (other.deviceGroup != binding.deviceGroup
            || other.triggerCode != binding.triggerCode
            || other.activation != binding.activation
            || other.holdMs != binding.holdMs
            || other.tapCount != binding.tapCount
            || other.unbound != binding.unbound)
            return false;
    }
    return true;
}

QString BindingResolver::materializedKey(const QString& deviceGroup, const QString& profile)
{
    return deviceGroup + QLatin1Char('\x1f') + profile;
}

bool BindingResolver::activateMaterializedChain(const QString& deviceGroup, const QString& profile,
                                                const QString& presetId,
                                                const QVector<MappingPresetRow>& rows,
                                                const QStringList& aliases)
{
    if (deviceGroup.isEmpty()
        || (profile.isEmpty() && deviceGroup == QLatin1String("controller")))
        return false;
    MaterializedView view;
    view.presetId = presetId;
    view.aliases = aliases;
    view.table = chainTableFromRows(defaultBindings(), deviceGroup, profile, rows);
    m_materialized.insert(materializedKey(deviceGroup, profile), view);
    ++m_revision;
    return true;
}

void BindingResolver::deactivateMaterializedChain(const QString& deviceGroup, const QString& profile)
{
    if (m_materialized.remove(materializedKey(deviceGroup, profile)) > 0)
        ++m_revision;
}

bool BindingResolver::isMaterialized(const QString& deviceGroup, const QString& profile) const
{
    return m_materialized.contains(materializedKey(deviceGroup, profile));
}

QString BindingResolver::materializedPreset(const QString& deviceGroup, const QString& profile) const
{
    const auto view = m_materialized.constFind(materializedKey(deviceGroup, profile));
    return view == m_materialized.cend() ? QString() : view->presetId;
}

void BindingResolver::setPresetTable(const QString& deviceGroup, const QString& deviceProfile,
                                     const QString& presetId, const QString& source,
                                     const QVector<Binding>& table)
{
    PresetTableView view;
    view.presetId = presetId;
    view.source = source;
    view.table = table;
    m_presetTables.insert(materializedKey(deviceGroup, deviceProfile), view);
    ++m_revision;
}

void BindingResolver::clearPresetTable(const QString& deviceGroup, const QString& deviceProfile)
{
    if (m_presetTables.remove(materializedKey(deviceGroup, deviceProfile)) > 0)
        ++m_revision;
}

bool BindingResolver::hasPresetTable(const QString& deviceGroup, const QString& deviceProfile) const
{
    return m_presetTables.contains(materializedKey(deviceGroup, deviceProfile));
}

QString BindingResolver::presetTableId(const QString& deviceGroup, const QString& deviceProfile) const
{
    const auto view = m_presetTables.constFind(materializedKey(deviceGroup, deviceProfile));
    return view == m_presetTables.cend() ? QString() : view->presetId;
}

QString BindingResolver::presetTableSource(const QString& deviceGroup, const QString& deviceProfile) const
{
    const auto view = m_presetTables.constFind(materializedKey(deviceGroup, deviceProfile));
    return view == m_presetTables.cend() ? QString() : view->source;
}

QStringList BindingResolver::installedPresetChainKeys() const
{
    return m_presetTables.keys();
}

QVector<BindingResolver::Binding> BindingResolver::inheritedTable(
    const QString& deviceGroup, const QString& deviceProfile) const
{
    const auto view = m_materialized.constFind(materializedKey(deviceGroup, deviceProfile));
    if (view != m_materialized.cend())
        return view->table;
    return mergedTable(defaultBindings(), m_overrides, deviceGroup,
                       chainLayers(deviceProfile, m_profileAliases.value(deviceProfile)));
}

bool BindingResolver::hasUnretiredSpecificLegacy(const QString& deviceGroup,
                                                 const QString& deviceProfile) const
{
    if (m_overrides.isEmpty())
        return false;
    // A chain resolves through group-wide rows first, then its alias layers,
    // then its own profile. Only the latter two are device-specific behavior
    // that the migration bridge exists to protect.
    QSet<QString> specific;
    const QStringList layers = chainLayers(deviceProfile, m_profileAliases.value(deviceProfile));
    for (const QString& layer : layers) {
        if (!layer.isEmpty())
            specific.insert(layer);
    }
    if (specific.isEmpty())
        return false;
    for (const Binding& binding : m_overrides) {
        if (binding.deviceGroup == deviceGroup && specific.contains(binding.deviceProfile))
            return true;
    }
    return false;
}

bool BindingResolver::hasRetainedLocalRows(const QString& deviceGroup,
                                           const QString& deviceProfile) const
{
    if (m_overrides.isEmpty())
        return false;
    // Every layer the chain resolves through counts here, the group-wide layer
    // included: these rows are still live behavior, they are simply not a
    // preset. `specific` (alias + own profile) is what makes a chain
    // bridge-owned; this method is the wider question "is anything local left".
    const QStringList layers = chainLayers(deviceProfile, m_profileAliases.value(deviceProfile));
    for (const Binding& binding : m_overrides) {
        if (binding.deviceGroup == deviceGroup && layers.contains(binding.deviceProfile))
            return true;
    }
    return false;
}

// Keyboard and mouse have no per-device identity layer, so their migrated
// group-default content may become the active resolution once it is proven
// equal to today's group-wide table. Controllers deliberately never activate
// here: a controller group default existing in the database must not move any
// pad off the legacy chain before that pad's own chain is materialized.
void BindingResolver::rebuildGroupChains()
{
    if (!m_database)
        return;
    for (const QString& group :
         {QStringLiteral("keyboard"), QStringLiteral("mouse")}) {
        deactivateMaterializedChain(group, QString());
        QString presetId;
        for (const MappingAssignment& assignment : m_database->listMappingAssignments(group)) {
            if (assignment.targetKind == QLatin1String("group_default"))
                presetId = assignment.presetId;
        }
        if (presetId.isEmpty())
            continue;
        const MappingPreset preset = m_database->mappingPreset(presetId);
        if (preset.id.isEmpty() || preset.origin != QLatin1String("migration"))
            continue;   // user content is not activated by the migration bridge
        const QVector<MappingPresetRow> rows = m_database->mappingPresetRows(presetId);
        const QVector<Binding> legacy = mergedTable(defaultBindings(), m_overrides, group,
                                                    chainLayers(QString(), {}));
        const QVector<Binding> candidate = chainTableFromRows(defaultBindings(), group,
                                                              QString(), rows);
        if (!chainTablesEqual(legacy, candidate)) {
            qWarning() << "Bindings: group default preset for" << group
                       << "does not match the stored group rows; keeping legacy resolution";
            continue;
        }
        activateMaterializedChain(group, QString(), presetId, rows, {});
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
    // The chain just changed; a view proven against the old alias list may not
    // execute against the new one (section 6: revalidate, never trust a stale
    // proof). Views for other profiles are untouched.
    for (auto it = m_materialized.begin(); it != m_materialized.end();) {
        const int split = it.key().indexOf(QLatin1Char('\x1f'));
        const QString keyProfile = split >= 0 ? it.key().mid(split + 1) : QString();
        if (keyProfile == profile && it.value().aliases != aliases)
            it = m_materialized.erase(it);
        else
            ++it;
    }
    ++m_revision;
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
    // A reload means the stored rows changed: every materialized chain was
    // proven against the old bytes, so all views drop and the materializer has
    // to re-prove per attach. Group-wide chains (keyboard/mouse) re-verify
    // here, because they have no attach-time identity to wait for.
    m_materialized.clear();
    ++m_revision;
    rebuildGroupChains();
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
    // A preset installed by the cpo-p05 mapping switch serves this chain whole;
    // below it, a proven chain serves its materialized preset table and every
    // other chain - and every keyboard/mouse group whose default has not been
    // proven - keeps the legacy merge, byte for byte.
    const auto preset = m_presetTables.constFind(materializedKey(deviceGroup, deviceProfile));
    if (preset != m_presetTables.cend())
        return preset->table;
    return inheritedTable(deviceGroup, deviceProfile);
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
