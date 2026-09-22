#pragma once

#include <QString>

// cpo-o06c: the two focus policies GameHQ can run the GameInput runtime under,
// and the only interface an interactive surface may use to ask for one.
//
// Background is the shipped default: background input plus the background
// Guide/Share flags, so GameHQ keeps seeing the pad and the system buttons while
// the game owns the foreground. ExclusiveForeground is what this leaf adds: while
// the overlay is the verified interactive foreground, other GameInput clients are
// meant to stop receiving the pad.
//
// What the exclusive mode cannot do, stated once so it is never implied:
// GameInput's focus policy is best-effort and GameInput-scoped. Games reading
// XInput, DirectInput or Raw Input directly are unaffected, and nothing inside
// this process can observe whether another process actually stopped receiving
// input (docs/design/gameinput-spike.md, cpo-o06d for the external receiver).
namespace ModernInput
{

enum class GameInputFocusMode
{
    Background,           // default: background input + background guide/share
    ExclusiveForeground,  // overlay interactive: foreground-only standard input
};

// Stable identifiers, so a log line, a trace field and a test expectation spell
// the mode the same way.
inline QString gameInputFocusModeName(GameInputFocusMode mode)
{
    return mode == GameInputFocusMode::ExclusiveForeground
        ? QStringLiteral("exclusive-foreground")
        : QStringLiteral("background");
}

// The vendor's GameInputFocusPolicy constants, passed in as plain values so this
// header stays free of the vendor SDK.
struct GameInputFocusPolicyFlags
{
    int backgroundInput = 0;
    int backgroundGuideButton = 0;
    int backgroundShareButton = 0;
    int exclusiveForegroundInput = 0;
};

// The mask each mode applies, in ONE place, so the flags sent to the runtime and
// the words the diagnostics export prints come from the same definition and
// cannot drift. The rules, deliberately:
//
//   * both background system-button flags are in BOTH masks: GameHQ's own
//     Guide/Share handling (hold-PS desktop summon, Share capture) must behave
//     exactly as before while the overlay is interactive;
//   * exclusive-foreground input REPLACES background input — requesting both
//     would contradict itself;
//   * GameInputExclusiveForegroundGuideButton / ...ShareButton are never
//     requested: taking the system buttons away from the game as well is broader
//     than this experiment needs (cpo-o06c).
inline int gameInputFocusMask(GameInputFocusMode mode, const GameInputFocusPolicyFlags& flags)
{
    const bool exclusive = mode == GameInputFocusMode::ExclusiveForeground;
    return (exclusive ? flags.exclusiveForegroundInput : flags.backgroundInput)
        | flags.backgroundGuideButton
        | flags.backgroundShareButton;
}

// What the export says about the policy in force. Phrased so the exclusive state
// cannot be read as verified controller isolation.
inline QString gameInputFocusPolicyDescription(GameInputFocusMode mode)
{
    return mode == GameInputFocusMode::ExclusiveForeground
        ? QStringLiteral("exclusive-foreground input + background guide + background share "
                         "(guide/share still delivered to other clients; effect not verified "
                         "in-process)")
        : QStringLiteral("background input + background guide + background share "
                         "(no exclusive-foreground flags)");
}

// What a caller may ask of the focus policy. Deliberately not a setter for a raw
// flag mask: callers name the state they need, the single owner
// (GameInputFocusController) decides the flags, the transition and the record.
//
// Kept as an interface so an interactive surface can be exercised against the
// request/refusal/release sequence without a GameInput runtime.
class GameInputFocusRequestSink
{
public:
    virtual ~GameInputFocusRequestSink() = default;

    // Ask for exclusive-foreground input. Idempotent: a second call while the
    // policy is already in force changes nothing and reports success.
    // Returns true when the policy is in force as far as this process can tell,
    // false when it was refused (no runtime attached) — never a claim that any
    // other process lost controller input.
    virtual bool requestExclusiveForeground(const QString& reason) = 0;
    // Return to the background policy. Idempotent; a no-op when already there,
    // so every exit path may call it unconditionally.
    virtual void restoreBackground(const QString& reason) = 0;

    virtual GameInputFocusMode focusMode() const = 0;
    virtual bool exclusiveForegroundActive() const = 0;
    virtual QString focusModeName() const = 0;
    // Applications of the policy through the API seam, including the session's
    // start-up background application. A caller compares this before and after
    // its own actions to show requests do not accumulate.
    virtual int focusTransitionCount() const = 0;
    // Is a GameInput runtime attached at all? False means every request in this
    // session is refused, which the overlay records instead of implying a policy.
    virtual bool policyAttached() const = 0;
};

}  // namespace ModernInput
