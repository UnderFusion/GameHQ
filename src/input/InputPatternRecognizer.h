#pragma once

#include "input/ActionCatalog.h"
#include "input/BindingPattern.h"

#include <QElapsedTimer>
#include <QHash>
#include <QMetaType>
#include <QObject>
#include <QStringList>
#include <QVector>
#include <functional>

class QTimer;

// Turns raw press/release edges into recognized patterns.
//
// This is the whole timing state machine and nothing else: it counts taps,
// times holds, opens and closes chord candidates, suppresses constituents a
// completed chord consumed, and cancels anything pending when the world moves
// underneath it. It never touches the database, the resolver or the action
// catalog — what a recognized pattern *does* is BindingRuntime's problem.
//
// The separation matters because the two halves fail differently. Dispatch bugs
// show up as "the wrong thing happened"; timing bugs show up as "sometimes
// nothing happens", which is only reproducible with a clock you control. Behind
// the TriggerFacts seam this class is fully testable without a device, a
// database or a running app.
class InputPatternRecognizer : public QObject
{
    Q_OBJECT
public:
    // Where a pattern started. Snapshotted at the first physical press and
    // carried to dispatch, so an action delayed by a tap window still fires
    // against the context the user was actually in when they pressed.
    struct Context {
        QString deviceGroup;
        QString deviceProfile;
        ActionCatalog::Scope primaryScope = ActionCatalog::Scope::Global;
        ActionCatalog::Scope fallbackScope = ActionCatalog::Scope::Global;

        bool operator==(const Context& other) const = default;
    };

    struct Timing {
        // How long a lower tap count waits to find out whether more taps are
        // coming. Only ever spent when a higher count is actually bound.
        int multiTapIntervalMs = 300;
        // How long the first control of a chord waits for its partner.
        int chordWindowMs = 300;
        // What a Hold binding storing 0 means.
        int defaultHoldMs = 2000;
    };

    // What the recognizer needs to know about one control in one context. The
    // provider answers from the effective binding table; the recognizer treats
    // the answer as opaque facts and snapshots them for the whole pattern, so a
    // binding reload mid-gesture cannot change the rules halfway through.
    struct TriggerFacts {
        bool hasPress = false;
        // Bit N (1..kMaxTapCount) set = a Tap xN binding exists on this control.
        int tapCountMask = 0;
        // Every distinct hold threshold bound to this control, ascending, with
        // "use the default" already resolved to a real number.
        QVector<int> holdThresholdsMs;
        // Controls that complete an ordered chord this control can start.
        QStringList chordPartners;

        int highestTapCount() const;
        bool hasTapCount(int count) const;
        bool isEmpty() const;
    };

    using FactsProvider = std::function<TriggerFacts(const Context&, const QString& control)>;

    explicit InputPatternRecognizer(QObject* parent = nullptr);
    ~InputPatternRecognizer() override;

    void setFactsProvider(FactsProvider provider);
    void setTiming(const Timing& timing);
    Timing timing() const { return m_timing; }

    // Everything that can make a pending pattern meaningless: a backend switch,
    // a disconnect, a binding reload, leaving the scope. Bumps the generation
    // counter, so a timer that is already queued resolves to nothing instead of
    // firing an action into a world that has moved on.
    void invalidate();

    // The cpo-p05 mapping switch replaces the table of ONE route. Only the
    // states whose (deviceGroup, deviceProfile) matches that route are reset,
    // and only those are marked stale, so an unaffected route (another pad,
    // keyboard, mouse) keeps its timers, tap windows and snapshots running.
    // The global generation deliberately does NOT move: it is the value every
    // other state's queued callback compares itself against.
    void invalidateRoute(const QString& deviceGroup, const QString& deviceProfile);

    // Physical bookkeeping only, no gesture semantics: clears the "still down"
    // bit for one control in one route without completing, firing or cancelling
    // anything. The switch needs it when a gated release closes a release gate
    // (the press it belonged to already ended with the old table, so there is no
    // pattern left to complete) - without it the control would stay "down"
    // forever and a LATER switch would arm a phantom gate for a button the user
    // is not holding. A no-op when no state exists for that route + control.
    void notePhysicalRelease(const QString& deviceGroup, const QString& deviceProfile,
                             const QString& control);

    // Returns true when the edge was consumed by a pattern (pending or fired).
    bool press(const Context& context, const QString& control);
    bool release(const Context& context, const QString& control);

    // Controls whose press is still physically down in one mapping chain
    // (device group + profile), sorted. The cpo-p05 switch snapshots this
    // BEFORE invalidating, so a control held across the boundary can be
    // release-gated instead of acting under the table that replaced its own.
    QStringList downControls(const QString& deviceGroup, const QString& deviceProfile) const;

signals:
    // A pattern completed. `gesture` is exact: Tap carries the number of taps
    // actually performed, Hold carries the threshold that just elapsed.
    void recognized(const InputPatternRecognizer::Context& context,
                    const TriggerSpec& trigger, const GestureSpec& gesture);
    // Short human-readable lifecycle note for diagnostics. Emitted only for
    // events a bug report needs to reconstruct a miss: chord candidates opening,
    // completing and timing out. Deliberately a signal rather than a direct call
    // into the diagnostics singleton, so this class stays free of it.
    void patternNote(const QString& detail);

private:
    struct ControlState;

    ControlState* stateFor(const Context& context, const QString& control);
    void snapshot(ControlState* state, const Context& context);
    void beginSinglePress(ControlState* state, bool keepPressClock = false);
    void scheduleNextHold(ControlState* state);
    void fireDueHolds(ControlState* state);
    void completeTaps(ControlState* state);
    void reset(ControlState* state);
    static QString stateKey(const Context& context, const QString& control);

    // Chords: the first control opens a bounded candidate instead of acting.
    ControlState* candidateFor(const Context& context, const QString& secondControl);
    void openChordCandidate(ControlState* state);
    void closeChordCandidate(ControlState* state);
    void abandonChordCandidate(ControlState* state);
    bool releaseSingle(ControlState* state);

    FactsProvider m_facts;
    Timing m_timing;
    quint64 m_generation = 1;
    QHash<QString, ControlState*> m_states;
    // Open candidates in press order; the newest one that accepts an incoming
    // control wins, so holding two chord starters cannot lose either fallback.
    QVector<ControlState*> m_openChords;
};

Q_DECLARE_METATYPE(InputPatternRecognizer::Context)
