// InputEngine: resolved mapping preset switching (cpo-p05). Split out of
// InputEngine.cpp; the class itself is declared in InputEngine.h.
#include "input/InputEngine.h"

#include "input/BindingEditorModel.h"
#include "input/BindingRuntime.h"
#include "input/InputDiagnostics.h"
#include "input/MappingPresetMaterializer.h"
#include "input/MappingPresetModel.h"
#include "input/SteamInputAdvice.h"
#include "storage/CaptureDatabase.h"

#include <QDebug>
#include <QSet>

// ---------------------------------------------------------------------- cpo-p05
// Switching the resolved mapping at a safe input boundary.
//
// The invariant: changing the resolved preset must never let an input that
// began under the old mapping complete under the new one, and a refresh that
// resolves to the same winner with the same content must not disturb an active
// gesture at all. The order below is the whole guarantee:
//   prepare -> compare -> snapshot -> invalidate -> publish -> gate.

QString InputEngine::mappingChainKey(const QString& group, const QString& profile) const
{
    return group + QChar(0x1f) + profile;
}

QVector<MappingAssignmentResolver::IdentityCandidate> InputEngine::controllerIdentityChain(
    const QString& profile) const
{
    QVector<MappingAssignmentResolver::IdentityCandidate> chain;
    if (profile.isEmpty())
        return chain;
    // Provenance, not syntax: only a registry identity with strong confidence
    // is presented as a persisted "controller" target. A weak or session-local
    // logical id is a compatibility key and is presented as "legacy_slot".
    const auto* logical = m_providers.registry().controller(profile);
    const bool durable = logical
        && logical->confidence == ModernInput::IdentityConfidence::Strong;
    chain.append({durable ? QStringLiteral("controller") : QStringLiteral("legacy_slot"),
                  profile});
    const QStringList aliases = m_runtime->resolver().aliasesFor(profile);
    for (const QString& alias : aliases) {
        if (alias.isEmpty() || alias == profile)
            continue;
        chain.append({QStringLiteral("legacy_slot"), alias});
    }
    return chain;
}

InputEngine::PlannedChain InputEngine::planMappingChain(const QString& group,
                                                        const QString& profile) const
{
    PlannedChain plan;
    plan.group = group;
    plan.profile = profile;

    const MappingAssignmentResolver::Resolution resolution =
        m_assignmentResolver->resolve(group, controllerIdentityChain(profile), m_runningGameKey);

    const bool materialized = m_runtime->resolver().isMaterialized(group, profile);
    // The cpo-p03 migration bridge stays the effective source for a controller
    // chain that still has unretired DEVICE-SPECIFIC legacy rows: the winner
    // layer must not flatten behavior the migration has not proven yet.
    const bool bridgeOwned = group == QLatin1String("controller") && !profile.isEmpty()
        && !materialized && m_runtime->resolver().hasUnretiredSpecificLegacy(group, profile);

    bool presetServes = false;
    if (!resolution.presetId.isEmpty()) {
        const bool explicitWinner = resolution.source == QLatin1String("game")
            || resolution.source == QLatin1String("controller");
        if (explicitWinner) {
            // An explicit assignment always wins, including over an unresolved
            // bridge and over a materialized chain.
            presetServes = true;
        } else if (resolution.source == QLatin1String("group_default")) {
            // Keyboard and mouse group defaults are activated by the migration
            // only when a MIGRATION preset is proven equal to the stored group
            // rows; one that failed that proof keeps legacy resolution, exactly
            // as cpo-p03 left it.
            bool migrationRefuted = false;
            if (group != QLatin1String("controller")) {
                const MappingPreset preset = m_db->mappingPreset(resolution.presetId);
                migrationRefuted = !materialized
                    && preset.origin == QLatin1String("migration");
            }
            presetServes = !bridgeOwned && !materialized && !migrationRefuted;
        }
    }

    if (presetServes) {
        plan.install = true;
        plan.presetId = resolution.presetId;
        plan.source = resolution.source;
        plan.table = BindingResolver::chainTableFromRows(
            BindingResolver::defaultBindings(), group, profile,
            m_db->mappingPresetRows(resolution.presetId));
        plan.fingerprint = BindingRuntime::tableFingerprint(plan.table);
        return plan;
    }

    // No winner preset serves. The effective source below is the honest label
    // of what actually provides this chain's behavior now:
    //  - materialized: the proven cpo-p03 view keeps serving, and a group
    //    default must not flatten it;
    //  - migration_bridge: device-specific legacy rows the migration has not
    //    proven yet keep legacy resolution;
    //  - local_legacy: no assignment, but retained binding_overrides for this
    //    chain still provide behavior - a TRANSITIONAL compatibility source
    //    (the binding editor still writes plain rows; cpo-p06 moves those edits
    //    into presets, and this label retires with it). Retained rows are
    //    recovery evidence, never an invisible permanent layer behind builtin;
    //  - builtin: nothing else applies, so the chain serves shipped defaults
    //    ONLY. The switch layer OWNS an explicitly prepared table for it, pinned
    //    to the code-owned rows instead of to whatever the legacy merge would
    //    produce - which is why ownership can never be inferred from a
    //    non-empty preset id.
    if (materialized || bridgeOwned) {
        plan.install = false;
        plan.source = materialized ? QStringLiteral("materialized")
                                   : QStringLiteral("migration_bridge");
        plan.table = m_runtime->resolver().inheritedTable(group, profile);
        plan.fingerprint = BindingRuntime::tableFingerprint(plan.table);
        return plan;
    }

    if (m_runtime->resolver().hasRetainedLocalRows(group, profile)) {
        plan.install = false;
        plan.source = QStringLiteral("local_legacy");
        plan.table = m_runtime->resolver().inheritedTable(group, profile);
        plan.fingerprint = BindingRuntime::tableFingerprint(plan.table);
        return plan;
    }

    plan.install = true;
    plan.presetId.clear();   // builtin: an owned table with no preset identity
    plan.source = QStringLiteral("builtin");
    plan.table = BindingResolver::chainTableFromRows(BindingResolver::defaultBindings(),
                                                     group, profile, {});
    plan.fingerprint = BindingRuntime::tableFingerprint(plan.table);
    return plan;
}

void InputEngine::setRunningGameKey(const QString& executablePathOrKey)
{
    const QString key = MappingAssignmentResolver::canonicalGameKey(executablePathOrKey);
    if (key == m_runningGameKey)
        return;
    m_runningGameKey = key;
    refreshResolvedPreset();
}

// cpo-p05: the ONE route-ensure seam. The legacy pad path, GameInput and
// selective Raw-HID all call it before dispatching a press, so a route's winner
// is planned and published BEFORE the first press on it can act under an older
// table. It is deliberately cheap after the first registration: an already
// tracked route answers with one hash lookup, so the per-event path never
// re-resolves assignments or preset content.
bool InputEngine::ensureMappingRoute(const QString& logicalProfile)
{
    if (logicalProfile.isEmpty())
        return false;
    // Whatever happens next, this is the pad whose presses are arriving now, so
    // the next refresh plans this route even when it needed no refresh itself.
    if (m_lastControllerRoute != logicalProfile) {
        m_lastControllerRoute = logicalProfile;
        emit mappingTablesChanged();   // another pad's table may bind other buttons
    }
    if (m_mappingChains.contains(mappingChainKey(QStringLiteral("controller"), logicalProfile)))
        return false;
    refreshResolvedPreset();
    return true;
}

QStringList InputEngine::steamAdvisedBoundControls() const
{
    return SteamInputAdvice::boundControls(
        m_runtime->effectiveBindings(QStringLiteral("controller"), m_lastControllerRoute));
}

void InputEngine::refreshResolvedPreset()
{
    if (!m_db || !m_assignmentResolver || m_mappingSwitchRunning)
        return;

    m_mappingSwitchRunning = true;

    // The buffered provider candidate (if any) is a live route too: the pad is
    // delivering input right now, so its route is planned with every other live
    // one, and the candidate is dropped the moment THAT route's table changes.
    // `released` only decides whether a gate is needed - a press the user
    // already let go of cannot be held down.
    QString pendingGroup;
    QString pendingProfile;
    QString pendingControl;
    if (m_pending.source && !m_pending.controlId.isEmpty()) {
        pendingGroup = QStringLiteral("controller");
        pendingProfile = canonicalProfile(m_pending.source, m_pending.fingerprint);
        if (!m_pending.released)
            pendingControl = m_pending.controlId;
    }

    // 1. Prepare. Every chain this engine tracks is re-planned: the two groups
    //    without per-device identity, the active controller route, any route a
    //    preset is installed for, and the route of a buffered candidate - so a
    //    winner installed for a pad that went away is retired instead of leaking
    //    onto a later press. Nothing prepared here is visible anywhere yet.
    QSet<QString> keys;
    for (auto it = m_mappingChains.cbegin(); it != m_mappingChains.cend(); ++it)
        keys.insert(it.key());
    keys.insert(mappingChainKey(QStringLiteral("keyboard"), QString()));
    keys.insert(mappingChainKey(QStringLiteral("mouse"), QString()));
    if (!m_lastControllerRoute.isEmpty())
        keys.insert(mappingChainKey(QStringLiteral("controller"), m_lastControllerRoute));
    if (!pendingGroup.isEmpty())
        keys.insert(mappingChainKey(pendingGroup, pendingProfile));
    QStringList ordered = keys.values();
    ordered.sort();   // deterministic prepare order

    QVector<PlannedChain> plans;
    for (const QString& key : ordered) {
        const int separator = key.indexOf(QChar(0x1f));
        plans.append(planMappingChain(key.left(separator), key.mid(separator + 1)));
    }

    // 2. Is anything actually changing? A refresh that resolves to the same
    //    effective state AND the same table content is a true no-op: it must not
    //    destroy an active gesture, arm a gate or drop a candidate. The same
    //    preset id with rewritten rows is NOT a no-op - the content fingerprint
    //    is what separates the two. Everything a switch layer already planned is
    //    compared by the complete state: owned/installed, effective source,
    //    preset identity and table fingerprint, with no source special-cased, so
    //    a transition between two inherited states (bridge -> materialized,
    //    either -> builtin, builtin defaults rewritten) is the real switch it is.
    //    A chain this layer has never planned was served by the resolver's
    //    inherited view, so its first plan is compared by table CONTENT: merely
    //    finding an owner for the table the route already serves is bookkeeping,
    //    not a boundary, and must not cancel a buffered provider candidate that
    //    has yet to be dispatched at all.
    QVector<PlannedChain> switches;    // routes whose served table this refresh changes
    QVector<PlannedChain> firstPlans;  // routes planned for the first time
    QSet<QString> firstPlanKeys;
    QSet<QString> switchKeys;
    for (const PlannedChain& plan : plans) {
        const QString key = mappingChainKey(plan.group, plan.profile);
        const auto previousIt = m_mappingChains.constFind(key);
        if (previousIt == m_mappingChains.cend()) {
            firstPlans.append(plan);
            firstPlanKeys.insert(key);
            const QVector<BindingResolver::Binding> served =
                m_runtime->resolver().inheritedTable(plan.group, plan.profile);
            if (!BindingResolver::chainTablesEqual(served, plan.table)) {
                switches.append(plan);
                switchKeys.insert(key);
            }
            continue;
        }
        if (previousIt->owned != plan.install
            || previousIt->source != plan.source
            || previousIt->presetId != plan.presetId
            || previousIt->fingerprint != plan.fingerprint) {
            switches.append(plan);
            switchKeys.insert(key);
        }
    }
    if (switches.isEmpty() && firstPlans.isEmpty()) {
        for (const PlannedChain& plan : plans) {
            MappingChainState state;
            state.group = plan.group;
            state.profile = plan.profile;
            state.source = plan.source;
            state.owned = plan.install;
            state.presetId = plan.presetId;
            state.fingerprint = plan.fingerprint;
            m_mappingChains.insert(mappingChainKey(plan.group, plan.profile), state);
        }
        // The served state did not change, but the game context may have: the
        // export still gets the current snapshot.
        publishMappingDiagnostics();
        m_mappingSwitchRunning = false;
        return;
    }

    // 3. Snapshot the boundary before anything is invalidated. Only meant
    //    controls are gated: a control that is down in a chain whose table did
    //    not change keeps its own gesture, and gates are keyed by logical route
    //    so two pads holding the same button never block one another. A
    //    buffered provider-candidate press belongs to the moment before the
    //    switch either way: it is cancelled here and gated only when its
    //    physical control is still down.
    struct Gate {
        QString group;
        QString profile;
        QString control;
    };
    QVector<Gate> gates;
    for (const PlannedChain& plan : switches) {
        const QStringList down = m_runtime->downControls(plan.group, plan.profile);
        for (const QString& control : down) {
            if (!control.isEmpty())
                gates.append({plan.group, plan.profile, control});
        }
    }

    // Does this refresh change the chain that serves one specific route? The
    // scoped invalidation below is exactly "for every route where this is true".
    const auto routeChanged = [&switches](const QString& group, const QString& profile) {
        for (const PlannedChain& plan : switches) {
            if (plan.group == group && plan.profile == profile)
                return true;
        }
        return false;
    };
    const bool pendingRouteChanged = !pendingGroup.isEmpty()
        && routeChanged(pendingGroup, pendingProfile);

    // 4. Invalidate exactly the routes that are changing. The new table must
    //    never be visible while gesture state resolved from the old one could
    //    still complete - but a route whose table did NOT change keeps every
    //    pattern it is in the middle of: the other pad, keyboard and mouse are
    //    untouched. The global form of invalidation is reserved for
    //    whole-runtime events (reload, shutdown, backend lifecycle resets).
    //    Engine-owned transient state follows the same rule.
    for (const PlannedChain& plan : switches)
        m_runtime->invalidateGestureStateFor(plan.group, plan.profile);
    // The mapping-derived navigation repeat is a press of one table still
    // ticking: it ends only when THAT route changed, never because some other
    // route did.
    if (!m_repeatTrigger.isEmpty() && routeChanged(m_repeatRouteGroup, m_repeatRouteProfile))
        stopNavRepeat();
    // The legacy View/Back capability fallback belongs to one controller
    // profile: only that profile's entry is stale.
    for (const PlannedChain& plan : switches) {
        if (plan.group == QLatin1String("controller"))
            m_legacyViewFallbackHeld.remove(plan.profile);
    }
    // A buffered provider candidate is dropped only when its own logical route
    // changed; on any other route it is still a valid press of a table that is
    // still current.
    if (pendingRouteChanged)
        clearPendingCandidate();

    // 5. Publish. Every chain changes together, inside this one turn. A route
    //    planned for the first time is published too, even when its table did
    //    not change: `owned` in the recorded state has to be backed by an
    //    actually installed table, or a later raw write could leak into a route
    //    the state claims this layer serves - while every already-tracked route
    //    publishes only when it is a real switch.
    for (const PlannedChain& plan : plans) {
        const QString key = mappingChainKey(plan.group, plan.profile);
        if (!firstPlanKeys.contains(key) && !switchKeys.contains(key))
            continue;
        if (plan.install) {
            m_runtime->installPresetTable(plan.group, plan.profile, plan.presetId, plan.source,
                                          plan.table);
        } else {
            m_runtime->uninstallPresetTable(plan.group, plan.profile);
        }
    }

    // 6. Gate controls held across the boundary: their press ended with the
    //    old table, so they stay inert until a real release arrives. A pending
    //    candidate on an UNCHANGED route is not gated - it was never cancelled.
    for (const Gate& gate : gates)
        m_runtime->armReleaseGate(gate.group, gate.profile, gate.control);
    if (pendingRouteChanged)
        m_runtime->armReleaseGate(pendingGroup, pendingProfile, pendingControl);

    // 7. Record what each chain now serves, and republish diagnostics.
    for (const PlannedChain& plan : plans) {
        MappingChainState state;
        state.group = plan.group;
        state.profile = plan.profile;
        state.source = plan.source;
        state.owned = plan.install;
        state.presetId = plan.presetId;
        state.fingerprint = plan.fingerprint;
        m_mappingChains.insert(mappingChainKey(plan.group, plan.profile), state);
    }
    m_runtime->refreshPatternDiagnostics();
    publishMappingDiagnostics();
    m_mappingSwitchRunning = false;
    emit mappingTablesChanged();
}

QString InputEngine::mappingEffectiveSource(const QString& deviceGroup,
                                            const QString& deviceProfile) const
{
    const auto it = m_mappingChains.constFind(mappingChainKey(deviceGroup, deviceProfile));
    return it == m_mappingChains.cend() ? QString() : it->source;
}

QString InputEngine::mappingInstalledPresetId(const QString& deviceGroup,
                                              const QString& deviceProfile) const
{
    const auto it = m_mappingChains.constFind(mappingChainKey(deviceGroup, deviceProfile));
    return it == m_mappingChains.cend() ? QString() : it->presetId;
}

QStringList InputEngine::mappingArmedReleaseGates() const
{
    return m_runtime->armedReleaseGates();
}
