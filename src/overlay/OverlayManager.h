#pragma once
#include <QObject>
#include <QPointer>
#include <QString>

#include "gameinput/NeutralHandoffRunner.h"

#include <memory>

class QQmlApplicationEngine;
class QQuickWindow;
class QScreen;
class OverlayPresenter;
class ForegroundAcquirer;
class ForegroundApi;

namespace ModernInput
{
class GameInputFocusRequestSink;
class NeutralHandoffSource;
}

namespace OverlayFocus
{
struct ShowTrace;
struct HideTrace;
}  // namespace OverlayFocus

// In-game overlay window lifecycle (docs/overlay.md): lazy-loads
// OverlayWindow.qml, shows it frameless/topmost over the active app and
// remembers the game it covers.
//
// cpo-o06b: presentation is still non-activating, but it is now followed by an
// explicit, bounded foreground request — the overlay asks to become the active
// window so keyboard and controller input belong to it, while the game stays
// visible and un-minimized behind it. The game window itself is never touched:
// no minimize, no restore, no restyle, and nothing is injected into it. When
// the request is denied the overlay simply stays as presentation left it, which
// is exactly the behaviour that shipped before.
//
// cpo-o06c: while that interactive state holds, the overlay may also ask the
// GameInput runtime to make foreground input exclusive to it — and must give the
// policy back on every exit path. The overlay only REQUESTS: the process-wide
// policy has exactly one owner (GameInputFocusController, installed by the app),
// which decides the flags, applies them and records the transition. A request
// that never happens because the truth condition did not hold is recorded with
// the clause that refused it.
class OverlayManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool visible READ isVisible NOTIFY visibleChanged)
    // cpo-o06b: observed, not intended. While the overlay is open this is the
    // acquirer's verified result (it re-reads the OS foreground); a denied
    // request leaves it false and the in-overlay warning visible, because the
    // game really can still be reading the pad. True while closed makes no
    // claim — it only keeps the warning out of a closed overlay.
    Q_PROPERTY(bool foregroundAcquired READ foregroundAcquired NOTIFY foregroundAcquiredChanged)

public:
    explicit OverlayManager(QQmlApplicationEngine* engine, QObject* parent = nullptr);
    // Test seam: takes ownership of `foregroundApi`, so a session that cannot
    // move the real foreground (or must refuse to) can still be exercised.
    OverlayManager(QQmlApplicationEngine* engine, ForegroundApi* foregroundApi,
                   QObject* parent = nullptr);
    ~OverlayManager() override;

    bool isVisible() const;
    bool foregroundAcquired() const { return m_foregroundAcquired; }

    // cpo-o06c: the one owner of the process-wide GameInput focus policy. The
    // app installs it at startup (and it outlives the overlay). Left null — as in
    // the overlay-only tests — every policy request is recorded as refused
    // instead of silently pretending one happened.
    void setGameInputFocusRequestSink(ModernInput::GameInputFocusRequestSink* sink);

    // cpo-o06e: the input layer that can say whether the pad is neutral, and
    // that stops producing overlay actions while the release handoff runs. The
    // app installs it at startup (InputEngine). Left null — as in the
    // overlay-only tests — the handoff is recorded as not engaged instead of
    // being faked: an unmeasurable wait must not exist.
    void setNeutralHandoffSource(ModernInput::NeutralHandoffSource* source);

    Q_INVOKABLE void toggle();
    Q_INVOKABLE void show();
    Q_INVOKABLE void hide();

    // Desktop-window summon (hold PS): the overlay gets out of the way but
    // must NOT hand focus back to the game — the desktop window is about to
    // take it. Returns the window the overlay had remembered (null when it
    // was not visible) so the caller can restore focus there later.
    void* hideForDesktopHandoff();

    // Called from the WinEvent hook callback (see .cpp) whenever the OS
    // foreground window changes to something other than the overlay itself
    // while the overlay is visible — Win key (Start menu), Alt-Tab, the task
    // switcher, or a click on another app all land here. Public so the free
    // function callback can reach it; not meant for QML/general use.
    void onForegroundWindowChanged(void* newForeground);

signals:
    void aboutToShow();
    void visibleChanged();
    void foregroundAcquiredChanged();

private:
    // Whether closing should hand the foreground back to the game. It must not
    // when the overlay no longer owns the foreground (something else took it —
    // pulling it back would fight the user), and not for the desktop handoff,
    // where GameHQ's main window is about to take focus instead.
    enum class ForegroundReturn
    {
        ToGame,
        LeaveAlone,
    };

    bool ensureLoaded();
    void hideInternal(ForegroundReturn returnPolicy = ForegroundReturn::ToGame);
    // cpo-o06e: the close has two stages once a neutral handoff is possible.
    // ReleasingInput is bounded by the handoff runner and ends in
    // onReleaseHandoffFinished; Idle is every other moment.
    enum class CloseStage
    {
        Idle,
        ReleasingInput,
    };
    // Why this close does not defer its policy release — empty means it does.
    // The reasons are all "nothing could leak through this handoff anyway".
    QString handoffBlockedReason(ForegroundReturn returnPolicy) const;
    // The rest of the close: trace facts, the policy release (already done when
    // the handoff ran), the window hide and the foreground hand-back.
    void completeHide(ForegroundReturn returnPolicy);
    void onReleaseHandoffFinished();
    // cpo-o06c: the ONE release point. Every close path funnels through
    // completeHide(), so the exclusive policy cannot outlive the interactive
    // state it was granted for. Fills the trace's policy and handoff facts.
    void releaseGameInputPolicy(OverlayFocus::HideTrace& trace, bool gameAlive, bool gameIconic,
                                bool overlayOwnedForeground, bool desktopHandoff,
                                const ModernInput::NeutralHandoffRunner::Outcome& handoff);
    // cpo-o06b: the open/close records are completed once the bounded
    // foreground request has settled — its retries are asynchronous, so a
    // synchronous line would state a result that had not happened yet.
    void onForegroundAcquisitionFinished(const QString& phase, void* target,
                                         bool acquired, int attempts);
    void finishShowTrace(bool acquired, int attempts);
    void finishHideTrace(bool restored);
    // cpo-o06c: the policy side of an open and of a close, kept in one place each
    // so every exit path releases the same way and no path can forget to.
    void askForExclusiveGameInputPolicy(OverlayFocus::ShowTrace& trace);
    void startShowProbe();
    void probeTick();
    void syncGameWindowOwner();

    // cpo-o03: the lifetime decision is pure (overlay/OverlayLifetimePolicy.h);
    // this class only resolves Win32 facts for it and performs the effects
    // through the existing presenter.
    class LifetimeActions;
    // The screen the overlay should cover for `gameWindow`: the game's monitor
    // when that handle still exists, the primary screen otherwise. A stale
    // HWND can therefore never drive overlay geometry.
    QScreen* targetScreenForGameWindow(void* gameWindow) const;
    bool gameWindowMovedToAnotherMonitor(void* gameWindow) const;
    void rebindGameWindow(void* newWindow);
    void reassertOverlay();
    void repositionOverlay();

    QQmlApplicationEngine* m_engine;
    QPointer<QQuickWindow> m_window;
    // The one production path that makes the overlay visible, positioned and
    // topmost; it owns the never-activate guarantee (docs/overlay.md).
    std::unique_ptr<OverlayPresenter> m_presenter;
    std::unique_ptr<LifetimeActions> m_lifetimeActions;
    void* m_previousForeground = nullptr;   // HWND of the game/app under us
    // Process id of the window above. It is the continuity evidence that keeps
    // the overlay open when a game replaces its own window, and the reason a
    // destroyed HWND is never remembered: a pid outlives a handle.
    unsigned long m_previousForegroundPid = 0;
    bool m_groupWithGame = false;
    bool m_ownerTopmostRepaired = false;
    void* m_focusHook = nullptr;            // HWINEVENTHOOK, opaque here to avoid <windows.h> in the header
    bool m_foregroundAcquired = true;
    // cpo-o06b: one bounded acquisition at a time, shared by the show request
    // and the hand-back on close (the phase string tells them apart).
    std::unique_ptr<ForegroundAcquirer> m_focusAcquirer;
    std::unique_ptr<OverlayFocus::ShowTrace> m_pendingShowTrace;
    std::unique_ptr<OverlayFocus::HideTrace> m_pendingHideTrace;
    // cpo-o06c: non-owning. The app owns the single GameInput focus-policy owner
    // and keeps it alive for longer than this window manager.
    ModernInput::GameInputFocusRequestSink* m_focusPolicySink = nullptr;
    // cpo-o06e: the close's release transition. Always constructed — a close
    // with no source or no policy still records why no handoff ran — and owned
    // here, because the overlay is the one that closes.
    std::unique_ptr<ModernInput::NeutralHandoffRunner> m_releaseHandoff;
    CloseStage m_closeStage = CloseStage::Idle;
    ForegroundReturn m_pendingReturnPolicy = ForegroundReturn::ToGame;
    // Logged once per open, not per foreground event: the lifetime rules can
    // see our own overlay many times while it is up.
    bool m_loggedOverlayForeground = false;

    // Diagnostic probe: for a few seconds after show() the game window's
    // state is sampled and every change is logged, so a game that hides or
    // minimizes itself in reaction to the overlay leaves evidence in the log.
    // After that window it keeps running at a slower rate as the game-context
    // watch (cpo-o06b): while the overlay owns the foreground, a game that
    // loses its window produces no foreground event, so the only way to notice
    // is to look.
    class QTimer* m_probeTimer = nullptr;
    int m_probeElapsedMs = 0;
    QString m_probeLastState;
    // Only watch a context that existed: the overlay opened over nothing (no
    // foreground window at all) must not close itself immediately.
    bool m_watchGameContext = false;
    // Consecutive watch ticks that saw the game gone. A game recreating its
    // own window is gone for an instant, and that must rebind, not hide.
    int m_gameContextLostTicks = 0;
};
