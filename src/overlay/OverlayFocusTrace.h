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

    // What this record proves about controller isolation — which is nothing.
    // Foreground ownership is a precondition GameHQ can observe; whether the
    // game stopped receiving the pad can only be measured outside this
    // process (cpo-o06d). Stated here so no reader mistakes a successful
    // foreground grab for isolation.
    static QString isolationEvidence() { return QStringLiteral("not measured in-process"); }

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

    // Filled once the neutral-state handoff exists (cpo-o06e). Until then it
    // says so, rather than implying a handoff that never ran.
    QString neutralHandoff = QStringLiteral("not implemented");

    QString providerBefore;
    QString providerAfter;

    bool providerChanged() const { return providerBefore != providerAfter; }

    QString toLogString() const;
};

// "none" for nullptr, "0x1a2b3c" otherwise. One formatter, so the show line,
// the hide line and the diagnostics export cannot drift apart.
QString formatHandle(const void* handle);

}  // namespace OverlayFocus
