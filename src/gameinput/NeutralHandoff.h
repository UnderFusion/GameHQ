#pragma once

#include <QString>

// cpo-o06e: the RELEASE transition of the exclusive GameInput policy.
//
// Closing the overlay while a control is still held used to expose that held
// state the instant the policy was dropped and the foreground went back: the
// game resumed on a controller the user was still holding down. The handoff
// defers both — the release and the foreground hand-back — until the pad is
// physically neutral, or until a bounded timeout expires.
//
// What "neutral" means, in ONE place (decideNeutralHandoff below):
//
//   * no digital control is held, as published by the active backend. That is
//     where the threshold and deadzone decisions already live: GameInput
//     publishes a trigger button only past its own threshold and a
//     thumbstick-direction flag only past its own deadzone, and the legacy
//     backends run StickNav's per-backend AxisConfig. The handoff reuses those
//     published edges instead of inventing a second controller interpretation
//     that could disagree with the one that routed the press.
//   * the overlay input path has quiesced: no navigation repeat and no pending
//     gesture is still able to fire an action after the close.
//
// A device that disappears counts as neutral — it cannot carry a held state into
// the game — and a pad that is already neutral at close adds exactly zero
// latency: the first evaluation runs synchronously.
namespace ModernInput
{

struct NeutralHandoffSample
{
    int attachedDevices = 0;   // devices the engine can still read
    int heldControls = 0;      // ... and how many controls are down right now
    QString heldSummary;       // canonical control ids, bounded; receipt only
    bool overlayActionsQuiesced = false;
};

struct NeutralHandoffDecision
{
    bool neutral = true;
    QString reason;   // why it is neutral / not neutral yet, for the receipt
};

NeutralHandoffDecision decideNeutralHandoff(const NeutralHandoffSample& sample);

// The one receipt vocabulary, shared by the diagnostics timeline and the overlay
// close line, so a report and a test spell a stage the same way:
//
//   neutral-handoff=waiting (reason)              - the wait started
//   neutral-handoff=passed duration_ms=… polls=…  - neutral reached in time
//   neutral-handoff=timeout duration_ms=… polls=… held="…"
//   neutral-handoff=released-elsewhere …          - another path restored first
//   neutral-handoff=not-engaged (reason)          - nothing to defer
//   neutral-handoff=cancelled (reason)            - the close was abandoned
QString neutralHandoffReceipt(const QString& stage, int durationMs, int polls, const QString& detail);

// Bounded by design: the close must not turn into an indefinite "release the
// button to get back to the game" state. 400 ms covers a deliberate release plus
// input latency; past that the close completes and the receipt says it timed out
// rather than claiming a clean handoff.
inline constexpr int kNeutralHandoffTimeoutMs = 400;
// Poll interval: fast enough to follow a release, cheap enough not to spin.
inline constexpr int kNeutralHandoffTickMs = 10;
// How many held control ids a receipt spells out before it counts the rest.
inline constexpr int kNeutralHandoffSummaryLimit = 3;

// Implemented by the input layer (InputEngine). The overlay ASKS; it never reads
// a device itself and it never synthesizes a release event into the game.
class NeutralHandoffSource
{
public:
    virtual ~NeutralHandoffSource() = default;
    virtual NeutralHandoffSample sampleNeutralPadState() const = 0;
    // true for the duration of the release handoff: overlay input stops
    // producing actions and pending gestures are cancelled, while the physical
    // state keeps being tracked so the wait can observe the release.
    virtual void setOverlayReleaseActive(bool active) = 0;
};

}  // namespace ModernInput
