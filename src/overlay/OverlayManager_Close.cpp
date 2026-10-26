// OverlayManager: the close sequence (hide paths, neutral handoff, policy release).
#include "overlay/OverlayManager.h"
#include "overlay/OverlayManagerInternal.h"

#include "input/InputDiagnostics.h"
#include "gameinput/GameInputFocusPolicy.h"
#include "gameinput/IsolationCapability.h"
#include "overlay/ForegroundAcquirer.h"
#include "overlay/ForegroundApi.h"
#include "overlay/OverlayFocusTrace.h"
#include "overlay/OverlayLifetimePolicy.h"
#include "overlay/OverlayPresenter.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QScreen>
#include <QStringList>
#include <QTimer>
#include <QDebug>

#include <windows.h>
#include <dwmapi.h>

using namespace OverlayManagerInternal;

// cpo-o06c: ONE release point. Every exit path in this class funnels through
// completeHide(), so the exclusive policy cannot outlive the interactive state it
// was granted for — including the paths that hide because the game itself went
// away. The reason is derived from the same Windows facts the close record
// already gathered, so it cannot drift from the state that caused it.
//
// cpo-o06e: when the neutral handoff ran, it already restored the policy — while
// the pad was held and before the foreground moved, which is the whole point of
// the ordering — so this function only records what that release did. It never
// releases a second time.
void OverlayManager::releaseGameInputPolicy(OverlayFocus::HideTrace& trace, bool gameAlive,
                                            bool gameIconic, bool overlayOwnedForeground,
                                            bool desktopHandoff,
                                            const ModernInput::NeutralHandoffRunner::Outcome& handoff)
{
    OverlayFocus::GameInputPolicyFacts& facts = trace.gameInputPolicy;

    // The close line carries the handoff whatever its outcome: the wait's
    // numbers when it ran, and the fact that refused it when it did not. The
    // runner owns the wording, so the record and the diagnostics timeline can
    // never spell a stage two ways.
    trace.neutralHandoff = m_releaseHandoff->receiptText();

    if (!m_focusPolicySink) {
        facts.mode = QStringLiteral("uncontrolled");
        facts.request = QStringLiteral("not-engaged");
        facts.reason = QStringLiteral("no GameInput focus-policy owner in this session");
        return;
    }

    if (handoff.released) {
        facts.mode = m_focusPolicySink->focusModeName();
        facts.request = QStringLiteral("released");
        facts.reason = handoff.releaseReason;
        facts.transitions = handoff.releaseTransitions;
        return;
    }

    // Read the state BEFORE releasing: the record has to say whether this close
    // actually released something or found the policy already at background.
    const bool engaged = m_focusPolicySink->exclusiveForegroundActive();
    const int transitionsBefore = m_focusPolicySink->focusTransitionCount();
    const QString reason = OverlayFocus::gameInputRestoreReason(
        gameAlive, gameIconic, overlayOwnedForeground, desktopHandoff);
    // Idempotent by contract, so every exit path may call it unconditionally.
    m_focusPolicySink->restoreBackground(reason);
    facts.mode = m_focusPolicySink->focusModeName();
    facts.request = engaged ? QStringLiteral("released") : QStringLiteral("not-engaged");
    facts.reason = reason;
    facts.transitions = m_focusPolicySink->focusTransitionCount() - transitionsBefore;
}

void OverlayManager::hide()
{
    hideInternal();
}

void* OverlayManager::hideForDesktopHandoff()
{
    // Only a window that still exists can be a return address for the desktop
    // window: handing a destroyed HWND to the focus acquirer would fail
    // silently, so the caller falls back to the current foreground instead.
    const HWND remembered = static_cast<HWND>(m_previousForeground);
    const bool usable = isVisible() && remembered && IsWindow(remembered);
    void* previous = usable ? m_previousForeground : nullptr;
    // The desktop window is about to take the foreground: handing it back to
    // the game here would make the two fight over it.
    hideInternal(ForegroundReturn::LeaveAlone);
    return previous;
}

void OverlayManager::hideInternal(ForegroundReturn returnPolicy)
{
    if (!isVisible())
        return;

    // cpo-o06e: one close at a time. The handoff is asynchronous, and every
    // other exit path (a repeated hide, the foreground event, the game-context
    // watch) can arrive while it runs; each would otherwise restart the wait or
    // release the policy a second time.
    if (m_closeStage == CloseStage::ReleasingInput) {
        qInfo() << "Overlay: close already in progress - ignoring the repeated request";
        return;
    }

    // cpo-o06e: the release transition is decided in ONE place. The wait runs
    // when the handoff could actually protect something — the game is the next
    // owner — and the runner itself declines, with a receipt, when the exclusive
    // policy was never in force or no source can observe the pad. A close that
    // truly needs no wait completes synchronously.
    const QString blocked = handoffBlockedReason(returnPolicy);
    if (!blocked.isEmpty()) {
        m_releaseHandoff->noteSkipped(blocked);
        completeHide(returnPolicy);
        return;
    }

    m_closeStage = CloseStage::ReleasingInput;
    m_pendingReturnPolicy = returnPolicy;
    // The window stays up (it must keep the foreground, or the game would see
    // the held controls the wait is protecting it from) but stops drawing now.
    setClosing(true);
    m_releaseHandoff->begin(QStringLiteral("the overlay closed"));
}

void OverlayManager::setClosing(bool closing)
{
    if (m_closing == closing)
        return;
    m_closing = closing;
    emit closingChanged();
}

// cpo-o06e: may this close defer its policy release until the pad is neutral?
// An empty result means yes. Every refusal is a path on which nothing could be
// exposed by the release, so waiting would only delay the close:
//   * the desktop handoff hands the foreground to GameHQ's own window, not to
//     the game (the same call that must not fight the desktop window for focus);
//   * a game that is gone or minimized is not a hand-back target at all.
// The remaining two refusals (no policy in force, no source to observe with)
// belong to the runner, which reports them with the same receipt vocabulary
// instead of leaving a gap here.
QString OverlayManager::handoffBlockedReason(ForegroundReturn returnPolicy) const
{
    if (returnPolicy != ForegroundReturn::ToGame)
        return QStringLiteral("desktop handoff - the game is not the next owner");
    const HWND game = static_cast<HWND>(m_previousForeground);
    if (!game || !IsWindow(game) || IsIconic(game))
        return QStringLiteral("the game window is not a hand-back target");
    return QString();
}

void OverlayManager::onReleaseHandoffFinished()
{
    if (m_closeStage != CloseStage::ReleasingInput)
        return;   // a cancelled handoff or a stray signal: nothing to finish
    m_closeStage = CloseStage::Idle;
    completeHide(m_pendingReturnPolicy);
    // Cleared only after the hide, so no frame of the overlay is drawn again.
    setClosing(false);
}

void OverlayManager::completeHide(ForegroundReturn returnPolicy)
{
    if (!isVisible())
        return;
    if (m_probeTimer)
        m_probeTimer->stop();

    // A show acquisition still retrying must not outlive the window it was
    // acquiring for. Cancelling settles its record at "not acquired", which is
    // the truth: it never finished.
    m_focusAcquirer->cancel();
    if (m_pendingShowTrace)
        finishShowTrace(false, 0);

    // cpo-o06a: the close record is gathered around the hide, not after it —
    // m_previousForeground is cleared below, and the provider can change while
    // the window goes away.
    const InputDiagnostics& diagnostics = InputDiagnostics::instance();
    auto owned = std::make_unique<OverlayFocus::HideTrace>();
    OverlayFocus::HideTrace& trace = *owned;
    const HWND foregroundBefore = GetForegroundWindow();
    const HWND overlayHwnd = m_window ? reinterpret_cast<HWND>(m_window->winId()) : nullptr;
    const HWND game = static_cast<HWND>(m_previousForeground);
    trace.foregroundBefore = foregroundBefore;
    trace.restoreTarget = m_previousForeground;
    trace.providerBefore = diagnostics.servingProvider();

    // cpo-o06b: having taken the foreground, give it back — but only when the
    // overlay is the window still holding it. If anything else owns it the
    // user has already moved on (Alt-Tab, the shell, another app), and pulling
    // focus back to the game would be exactly the ping-pong this must never
    // do. A minimized or destroyed game is never a target either.
    const bool overlayOwnsForeground = overlayHwnd && foregroundBefore == overlayHwnd;
    const bool returnToGame = returnPolicy == ForegroundReturn::ToGame && overlayOwnsForeground
        && game && IsWindow(game) && !IsIconic(game);

    // cpo-o06c: release the exclusive GameInput policy BEFORE the foreground goes
    // back, so the game returns to a normal policy rather than to a narrowed one.
    releaseGameInputPolicy(trace, game && IsWindow(game), game && IsIconic(game),
                           overlayOwnsForeground,
                           returnPolicy == ForegroundReturn::LeaveAlone,
                           m_releaseHandoff->outcome());

    m_window->hide();
    m_groupWithGame = false;
    syncGameWindowOwner();
    // The window goes back to non-activating for its next open; the style is
    // re-written by the next present(), on whatever handle exists then.
    m_presenter->resetActivationPolicy();
    m_watchGameContext = false;
    m_gameContextLostTicks = 0;
    m_previousForeground = nullptr;
    m_previousForegroundPid = 0;

    trace.restoreRequested = returnToGame;
    trace.providerAfter = diagnostics.servingProvider();

    // A closed overlay makes no isolation claim; clear any stale warning.
    if (!m_foregroundAcquired) {
        m_foregroundAcquired = true;
        emit foregroundAcquiredChanged();
    }
    emit visibleChanged();

    if (returnToGame) {
        qInfo() << "Overlay: hidden, handing the foreground back to the game";
        m_pendingHideTrace = std::move(owned);
        m_focusAcquirer->acquire(game, kHidePhase);
        return;
    }
    qInfo() << "Overlay: hidden without changing foreground";
    m_pendingHideTrace = std::move(owned);
    finishHideTrace(false);
}

// Completes the close record. `restored` is the acquirer's verified result
// when a hand-back was requested; without one it is false and the line still
// reports where the foreground actually ended up.
void OverlayManager::finishHideTrace(bool restored)
{
    if (!m_pendingHideTrace)
        return;
    const std::unique_ptr<OverlayFocus::HideTrace> owned = std::move(m_pendingHideTrace);
    OverlayFocus::HideTrace& trace = *owned;

    trace.foregroundAfter = GetForegroundWindow();
    // Both halves have to agree: the acquirer says it moved the foreground,
    // and the foreground really is the window we remembered.
    trace.restored = restored && trace.restoreTarget != nullptr
        && trace.foregroundAfter == trace.restoreTarget;
    trace.providerAfter = InputDiagnostics::instance().servingProvider();

    const QString traceLine = trace.toLogString();
    qInfo().noquote() << "Overlay focus trace (close):" << traceLine;
    if (trace.restoreRequested && !trace.restored)
        qWarning().noquote() << "Overlay: the game did NOT get the foreground back |" << traceLine;
    // cpo-x01: closing the overlay is a real transition the export shows; it
    // makes no foreground claim, so no preservation value is recorded.
    InputDiagnostics::instance().noteOverlayHide(traceLine);
}
