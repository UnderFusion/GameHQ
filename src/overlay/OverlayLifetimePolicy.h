#pragma once

// cpo-o03: one pure decision for "what does this OS foreground change mean for
// the overlay?", plus the smallest mapping from that decision to the effects
// the manager performs. Every Win32 query lives in OverlayManager; this unit
// stays free of <windows.h> and of QML, so the lifetime rules can be exercised
// without a desktop session, a game, or an overlay window
// (tests/tst_overlaylifetime.cpp).
namespace OverlayLifetime
{

// One foreground change with every OS query already resolved to plain facts.
//
// `sameProcessAsGame` is process identity — the pid remembered when the
// overlay opened — not a second handle comparison: a game that recreates its
// window is still the same context, and a pid survives the old handle being
// destroyed. It is deliberately false when that pid is GameHQ's own, so a
// foreground change between our own windows can never read as "the game".
//
// The second block is the health of the window we CURRENTLY remember as the
// game's, gathered in the same pass. Without it the rules cannot tell a
// genuine replacement (the remembered handle is gone) from a bystander of the
// same process (the game's own window is still right there) — and treating a
// bystander as the replacement is how the overlay would end up measuring and
// following the wrong HWND.
struct ForegroundFacts
{
    bool validWindow = false;        // IsWindow(newForeground)
    bool isOverlay = false;          // it is the overlay's own HWND
    bool isRememberedGame = false;   // it is the game window we remembered
    bool sameProcessAsGame = false;  // its pid is the remembered game's pid
    bool visible = false;            // IsWindowVisible(newForeground)
    bool iconic = false;             // IsIconic(newForeground): minimized
    bool topLevel = false;           // GetAncestor(GA_ROOT) is the window itself
    bool owned = false;              // has an owner: an auxiliary/owned popup

    bool rememberedGameAlive = false;     // IsWindow(remembered)
    bool rememberedGameVisible = false;   // IsWindowVisible(remembered)
    bool rememberedGameIconic = false;    // IsIconic(remembered): minimized
    bool rememberedGameTopLevel = false;  // still shaped like a main window
    bool rememberedGameOwned = false;     // ... and still unowned
};

enum class Decision
{
    Ignore,      // our own foreground, or an event we must not act on
    Keep,        // still inside the game's context: leave the overlay alone
    RebindGame,  // a same-process replacement qualified as the game's window
    Hide,        // the game's context is genuinely gone: hide the overlay
};

const char* decisionName(Decision decision);

// A queued foreground event is acted on only while it still describes the
// current foreground: equality decides, INCLUDING nullptr == nullptr. That
// moment — Windows having no foreground window at all while the game
// minimizes or is destroyed — is precisely a lost context, and it must reach
// `decide()` as invalid foreground facts (Hide) instead of being dropped as
// if the overlay still had a context. A non-null event whose window is no
// longer the foreground is stale and must be ignored.
bool isCurrentForegroundEvent(const void* eventWindow, const void* currentForeground);

// Process identity is evidence of continuity, not permission to ignore a real
// loss of game context. Hiding wins whenever the game's own window is
// minimized or hidden, and whenever the foreground belongs to another process
// (desktop, shell, task switcher, another application). A same-process
// top-level window replaces the remembered game window only when that
// remembered handle is GONE; while it is still a healthy visible main window a
// second same-process window is a bystander that must NOT take its place, and
// when the remembered window merely lost its screen (minimized/hidden) no
// same-process bystander may keep the overlay alive. An owned/auxiliary
// same-process popup counts as still being inside the game context but must
// never replace the remembered window.
Decision decide(const ForegroundFacts& facts);

// cpo-o06b: the same loss of context, detected by looking instead of by being
// told. While the overlay itself owns the OS foreground, a game that hides,
// minimizes or destroys its window produces NO foreground event — nothing
// changed foreground, because we already had it — so `decide()` is never
// asked. Only the remembered-window fields are consulted; who owns the
// foreground is deliberately irrelevant here.
//
// This is a question, not a decision: the caller is expected to require the
// answer to hold for a short, bounded while before hiding, so a game that
// destroys and immediately recreates its window gets to rebind first.
bool rememberedGameContextLost(const ForegroundFacts& facts);

// What a rebind carries: the new game window, and whether it sits on another
// monitor than the overlay is covering right now.
struct RebindRequest
{
    void* newWindow = nullptr;
    bool otherMonitor = false;
};

// The effects a decision has on the overlay. The manager implements every slot
// through the existing OverlayPresenter; nothing here touches a window itself,
// so no second presentation path can appear next to it.
struct Actions
{
    virtual ~Actions() = default;
    virtual void rebindGameWindow(void* newWindow) = 0;
    virtual void reassertOverlay() = 0;    // presenter->reassert()
    virtual void repositionOverlay() = 0;  // presenter->present(new geometry)
    virtual void hideOverlay() = 0;
};

// Ignore/Keep do nothing. RebindGame always rebinds the remembered window, then
// re-asserts through the presenter — or, when the replacement landed on
// another monitor, presents to that monitor's geometry so the overlay follows
// the game instead of staying behind. Hide hides.
void apply(Decision decision, const RebindRequest& rebind, Actions& actions);

// Index into `screenMonitorIds` of the monitor `gameMonitorId`, or -1 when
// there is none. The caller falls back to the primary screen, so a stale or
// unknown game window can never move the overlay to an invented monitor.
int screenIndexForMonitor(const void* gameMonitorId,
                          const void* const* screenMonitorIds,
                          int count);

}  // namespace OverlayLifetime
