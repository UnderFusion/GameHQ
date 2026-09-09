#pragma once

#include "input/BindingRelation.h"
#include "input/BindingResolver.h"
#include "input/InputPatternRecognizer.h"

#include <QHash>
#include <QObject>

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

signals:
    // deviceGroup is the group the recognized gesture came from ("controller",
    // "keyboard", "mouse"). A gesture resolves long after its press, so the
    // group has to travel with the signal: a "last press" guess would credit a
    // controller hold to a keyboard tap that landed in between.
    void actionTriggered(const QString& actionId, const QString& triggerCode,
                         const QString& deviceGroup);

private:
    InputPatternRecognizer::TriggerFacts factsFor(const InputPatternRecognizer::Context& context,
                                                  const QString& control) const;
    void dispatch(const InputPatternRecognizer::Context& context, const TriggerSpec& trigger,
                  const GestureSpec& gesture);
    int holdThreshold(const BindingResolver::Binding& binding) const;
    void publishBoundPatterns();

    BindingResolver m_resolver;
    InputPatternRecognizer m_recognizer;
    // The context a control was last pressed in, so release() can address the
    // same recognizer state without the caller having to repeat the scope.
    QHash<QString, InputPatternRecognizer::Context> m_pressContexts;
    // Cleared by reload(); repopulated on first request per group/profile.
    mutable QHash<QString, QVector<Relation>> m_relations;
    // The device last seen pressing something, for diagnostics only.
    QString m_activeGroup;
    QString m_activeProfile;
};
