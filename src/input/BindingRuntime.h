#pragma once

#include "input/BindingRelation.h"
#include "input/BindingResolver.h"
#include "input/InputPatternRecognizer.h"

#include <QHash>
#include <QObject>
#include <QSet>

class CaptureDatabase;

// Binds the pattern recognizer to the effective binding table.
//
// The timing state machine lives in InputPatternRecognizer; this class owns the
// two things that need the binding table: telling the recognizer what gestures
// exist on a control, and turning a recognized pattern into actions.
class BindingRuntime : public QObject
{
    Q_OBJECT
public:
    explicit BindingRuntime(CaptureDatabase* database, QObject* parent = nullptr);
    ~BindingRuntime() override;

    void setDefaultHoldMs(int milliseconds);
    void setTiming(const InputPatternRecognizer::Timing& timing);
    InputPatternRecognizer::Timing timing() const;
    void reload();
    // The resolver the runtime serves from. Exposed for the migration
    // materializer (cpo-p03), which needs the live alias chain and the
    // validated rows exactly as the runtime sees them.
    BindingResolver& resolver();
    const BindingResolver& resolver() const;
    QVector<BindingResolver::Binding> effectiveBindings(
        const QString& deviceGroup, const QString& deviceProfile = {}) const;
    QVector<BindingResolver::Binding> baselineBindings(
        const QString& deviceGroup, const QString& deviceProfile = {}) const;
    BindingResolver::Gesture inheritedGesture(const QString& deviceGroup,
                                              const QString& deviceProfile,
                                              const QString& actionId, int slot) const;
    // The device whose rows a diagnostics paste should show next to the shared
    // group rows. Cheap and idempotent: unchanged input returns immediately, so
    // the press path can keep it current without re-resolving anything.
    void setActiveProfile(const QString& deviceGroup, const QString& deviceProfile);

    void setProfileAlias(const QString& profile, const QString& legacyProfile);
    void setProfileAliases(const QString& profile, const QStringList& legacyProfiles);

    // One classified pair out of the effective table for a device group.
    struct Relation {
        BindingResolver::Binding left;
        BindingResolver::Binding right;
        BindingRelation::Kind kind = BindingRelation::Kind::None;
    };

    // Every non-None pair for this group/profile, classified once and cached
    // until the next reload(). The editor classifies its capture candidate
    // directly (the candidate is not in the table yet, so it cannot appear
    // here); tst_bindingeditor::runtimeRelationsMatchDirectClassification pins
    // both paths to the same BindingRelation::classify verdicts. Nothing on the
    // per-event path calls this — gesture dispatch works off the resolver — so
    // the O(n^2) pass costs nothing at input rates.
    const QVector<Relation>& relations(const QString& deviceGroup,
                                       const QString& deviceProfile = {}) const;

    bool press(const QString& deviceGroup, const QString& deviceProfile,
               const QString& triggerCode, ActionCatalog::Scope primaryScope,
               ActionCatalog::Scope fallbackScope = ActionCatalog::Scope::Global);
    bool release(const QString& deviceGroup, const QString& deviceProfile,
                 const QString& triggerCode);
    void cancelAll();

    // ---------------------------------------------------------------- cpo-p05
    // One mapping route (deviceGroup + canonical logical profile) can be
    // switched to a resolved preset winner at a safe input boundary. The engine
    // prepares the table first, then invalidates, then installs - so the new
    // table never becomes visible while old-generation gesture state could
    // still complete. Installing does not touch recognizer state by itself.
    void installPresetTable(const QString& deviceGroup, const QString& deviceProfile,
                            const QString& presetId, const QString& source,
                            const QVector<BindingResolver::Binding>& table);
    void uninstallPresetTable(const QString& deviceGroup, const QString& deviceProfile);
    bool hasInstalledPresetTable(const QString& deviceGroup, const QString& deviceProfile) const;
    QString installedPresetId(const QString& deviceGroup, const QString& deviceProfile) const;
    QString installedPresetSource(const QString& deviceGroup, const QString& deviceProfile) const;
    // Installed chains as "group\x1fprofile" keys, so a switch can also retire
    // a chain that is no longer on any live route.
    QStringList installedPresetChains() const;

    // The invalidation half of a switch: the recognizer generation moves and
    // every pending pattern dies (exactly like reload()), the mapping-derived
    // relation cache drops and the press contexts of the old table go with it.
    // Release gates are deliberately NOT touched here; the switch arms them
    // right after this call, from the pre-invalidation snapshot.
    //
    // This is the WHOLE-RUNTIME form (reload, shutdown, backend lifecycle
    // reset). A mapping switch that changes one route uses the scoped form
    // below instead, so a gesture belonging to an unchanged route survives.
    void invalidateGestureState();

    // The route-scoped half of a mapping switch: only this (deviceGroup,
    // deviceProfile)'s recognizer states are reset and marked stale, only its
    // press contexts are dropped and only its relation-cache entry is
    // invalidated. Every other route keeps its timers, snapshots and cache.
    void invalidateGestureStateFor(const QString& deviceGroup, const QString& deviceProfile);

    // Republish the diagnostics view of the current effective tables.
    void refreshPatternDiagnostics();

    // Physical release bookkeeping for a gated release: clears the recognizer's
    // "still down" bit for one route + control without completing anything. The
    // engine's non-active-backend path needs this because it closes a gate
    // without going through release().
    void notePhysicalRelease(const QString& deviceGroup, const QString& deviceProfile,
                             const QString& control);

    // Release gates: keyed by logical mapping route + control - never by
    // backend pointer, so two controllers holding the same button on different
    // routes cannot block one another, and a provider failover for one route
    // cannot open a hole in its gate. Armed only for controls that were
    // physically down at a switch boundary; cleared by a real release edge for
    // the same route + control. A press on a gated control is consumed and does
    // nothing until that release arrives.
    void armReleaseGate(const QString& deviceGroup, const QString& deviceProfile,
                        const QString& control);
    bool clearReleaseGate(const QString& deviceGroup, const QString& deviceProfile,
                          const QString& control);
    bool releaseGateArmed(const QString& deviceGroup, const QString& deviceProfile,
                          const QString& control) const;
    QStringList armedReleaseGates() const;
    QStringList downControls(const QString& deviceGroup, const QString& deviceProfile) const;

    // Deterministic content fingerprint of an effective table, sorted by row so
    // storage order cannot fake a change. This is the missing half of the
    // switch's no-op identity: the same winner with the same fingerprint is a
    // true no-op, the same preset id with rewritten rows is a real switch.
    static QString tableFingerprint(const QVector<BindingResolver::Binding>& table);

signals:
    // deviceGroup is the group the recognized gesture came from ("controller",
    // "keyboard", "mouse") and deviceProfile is the mapping route inside it
    // (the canonical logical profile; empty for keyboard and mouse). A gesture
    // resolves long after its press, so both have to travel with the signal: a
    // "last press" guess would credit a controller hold to a keyboard tap that
    // landed in between, and the cpo-p05 switch needs the route to tell whether
    // a mapping-derived repeat belongs to a table that is being replaced.
    void actionTriggered(const QString& actionId, const QString& triggerCode,
                         const QString& deviceGroup, const QString& deviceProfile);

private:
    InputPatternRecognizer::TriggerFacts factsFor(const InputPatternRecognizer::Context& context,
                                                  const QString& control) const;
    void dispatch(const InputPatternRecognizer::Context& context, const TriggerSpec& trigger,
                  const GestureSpec& gesture);
    int holdThreshold(const BindingResolver::Binding& binding) const;
    void publishBoundPatterns();

    BindingResolver m_resolver;
    InputPatternRecognizer m_recognizer;
    // Armed release gates, keyed group\x1fprofile\x1fcontrol (cpo-p05). Cleared
    // by the matching release edge; never by a switch or a reload.
    QSet<QString> m_releaseGates;
    // The context a control was last pressed in, so release() can address the
    // same recognizer state without the caller having to repeat the scope.
    QHash<QString, InputPatternRecognizer::Context> m_pressContexts;
    // Cleared by reload(); repopulated on first request per group/profile.
    mutable QHash<QString, QVector<Relation>> m_relations;
    // The device last seen pressing something, for diagnostics only.
    QString m_activeGroup;
    QString m_activeProfile;
};
