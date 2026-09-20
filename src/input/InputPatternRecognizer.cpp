#include "input/InputPatternRecognizer.h"

#include <QTimer>
#include <algorithm>
#include <utility>

// Per-control timing state. One of these exists per (context, control) that has
// ever produced an edge; they are reused, never reallocated per press, so the
// normal input path allocates nothing after the first press of a button.
struct InputPatternRecognizer::ControlState
{
    QString control;
    Context context;          // snapshotted at the first press of the pattern
    TriggerFacts facts;       // ditto — a reload mid-gesture cannot change them
    Timing timing;            // ditto
    quint64 generation = 0;

    QElapsedTimer sinceThisPress;
    QTimer* holdTimer = nullptr;
    QTimer* tapTimer = nullptr;
    QTimer* chordTimer = nullptr;

    bool down = false;
    // Physical reality, kept apart from the timing state machine: a press edge
    // sets this, a release edge clears it, and reset()/invalidate() never do.
    // A press-only control is not `down` after it fired, but the button is
    // still held - which is exactly what a cpo-p05 switch boundary has to know.
    bool physicallyDown = false;
    int completedTaps = 0;
    int nextHoldIndex = 0;    // into facts.holdThresholdsMs
    bool holdFired = false;

    // A chord candidate this control opened: its own gestures are held back
    // until the window resolves, so a completed chord can consume them.
    bool chordOpen = false;
    bool releasedWhileChordOpen = false;
    // Set on the control that *completed* a chord: the chord consumed this
    // physical press, so the release must not also produce a tap.
    bool chordConsumed = false;

    bool pending() const { return down || completedTaps > 0 || chordOpen; }

    ~ControlState()
    {
        delete holdTimer;
        delete tapTimer;
        delete chordTimer;
    }
};

int InputPatternRecognizer::TriggerFacts::highestTapCount() const
{
    for (int count = GestureSpec::kMaxTapCount; count >= 1; --count) {
        if (hasTapCount(count))
            return count;
    }
    return 0;
}

bool InputPatternRecognizer::TriggerFacts::hasTapCount(int count) const
{
    if (count < 1 || count > GestureSpec::kMaxTapCount)
        return false;
    return (tapCountMask & (1 << count)) != 0;
}

bool InputPatternRecognizer::TriggerFacts::isEmpty() const
{
    return !hasPress && tapCountMask == 0 && holdThresholdsMs.isEmpty()
        && chordPartners.isEmpty();
}

InputPatternRecognizer::InputPatternRecognizer(QObject* parent)
    : QObject(parent)
{
}

InputPatternRecognizer::~InputPatternRecognizer()
{
    for (ControlState* state : std::as_const(m_states))
        delete state;
}

void InputPatternRecognizer::setFactsProvider(FactsProvider provider)
{
    m_facts = std::move(provider);
    invalidate();
}

void InputPatternRecognizer::setTiming(const Timing& timing)
{
    m_timing = timing;
    // Pending patterns were snapshotted against the old numbers; letting them
    // finish under the new ones would make a tap window change length halfway.
    invalidate();
}

void InputPatternRecognizer::invalidate()
{
    ++m_generation;
    for (ControlState* state : std::as_const(m_states))
        reset(state);
}

void InputPatternRecognizer::invalidateRoute(const QString& deviceGroup,
                                             const QString& deviceProfile)
{
    for (ControlState* state : std::as_const(m_states)) {
        if (state->context.deviceGroup != deviceGroup
            || state->context.deviceProfile != deviceProfile)
            continue;
        // Mark this state's queued callbacks stale before resetting it. The
        // stale token is 0, which no live generation ever holds - so a callback
        // already dispatched for this state resolves to nothing, while every
        // other route keeps comparing against the unchanged m_generation.
        state->generation = 0;
        reset(state);
    }
}

void InputPatternRecognizer::notePhysicalRelease(const QString& deviceGroup,
                                                 const QString& deviceProfile,
                                                 const QString& control)
{
    const auto it = m_states.constFind(deviceGroup + QChar(0x1f) + deviceProfile
                                       + QChar(0x1f) + control);
    if (it == m_states.cend())
        return;
    // Only the physical truth changes: the pattern this press belonged to was
    // already invalidated with its own table, so there is nothing to end. The
    // timing state (down, timers, taps) is deliberately left exactly as the
    // invalidation left it.
    (*it)->physicallyDown = false;
}

QStringList InputPatternRecognizer::downControls(const QString& deviceGroup,
                                                 const QString& deviceProfile) const
{
    QStringList controls;
    for (ControlState* state : m_states) {
        if (!state->physicallyDown)
            continue;
        if (state->context.deviceGroup != deviceGroup
            || state->context.deviceProfile != deviceProfile)
            continue;
        controls.append(state->control);
    }
    // QHash iteration order is arbitrary; a deterministic list keeps the
    // snapshot (and the diagnostics built from it) reproducible.
    controls.sort();
    return controls;
}

QString InputPatternRecognizer::stateKey(const Context& context, const QString& control)
{
    return context.deviceGroup + QChar(0x1f) + context.deviceProfile + QChar(0x1f) + control;
}

InputPatternRecognizer::ControlState* InputPatternRecognizer::stateFor(const Context& context,
                                                                       const QString& control)
{
    const QString key = stateKey(context, control);
    ControlState*& state = m_states[key];
    if (!state) {
        state = new ControlState;
        state->control = control;
        state->holdTimer = new QTimer;
        state->holdTimer->setSingleShot(true);
        state->tapTimer = new QTimer;
        state->tapTimer->setSingleShot(true);
        state->chordTimer = new QTimer;
        state->chordTimer->setSingleShot(true);
        connect(state->holdTimer, &QTimer::timeout, this, [this, state] {
            if (state->generation != m_generation)
                return;
            fireDueHolds(state);
        });
        connect(state->tapTimer, &QTimer::timeout, this, [this, state] {
            if (state->generation != m_generation)
                return;
            completeTaps(state);
        });
        connect(state->chordTimer, &QTimer::timeout, this, [this, state] {
            if (state->generation != m_generation)
                return;
            abandonChordCandidate(state);
        });
    }
    // stateFor() is only ever reached on a press edge, so this is where a
    // control becomes physically down in this context; the matching release
    // edge in release() is the only place it stops being down. reset() and
    // invalidate() deliberately leave it alone: a switch boundary needs the
    // physical truth, not the timing state machine's bookkeeping.
    state->physicallyDown = true;
    return state;
}

void InputPatternRecognizer::snapshot(ControlState* state, const Context& context)
{
    state->context = context;
    state->timing = m_timing;
    state->generation = m_generation;
    state->facts = m_facts ? m_facts(context, state->control) : TriggerFacts{};
    state->completedTaps = 0;
    state->nextHoldIndex = 0;
    state->holdFired = false;
    state->chordConsumed = false;
    state->releasedWhileChordOpen = false;
}

bool InputPatternRecognizer::press(const Context& context, const QString& control)
{
    // A chord completes before anything else looks at this press: the second
    // control has to be able to consume the first control's held-back gesture,
    // and its own, before either of them can act on their own behalf.
    if (ControlState* candidate = candidateFor(context, control)) {
        const QString first = candidate->control;
        closeChordCandidate(candidate);
        ControlState* second = stateFor(context, control);
        reset(second);
        snapshot(second, context);
        second->chordConsumed = true;
        emit patternNote(QStringLiteral("chord completed %1 > %2").arg(first, control));
        emit recognized(context, TriggerSpec::orderedChord(first, control),
                        GestureSpec::press());
        return true;
    }

    ControlState* state = stateFor(context, control);

    // Three ways a pending pattern stops being the one the user started: the
    // world was invalidated under it, the context moved (the overlay opened
    // between two taps), or it simply is not pending. All three mean: throw the
    // half-finished sequence away and start over from this press.
    const bool continues = state->pending() && state->generation == m_generation
                        && state->context == context;
    if (!continues) {
        reset(state);
        snapshot(state, context);
    } else if (state->chordOpen) {
        // The same control pressed again while its own candidate is open: a
        // mirrored backend repeating an edge, not a second chord. Ignore it
        // rather than opening a duplicate candidate or restarting the window.
        return true;
    }

    if (!state->facts.chordPartners.isEmpty()) {
        openChordCandidate(state);
        return true;
    }
    beginSinglePress(state);
    return !state->facts.isEmpty();
}

// Everything a press does when no chord is involved: fire Press, then start the
// tap and hold clocks. Split out because the chord fallback replays exactly
// this, anchored to the original physical press instead of to the timeout.
void InputPatternRecognizer::beginSinglePress(ControlState* state, bool keepPressClock)
{
    if (state->facts.hasPress) {
        emit recognized(state->context, TriggerSpec::single(state->control),
                        GestureSpec::press());
        // The action just dispatched may have invalidated every pending
        // pattern (toggling the overlay does, through cancelAll). The reset
        // already cleared this state — arming tap/hold clocks on it now would
        // resurrect a pattern the invalidation meant to kill.
        if (state->generation != m_generation)
            return;
    }
    if (state->facts.tapCountMask == 0 && state->facts.holdThresholdsMs.isEmpty()) {
        // Press-only (or unbound) control: nothing is pending afterwards.
        reset(state);
        return;
    }

    state->tapTimer->stop();
    state->down = true;
    state->holdFired = false;
    state->nextHoldIndex = 0;
    // Hold is measured from *this* press, not from the start of the sequence:
    // holding on the second tap of a double tap is a hold, not a 900 ms tap.
    // A chord fallback passes keepPressClock: its clock is already anchored to
    // the original physical press, which is the whole point of the fallback.
    if (!keepPressClock)
        state->sinceThisPress.restart();
    scheduleNextHold(state);
}

InputPatternRecognizer::ControlState* InputPatternRecognizer::candidateFor(
    const Context& context, const QString& secondControl)
{
    // Newest first: holding two chord starters at once must not make the older
    // one swallow a partner that belongs to the newer.
    for (auto it = m_openChords.crbegin(); it != m_openChords.crend(); ++it) {
        ControlState* candidate = *it;
        if (candidate->generation != m_generation || !(candidate->context == context))
            continue;
        if (candidate->control != secondControl
            && candidate->facts.chordPartners.contains(secondControl))
            return candidate;
    }
    return nullptr;
}

void InputPatternRecognizer::openChordCandidate(ControlState* state)
{
    state->down = true;
    state->chordOpen = true;
    state->releasedWhileChordOpen = false;
    // The clock starts at the PHYSICAL press. A hold threshold, and the tap
    // that a fallback replays, are both measured from this moment — never from
    // the point where the chord window gave up.
    state->sinceThisPress.restart();
    if (!m_openChords.contains(state))
        m_openChords.append(state);
    state->chordTimer->start(qMax(1, state->timing.chordWindowMs));
    emit patternNote(QStringLiteral("chord candidate open on %1 (%2 ms)")
                         .arg(state->control).arg(state->timing.chordWindowMs));
}

// The chord completed: the first control's own gestures are consumed and never
// replayed.
void InputPatternRecognizer::closeChordCandidate(ControlState* state)
{
    state->chordTimer->stop();
    m_openChords.removeAll(state);
    state->chordOpen = false;
    reset(state);
}

// The window expired, or the first control was let go before a partner arrived.
// Either way the constituent gets its ordinary press back, replayed AT MOST
// ONCE and timed from the original physical press.
void InputPatternRecognizer::abandonChordCandidate(ControlState* state)
{
    if (!state->chordOpen)
        return;
    state->chordTimer->stop();
    m_openChords.removeAll(state);
    state->chordOpen = false;

    const bool wasReleased = state->releasedWhileChordOpen;
    emit patternNote(QStringLiteral("chord candidate on %1 gave up (%2)")
                         .arg(state->control,
                              wasReleased ? QStringLiteral("released early")
                                          : QStringLiteral("window expired")));
    beginSinglePress(state, /*keepPressClock=*/true);
    // The replayed press may have invalidated the recognizer (see
    // beginSinglePress) — this state is dead, nothing left to replay on it.
    if (state->generation != m_generation)
        return;
    if (wasReleased) {
        // Released before the window closed: replay the release immediately so
        // a quick tap of a chord's first button stays a tap. No hold can fire
        // in between, because no time passes here.
        state->releasedWhileChordOpen = false;
        releaseSingle(state);
        return;
    }
    // Still held. A threshold that already elapsed inside the chord window is
    // due right now — the user has been holding the button that whole time.
    fireDueHolds(state);
}

bool InputPatternRecognizer::release(const Context& context, const QString& control)
{
    const auto it = m_states.constFind(stateKey(context, control));
    if (it == m_states.cend())
        return false;
    ControlState* state = *it;

    // The physical button is up again, whatever the timing state machine
    // decides to do with this edge: a switch boundary may now clear this
    // control's release gate.
    state->physicallyDown = false;

    // The chord consumed this physical press. Its release means nothing.
    if (state->chordConsumed) {
        reset(state);
        return true;
    }
    if (state->chordOpen) {
        // Letting go of the first control ends the chord's chance immediately:
        // it has to be HELD for a partner to complete it.
        state->releasedWhileChordOpen = true;
        abandonChordCandidate(state);
        return true;
    }
    if (!state->down)
        return false;
    return releaseSingle(state);
}

bool InputPatternRecognizer::releaseSingle(ControlState* state)
{
    state->down = false;
    state->holdTimer->stop();

    // A hold that already fired consumes the press: releasing must not also
    // produce a tap, or Share-hold would save a replay *and* screenshot.
    if (state->holdFired) {
        reset(state);
        return true;
    }

    ++state->completedTaps;
    const int highest = state->facts.highestTapCount();
    if (highest == 0) {
        reset(state);
        return true;
    }

    // Exact-count dispatch. Three taps fire the x3 binding only — never x1,
    // then x2, then x3 on the way up. A count waits only when a higher one is
    // actually bound on this control; otherwise it fires on the release edge
    // exactly as a single tap always has.
    if (state->completedTaps >= highest) {
        completeTaps(state);
        return true;
    }
    state->tapTimer->start(qMax(1, state->timing.multiTapIntervalMs));
    return true;
}

void InputPatternRecognizer::completeTaps(ControlState* state)
{
    const int taps = state->completedTaps;
    const Context context = state->context;
    const QString control = state->control;
    const bool bound = state->facts.hasTapCount(taps);
    reset(state);
    // An unbound count is not an error and not a fallback: two taps on a
    // control bound at x1 and x3 mean neither of them.
    if (bound && taps >= 1)
        emit recognized(context, TriggerSpec::single(control), GestureSpec::tap(taps));
}

void InputPatternRecognizer::scheduleNextHold(ControlState* state)
{
    state->holdTimer->stop();
    if (!state->down || state->nextHoldIndex >= state->facts.holdThresholdsMs.size())
        return;
    const int threshold = state->facts.holdThresholdsMs.at(state->nextHoldIndex);
    const int elapsed = static_cast<int>(state->sinceThisPress.elapsed());
    state->holdTimer->start(qMax(1, threshold - elapsed));
}

void InputPatternRecognizer::fireDueHolds(ControlState* state)
{
    if (!state->down)
        return;
    const int elapsed = static_cast<int>(state->sinceThisPress.elapsed());
    while (state->nextHoldIndex < state->facts.holdThresholdsMs.size()) {
        const int threshold = state->facts.holdThresholdsMs.at(state->nextHoldIndex);
        if (elapsed < threshold)
            break;
        ++state->nextHoldIndex;
        state->holdFired = true;
        // The threshold travels with the pattern: two holds on one button (open
        // capture at 1 s, bulk select at 2 s) must each fire only their own.
        emit recognized(state->context, TriggerSpec::single(state->control),
                        GestureSpec::hold(threshold));
        // The hold's action may have invalidated the recognizer reentrantly
        // (Toggle Overlay does: show → setOverlayVisible → cancelAll). The
        // reset put nextHoldIndex back to 0 while this loop is mid-iteration;
        // continuing would re-fire the same hold forever — the overlay
        // show/hide loop that exhausts the process's window-handle quota.
        if (state->generation != m_generation || !state->down)
            return;
    }
    scheduleNextHold(state);
}

void InputPatternRecognizer::reset(ControlState* state)
{
    state->holdTimer->stop();
    state->tapTimer->stop();
    state->chordTimer->stop();
    m_openChords.removeAll(state);
    state->chordOpen = false;
    state->releasedWhileChordOpen = false;
    state->chordConsumed = false;
    state->down = false;
    state->completedTaps = 0;
    state->nextHoldIndex = 0;
    state->holdFired = false;
}
