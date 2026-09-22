#include "overlay/OverlayLifetimePolicy.h"

namespace OverlayLifetime
{

const char* decisionName(Decision decision)
{
    switch (decision) {
    case Decision::Ignore:
        return "ignore";
    case Decision::Keep:
        return "keep";
    case Decision::RebindGame:
        return "rebind_game";
    case Decision::Hide:
        return "hide";
    }
    return "unknown";
}

bool isCurrentForegroundEvent(const void* eventWindow, const void* currentForeground)
{
    // Deliberately the whole rule: a queued event is current exactly while it
    // is still the foreground. Neither side gets special-cased, so the genuine
    // "no foreground window" event (both null) arrives at `decide()` and hides
    // the overlay instead of being dropped like a stale one.
    return eventWindow == currentForeground;
}

namespace
{
// The remembered window is still doing the job it was remembered for: it
// exists, shows a screen (visible, not minimized) and is still shaped like a
// main window (top-level, unowned). Anything less means the game is not
// showing us its window right now.
bool rememberedGameUsable(const ForegroundFacts& facts)
{
    return facts.rememberedGameAlive && facts.rememberedGameVisible
        && !facts.rememberedGameIconic && facts.rememberedGameTopLevel
        && !facts.rememberedGameOwned;
}
}  // namespace

Decision decide(const ForegroundFacts& facts)
{
    // The overlay's own foreground during show() is expected, not a focus loss.
    if (facts.isOverlay)
        return Decision::Ignore;

    // A handle that no longer exists cannot be a context. Hiding rather than
    // remembering it is what keeps a destroyed HWND out of geometry and out of
    // the desktop-handoff return address.
    if (!facts.validWindow)
        return Decision::Hide;

    // The remembered game window is foreground again: nothing to do — unless
    // the game has genuinely minimized or hidden its own window, which is a
    // real loss of game context regardless of who owns the foreground.
    if (facts.isRememberedGame)
        return (facts.iconic || !facts.visible) ? Decision::Hide : Decision::Keep;

    if (facts.sameProcessAsGame) {
        // Same process, but minimized or hidden: the game is out of sight, so
        // the overlay must not stay over whatever ends up next in z-order.
        if (facts.iconic || !facts.visible)
            return Decision::Hide;

        // An owned or non-top-level window is an auxiliary popup: still inside
        // the game's context, but never the remembered game window.
        if (!facts.topLevel || facts.owned)
            return Decision::Keep;

        // A top-level, unowned window of the game's process COULD be its main
        // window — but it replaces the remembered one only when that one is
        // gone. While the remembered window is still a healthy visible main
        // window, this candidate is a second window of the same game (launcher
        // surface, splash, tool window): the overlay stays and keeps measuring
        // the remembered handle.
        if (rememberedGameUsable(facts))
            return Decision::Keep;

        // The remembered window still exists but no longer shows the game
        // (minimized, hidden, demoted): that is a genuine loss of game
        // context, and no same-process bystander may silently take the
        // remembered window's place and keep the overlay alive.
        if (facts.rememberedGameAlive)
            return Decision::Hide;

        // The old handle is destroyed: the pid is the continuity evidence, and
        // this is the game's new main window (recreation, resolution change, a
        // launcher handing over).
        return Decision::RebindGame;
    }

    // Desktop, shell, task switcher or another application owns the
    // foreground: the game's context is gone.
    return Decision::Hide;
}

bool rememberedGameContextLost(const ForegroundFacts& facts)
{
    // Deliberately the same three conditions `decide()` treats as a lost game
    // context for the remembered window — destroyed, hidden or minimized — so
    // the polled path and the event path cannot drift into disagreeing about
    // what "the game is gone" means. Shape (top-level/owned) is NOT part of
    // it: a game briefly reparenting its own window must not be read as a loss.
    return !facts.rememberedGameAlive || !facts.rememberedGameVisible
        || facts.rememberedGameIconic;
}

void apply(Decision decision, const RebindRequest& rebind, Actions& actions)
{
    switch (decision) {
    case Decision::Ignore:
    case Decision::Keep:
        return;
    case Decision::RebindGame:
        actions.rebindGameWindow(rebind.newWindow);
        // Keep the never-activate guarantee on the presentation that is
        // already up; when the game moved to another monitor the overlay has
        // to follow it, which is a presentation, not a bare move.
        if (rebind.otherMonitor)
            actions.repositionOverlay();
        else
            actions.reassertOverlay();
        return;
    case Decision::Hide:
        actions.hideOverlay();
        return;
    }
}

int screenIndexForMonitor(const void* gameMonitorId,
                          const void* const* screenMonitorIds,
                          int count)
{
    if (!gameMonitorId || !screenMonitorIds || count <= 0)
        return -1;
    for (int index = 0; index < count; ++index) {
        if (screenMonitorIds[index] && screenMonitorIds[index] == gameMonitorId)
            return index;
    }
    return -1;
}

}  // namespace OverlayLifetime
