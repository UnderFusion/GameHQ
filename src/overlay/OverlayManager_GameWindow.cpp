// OverlayManager: game window ownership and the foreground lifetime decision.
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

void OverlayManager::syncGameWindowOwner()
{
    if (!m_window)
        return;
    const HWND overlay = reinterpret_cast<HWND>(m_window->winId());
    const HWND game = static_cast<HWND>(m_previousForeground);
    const HWND owner = m_groupWithGame && game && IsWindow(game)
            && processIdOfWindow(game) == m_previousForegroundPid
            && m_previousForegroundPid != GetCurrentProcessId()
        ? game : nullptr;
    if (!IsWindow(overlay) || GetWindow(overlay, GW_OWNER) == owner)
        return;
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR previous = SetWindowLongPtrW(overlay, GWLP_HWNDPARENT,
                                               reinterpret_cast<LONG_PTR>(owner));
    const DWORD error = GetLastError();
    if (!previous && error != ERROR_SUCCESS) {
        qWarning() << "Overlay: could not associate our popup with game, error" << error;
        return;
    }
    qInfo().noquote() << "Overlay: popup owner=" << OverlayFocus::formatHandle(owner);
}

// --- lifetime decision (cpo-o03) ------------------------------------------
// Windows facts are resolved here; the rules themselves are pure
// (overlay/OverlayLifetimePolicy.h, tests/tst_overlaylifetime.cpp), and every
// effect goes through the presenter via OverlayManager::LifetimeActions — the
// same single presentation path as before, not a second one.

QScreen* OverlayManager::targetScreenForGameWindow(void* gameWindow) const
{
    QScreen* target = QGuiApplication::primaryScreen();
    const HWND game = static_cast<HWND>(gameWindow);
    if (!game || !IsWindow(game))
        return target;  // a stale or absent game handle never drives geometry

    const HMONITOR monitor = MonitorFromWindow(game, MONITOR_DEFAULTTOPRIMARY);
    const auto screens = QGuiApplication::screens();
    QList<const void*> screenMonitors;
    screenMonitors.reserve(screens.size());
    for (QScreen* screen : screens) {
        const QPoint center = screen->geometry().center();
        screenMonitors.append(reinterpret_cast<const void*>(
            MonitorFromPoint(POINT{ center.x(), center.y() }, MONITOR_DEFAULTTONULL)));
    }
    const int index = OverlayLifetime::screenIndexForMonitor(
        reinterpret_cast<const void*>(monitor), screenMonitors.constData(),
        static_cast<int>(screenMonitors.size()));
    if (index >= 0)
        target = screens.at(index);
    return target;
}

bool OverlayManager::gameWindowMovedToAnotherMonitor(void* gameWindow) const
{
    if (!m_window)
        return false;
    const QScreen* target = targetScreenForGameWindow(gameWindow);
    return target && target != m_window->screen();
}

void OverlayManager::rebindGameWindow(void* newWindow)
{
    // A game that replaces its own window (borderless/fullscreen toggle, a
    // resolution change, a launcher handing over) must not dismiss the
    // overlay: the process id proves it is still the same game.
    m_previousForeground = newWindow;
    m_previousForegroundPid = processIdOfWindow(static_cast<HWND>(newWindow));
    syncGameWindowOwner();
    qInfo().noquote() << QStringLiteral(
        "Overlay: game window replaced by 0x%1 (pid %2) — rebinding, overlay stays open")
        .arg(QString::number(reinterpret_cast<qulonglong>(newWindow), 16))
        .arg(m_previousForegroundPid);
}

void OverlayManager::reassertOverlay()
{
    if (!m_presenter)
        return;
    syncGameWindowOwner();
    const OverlayPresentReport report = m_presenter->reassert();
    qInfo().noquote() << "Overlay: re-asserted after the game window changed |"
                      << report.toLogString();
}

void OverlayManager::repositionOverlay()
{
    if (!m_presenter)
        return;
    QScreen* target = targetScreenForGameWindow(m_previousForeground);
    if (!target)
        return;
    const OverlayPresentReport report = m_presenter->present(target->geometry());
    qInfo().noquote() << "Overlay: followed the game to another monitor |"
                      << report.toLogString();
}

void OverlayManager::onForegroundWindowChanged(void* newForeground)
{
    if (!isVisible())
        return;
    // Out-of-context WinEvents are queued: an earlier away event can arrive
    // after show() has already acquired focus. Only an event that still
    // describes the current foreground is acted on; a mismatch means it went
    // stale while it waited. Both being null is NOT a mismatch: "no foreground
    // window" is a genuine moment during a minimize/destruction transition and
    // must reach the policy, whose invalid-foreground rule hides the overlay.
    const HWND foreground = GetForegroundWindow();
    if (!OverlayLifetime::isCurrentForegroundEvent(newForeground,
                                                   reinterpret_cast<void*>(foreground)))
        return;

    OverlayLifetime::ForegroundFacts facts;
    facts.validWindow = IsWindow(foreground) != FALSE;
    facts.isOverlay = foreground == reinterpret_cast<HWND>(m_window->winId());
    facts.isRememberedGame = foreground == static_cast<HWND>(m_previousForeground);
    facts.visible = facts.validWindow && IsWindowVisible(foreground) != FALSE;
    facts.iconic = facts.validWindow && IsIconic(foreground) != FALSE;
    facts.topLevel = facts.validWindow && GetAncestor(foreground, GA_ROOT) == foreground;
    facts.owned = facts.validWindow && GetWindow(foreground, GW_OWNER) != nullptr;
    const unsigned long pid = processIdOfWindow(foreground);
    // Process identity is the continuity evidence — but never our own process:
    // a foreground change between GameHQ's own windows must not read as "the
    // game replaced its window".
    facts.sameProcessAsGame = pid != 0 && pid == m_previousForegroundPid
        && pid != GetCurrentProcessId();

    // The window we remember, sampled at this same moment: the decision must
    // tell a genuine replacement (that handle is gone) from a second window of
    // the same game (that handle is still healthy) and from a game that
    // genuinely left the screen (it exists but shows nothing).
    const HWND remembered = static_cast<HWND>(m_previousForeground);
    facts.rememberedGameAlive = remembered && IsWindow(remembered);
    facts.rememberedGameVisible = facts.rememberedGameAlive && IsWindowVisible(remembered);
    facts.rememberedGameIconic = facts.rememberedGameAlive && IsIconic(remembered);
    facts.rememberedGameTopLevel = facts.rememberedGameAlive
        && GetAncestor(remembered, GA_ROOT) == remembered;
    facts.rememberedGameOwned = facts.rememberedGameAlive
        && GetWindow(remembered, GW_OWNER) != nullptr;

    OverlayLifetime::Decision decision = OverlayLifetime::decide(facts);

    // cpo-o06b: our own overlay becoming the foreground is the state variant B
    // exists to reach, not "the user left the game". The rules already say so
    // (isOverlay -> Ignore); this states it in the log once per open, so a
    // report shows the distinction was made rather than leaving it implied.
    // The remembered game window is untouched here: it stays the game context.
    if (facts.isOverlay && !m_loggedOverlayForeground) {
        m_loggedOverlayForeground = true;
        qInfo().noquote() << QStringLiteral(
            "Overlay: foreground is our own overlay 0x%1 — interactive state, overlay stays "
            "open (game context still 0x%2 pid %3)")
            .arg(QString::number(reinterpret_cast<qulonglong>(foreground), 16))
            .arg(QString::number(reinterpret_cast<qulonglong>(m_previousForeground), 16))
            .arg(m_previousForegroundPid);
    }

    OverlayLifetime::RebindRequest rebind;
    if (decision == OverlayLifetime::Decision::RebindGame) {
        // Fact gathering and application are two moments, and the candidate
        // sits in between. If it died or changed process meanwhile, committing
        // it would leave the overlay remembering a dead HWND with pid 0 — for
        // geometry and for the desktop-handoff return address.
        const HWND candidate = static_cast<HWND>(newForeground);
        const unsigned long candidatePid = processIdOfWindow(candidate);
        if (candidatePid == 0 || candidatePid != m_previousForegroundPid) {
            qWarning() << "Overlay: rebind candidate no longer valid — hiding instead";
            decision = OverlayLifetime::Decision::Hide;
        } else {
            rebind.newWindow = newForeground;
            rebind.otherMonitor = gameWindowMovedToAnotherMonitor(newForeground);
        }
    }
    if (decision == OverlayLifetime::Decision::Ignore
        || decision == OverlayLifetime::Decision::Keep) {
        return;
    }
    qInfo().noquote() << QStringLiteral(
        "Overlay: foreground 0x%1 -> %2 (same_process=%3 visible=%4 iconic=%5)")
        .arg(QString::number(reinterpret_cast<qulonglong>(foreground), 16))
        .arg(QLatin1String(OverlayLifetime::decisionName(decision)))
        .arg(facts.sameProcessAsGame ? 1 : 0)
        .arg(facts.visible ? 1 : 0)
        .arg(facts.iconic ? 1 : 0);
    OverlayLifetime::apply(decision, rebind, *m_lifetimeActions);
}
