#pragma once

#include <QString>

// cpo-o06a: one bounded record of what Windows and the controller stack
// actually did around an overlay open or close.
//
// The isolation question — "does overlay navigation also reach the game?" —
// cannot be answered from the two lines the overlay logged before: they stated
// the policy GameHQ intended, not the facts Windows produced. This unit is the
// place those facts are assembled into a single line, and it is deliberately
// pure: every Win32 query happens in OverlayManager, so the formatting and the
// derived verdicts can be exercised without a desktop session
// (tests/tst_overlayfocustrace.cpp).
//
// Bounded by construction: one record per real open and one per real close.
// Nothing here is per-event or per-frame telemetry.
//
// Privacy: window handles and process ids only — the same class of value the
// overlay already logged. Controller fields are passed in ALREADY sanitized by
// InputDiagnostics (provider labels and hashed profile ids, never device
// paths or serials).
namespace OverlayFocus
{

// A window as Windows described it at one instant.
struct WindowFacts
{
    const void* handle = nullptr;
    bool exists = false;    // IsWindow()
    bool visible = false;   // IsWindowVisible()
    bool iconic = false;    // IsIconic(): minimized
    unsigned long pid = 0;
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    // "0x1a2b exists=1 visible=1 iconic=0 pid=1234 rect=0,0-2560,1440", or
    // "none" / "0x1a2b destroyed" when there is nothing to describe.
    QString toLogString() const;
};

// cpo-o06c: what GameHQ asked GameInput to do about the pad while the overlay
// was interactive, and why. `mode` is the policy in force as GameHQ knows it,
// `request` says what this open/close did to it (requested / refused /
// released / not-engaged), `reason` names the fact behind it.
//
// This is a statement about POLICY CONTROL only. The runtime has no failure
// return, and nothing in this process can observe whether another process
// actually stopped receiving input — see `isolation` below (cpo-o06f).
struct GameInputPolicyFacts
{
    QString mode = QStringLiteral("uncontrolled");
    QString request = QStringLiteral("none");
    QString reason;
    int transitions = 0;   // policy applications caused by this open/close

    bool exclusiveApplied() const
    {
        return mode == QLatin1String("exclusive-foreground");
    }
    bool requested() const { return request == QLatin1String("requested"); }
    QString toLogString() const;
};

// What one overlay open observed.
struct ShowTrace
{
    WindowFacts game;      // the window that owned the screen before we showed
    WindowFacts overlay;   // our own window, after presentation

    const void* foregroundBefore = nullptr;
    const void* foregroundAfterPresent = nullptr;
    // Set only once a variant actually asks for activation (cpo-o06b). While
    // the overlay stays non-activating this equals foregroundAfterPresent.
    const void* foregroundAfterActivation = nullptr;
    bool activationRequested = false;
    // How the explicit foreground request went (cpo-o06b). `succeeded` is the
    // acquirer's verified result — it re-reads GetForegroundWindow rather than
    // trusting what SetForegroundWindow reported.
    int acquisitionAttempts = 0;
    bool acquisitionSucceeded = false;

    // Both windows sampled AGAIN after the request settled. The game's row is
    // the half of the truth condition that a foreground check alone cannot
    // give: a game that minimized itself in reaction still leaves the overlay
    // owning the foreground.
    WindowFacts gameAfterAcquisition;
    WindowFacts overlayAfterAcquisition;

    // The lifetime rules saw our own overlay in the foreground and kept the
    // overlay open instead of treating it as "the user left the game".
    bool lifetimeAcceptedOverlayForeground = false;

    // Qt and Win32 can disagree: Qt believes the window is active while
    // Windows still reports another foreground, which is exactly the state
    // that made earlier focus attempts look like they had worked.
    bool overlayActiveQt = false;
    bool overlayForegroundWin32 = false;

    QString controllerProvider;      // "Sony controller", "GameInput shadow", ...
    QString controllerProfile;       // hashed logical profile id, never the raw one
    QString gameInputFocusPolicy;    // the policy GameInput is actually running under
    // cpo-o06c: what this open did to that policy, and why.
    GameInputPolicyFacts gameInputPolicy;

    // The game window still owned the foreground when presentation finished.
    bool foregroundPreserved() const { return foregroundBefore == foregroundAfterPresent; }
    // Whoever owns the foreground at the end of the open sequence.
    const void* finalForeground() const
    {
        return activationRequested ? foregroundAfterActivation : foregroundAfterPresent;
    }
    // The overlay ended up owning the OS foreground. Not a claim about the
    // controller: see isolationEvidence().
    bool overlayOwnsForeground() const
    {
        return overlay.handle != nullptr && finalForeground() == overlay.handle;
    }
    bool qtWin32Disagree() const { return overlayActiveQt != overlayForegroundWin32; }

    // cpo-o06b's whole acceptance question, as one boolean. Every clause is an
    // observed Windows fact: a successful API call proves none of them, and
    // three of the four would still be true if the game had vanished.
    bool interactiveForegroundTruth() const
    {
        return overlayOwnsForeground() && gameAfterAcquisition.exists
            && gameAfterAcquisition.visible && !gameAfterAcquisition.iconic
            && overlayAfterAcquisition.exists && overlayAfterAcquisition.visible;
    }

    // cpo-o06f: what this open may honestly claim about controller isolation,
    // classified by gameinput/IsolationCapability from facts the open observed
    // — the policy that ended up in force (`gameInputPolicy`), plus the evidence
    // recorded OUTSIDE this process. Composed by the show path after the policy
    // request settled, never by this formatter, so the record cannot claim a
    // mechanism that was refused. A record that carries no classification says
    // so: foreground ownership alone is not isolation (cpo-o06a), and no API
    // call in this process can prove a game stopped seeing the pad (cpo-o06d).
    QString isolation;

    QString toLogString() const;
};

// What one overlay close observed.
struct HideTrace
{
    const void* foregroundBefore = nullptr;
    const void* foregroundAfter = nullptr;
    const void* restoreTarget = nullptr;   // the game window we remembered
    bool restoreRequested = false;         // a variant asked for it (cpo-o06e)
    bool restored = false;                 // ... and the foreground really moved there

    // cpo-o06e: this close's release handoff, in the shared receipt vocabulary —
    // "passed duration_ms=12 polls=2", "timeout duration_ms=402 polls=41
    // held=\"gamepad.cross\"", or "not-engaged (…)" when nothing had to be
    // deferred. A close that never produced a receipt says exactly that, instead
    // of implying a handoff that never ran.
    QString neutralHandoff = QStringLiteral("no receipt (this close path ran no handoff)");

    QString providerBefore;
    QString providerAfter;

    bool providerChanged() const { return providerBefore != providerAfter; }

    // cpo-o06c: the exclusive GameInput policy is scoped to the interactive
    // overlay, so every close reports what happened to it and why.
    GameInputPolicyFacts gameInputPolicy;

    QString toLogString() const;
};

// "none" for nullptr, "0x1a2b3c" otherwise. One formatter, so the show line,
// the hide line and the diagnostics export cannot drift apart.
QString formatHandle(const void* handle);

// Pure decision for the whole of cpo-o06c: may the overlay ask GameInput for the
// exclusive-foreground policy, given what it measured while opening?
//
// It is gated on the state the open record calls interactiveForegroundTruth() —
// the overlay owns the foreground AND the game is alive, visible and un-minimized
// AND the overlay is still visible — PLUS the acquirer's own verdict, because a
// request that was denied or cancelled must not be followed by asking for a
// policy the overlay is about to have to give back. So:
//
//   request            => the verified interactive state held
//   verified state
//   + acquisition won  => request
//
// and the refusal names the clause that failed, because "foreground denied" and
// "the game minimized itself in reaction" are different findings for whoever
// reads the report. The refusal direction is the safe one: a request that never
// happens cannot narrow anybody's delivery, and the game keeps the pad exactly
// as it was.
struct GameInputPolicyDecision
{
    bool request = false;
    QString reason;   // request reason, or the fact that refused it
};

GameInputPolicyDecision decideGameInputPolicy(const ShowTrace& trace);

// Pure: why the exclusive policy is being released on this exit path, derived
// from the same Windows facts the close record already gathers (game window
// alive / minimized / the overlay still owning the foreground / the desktop
// handoff), so the reason cannot drift from the state that caused it.
QString gameInputRestoreReason(bool gameAlive, bool gameIconic,
                               bool overlayOwnedForeground, bool desktopHandoff);
}  // namespace OverlayFocus
