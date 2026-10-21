// OverlayManager: post-show diagnostic probe and the game-context watch.
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

namespace {
// Post-show probe: dense sampling while the game reacts to the overlay, then
// the same timer slows down and becomes the game-context watch.
constexpr int kProbeIntervalMs = 50;
constexpr int kProbeDetailMs = 3000;
constexpr int kWatchIntervalMs = 250;
// How long the game has to look gone before the overlay acts on it.
constexpr int kContextLostGraceMs = 750;
}  // namespace

// --- post-show diagnostic probe -------------------------------------------
// Every 2026-09-12 failure had the same shape: overlay shown with the game
// still foreground, then ~0.5 s later the game window was gone (foreground
// NULL or handed to whatever was next in z-order) although nothing in GameHQ
// touches the game window. This samples the game window for 3 s after show()
// and logs each state change with its timestamp, so the next repro tells us
// whether the game hides, minimizes, resizes or destroys its window.
namespace {
QString cloakedState(HWND hwnd)
{
    DWORD cloaked = 0;
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))))
        return QStringLiteral("unavailable");
    return QString::number(cloaked);
}

QString describeWindow(HWND hwnd)
{
    if (!hwnd)
        return QStringLiteral("none");
    if (!IsWindow(hwnd))
        return QStringLiteral("destroyed");
    RECT r{};
    GetWindowRect(hwnd, &r);
    return QStringLiteral("visible=%1 iconic=%2 rect=%3,%4-%5,%6 style=0x%7 ex=0x%8 cloaked=%9")
        .arg(IsWindowVisible(hwnd) ? 1 : 0)
        .arg(IsIconic(hwnd) ? 1 : 0)
        .arg(r.left).arg(r.top).arg(r.right).arg(r.bottom)
        .arg(QString::number(static_cast<qulonglong>(GetWindowLongPtrW(hwnd, GWL_STYLE)), 16))
        .arg(QString::number(static_cast<qulonglong>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)), 16))
        .arg(cloakedState(hwnd));
}

// Read-only evidence, not an occlusion verdict: overlapping windows can be
// transparent. Bound both enumeration and output; never log window titles.
struct WindowStackProbe
{
    HWND game = nullptr;
    HWND overlay = nullptr;
    RECT gameRect{};
    int visited = 0;
    int candidates = 0;
    bool reachedGame = false;
    QStringList windows;
};

BOOL CALLBACK collectWindowsAboveGame(HWND hwnd, LPARAM context)
{
    auto& probe = *reinterpret_cast<WindowStackProbe*>(context);
    if (hwnd == probe.game) {
        probe.reachedGame = true;
        return FALSE;
    }
    if (++probe.visited > 128)
        return FALSE;
    if (hwnd == probe.overlay || !IsWindowVisible(hwnd) || IsIconic(hwnd))
        return TRUE;
    RECT rect{}, overlap{};
    if (!GetWindowRect(hwnd, &rect) || !IntersectRect(&overlap, &rect, &probe.gameRect))
        return TRUE;
    const QString cloaked = cloakedState(hwnd);
    if (cloaked != QLatin1String("0") && cloaked != QLatin1String("unavailable"))
        return TRUE;
    ++probe.candidates;
    if (probe.windows.size() < 4) {
        probe.windows.append(QStringLiteral("hwnd=%1 pid=%2 owner=%3 overlap=%4,%5-%6,%7 [%8]")
            .arg(OverlayFocus::formatHandle(hwnd))
            .arg(processIdOfWindow(hwnd))
            .arg(OverlayFocus::formatHandle(GetWindow(hwnd, GW_OWNER)))
            .arg(overlap.left).arg(overlap.top).arg(overlap.right).arg(overlap.bottom)
            .arg(describeWindow(hwnd)));
    }
    return TRUE;
}

QString describeWindowsAboveGame(HWND game, HWND overlay)
{
    WindowStackProbe probe;
    probe.game = game;
    probe.overlay = overlay;
    if (!game || !IsWindow(game) || !GetWindowRect(game, &probe.gameRect))
        return QStringLiteral("stack=unavailable");
    EnumWindows(collectWindowsAboveGame, reinterpret_cast<LPARAM>(&probe));
    return QStringLiteral("stack game-reached=%1 candidates=%2 listed=[%3]")
        .arg(probe.reachedGame ? 1 : 0).arg(probe.candidates)
        .arg(probe.windows.join(QStringLiteral("; ")));
}
}  // namespace

void OverlayManager::startShowProbe()
{
    if (!m_probeTimer) {
        m_probeTimer = new QTimer(this);
        m_probeTimer->setInterval(kProbeIntervalMs);
        connect(m_probeTimer, &QTimer::timeout, this, &OverlayManager::probeTick);
    }
    m_probeTimer->setInterval(kProbeIntervalMs);
    m_probeElapsedMs = 0;
    m_probeLastState.clear();
    m_gameContextLostTicks = 0;
    probeTick();
    m_probeTimer->start();
}

void OverlayManager::probeTick()
{
    if (!isVisible()) {
        m_probeTimer->stop();
        return;
    }

    const HWND game = static_cast<HWND>(m_previousForeground);
    const HWND fg = GetForegroundWindow();
    const HWND overlayHwnd = m_window ? reinterpret_cast<HWND>(m_window->winId()) : nullptr; 
    // Demoting an owner can also demote its popup. Repair only OUR topmost
    // position once per opening; never acquire focus or move/restyle the game.
    if (m_groupWithGame && !m_ownerTopmostRepaired && m_probeElapsedMs <= kProbeDetailMs
        && overlayHwnd && fg == overlayHwnd && game && IsWindow(game)
        && processIdOfWindow(game) == m_previousForegroundPid
        && GetWindow(overlayHwnd, GW_OWNER) == game
        && !(GetWindowLongPtrW(overlayHwnd, GWL_EXSTYLE) & WS_EX_TOPMOST)) {
        m_ownerTopmostRepaired = true;
        m_presenter->reassert();
        qInfo() << "Overlay: repaired our topmost position after owner demotion (once)";
    }
    QString state = QStringLiteral("game %1 | foreground=0x%2 | overlay %3")
        .arg(describeWindow(game))
        .arg(QString::number(reinterpret_cast<qulonglong>(fg), 16))
        .arg(overlayHwnd ? describeWindow(overlayHwnd) : QStringLiteral("none"));
    // Only the existing three-second diagnostic window enumerates z-order.
    // The steady-state context watch keeps its original lightweight queries.
    if (m_probeElapsedMs <= kProbeDetailMs)
        state += QStringLiteral(" | ") + describeWindowsAboveGame(game, overlayHwnd);
    if (state != m_probeLastState) {
        qInfo().noquote() << QStringLiteral("Overlay probe +%1ms:").arg(m_probeElapsedMs) << state;
        m_probeLastState = state;
    }
    // A foreground WinEvent can be missed during repeated window lifetimes.
    // Once we acquired foreground, reconcile an observed unrelated app through
    // the same lifetime policy: releasing input alone would leave our topmost
    // overlay visible over it. Do not act on transient null foreground, a game
    // window in the remembered process, or the denied-acquisition fallback.
    if (m_foregroundAcquired && fg && fg != overlayHwnd
        && processIdOfWindow(fg) != m_previousForegroundPid) {
        onForegroundWindowChanged(fg);
        if (!isVisible())
            return;
    }
    // cpo-o06c: the exclusive policy is granted for the interactive state only.
    // The foreground event normally reports a loss first; this is the fallback for
    // a session where it does not arrive at all, because staying in
    // exclusive-foreground while another window owns the foreground would withhold
    // input from GameHQ itself. Idempotent, so a repeated tick is harmless.
    if (m_focusPolicySink && m_focusPolicySink->exclusiveForegroundActive()
        && (!overlayHwnd || fg != overlayHwnd)) {
        qWarning() << "Overlay: the overlay no longer owns the foreground — releasing the "
                      "exclusive GameInput policy";
        m_focusPolicySink->restoreBackground(QStringLiteral("the overlay lost the foreground"));
    }
    m_probeElapsedMs += m_probeTimer->interval();
    // The dense sampling exists to catch what a game does in the first moments
    // after the overlay appears; after that the same timer keeps running, far
    // more slowly, purely as the game-context watch below.
    if (m_probeElapsedMs > kProbeDetailMs && m_probeTimer->interval() != kWatchIntervalMs)
        m_probeTimer->setInterval(kWatchIntervalMs);

    // cpo-o06b: with the overlay holding the foreground, Windows sends no
    // foreground event when the game's own window goes away — so ask directly.
    // The same rule the event path uses decides (OverlayLifetimePolicy), and
    // it has to hold for a bounded stretch before acting: a game that destroys
    // and immediately recreates its window must get the chance to rebind
    // through the normal foreground event instead of being treated as gone.
    if (!m_watchGameContext)
        return;
    OverlayLifetime::ForegroundFacts facts;
    facts.rememberedGameAlive = game && IsWindow(game);
    facts.rememberedGameVisible = facts.rememberedGameAlive && IsWindowVisible(game);
    facts.rememberedGameIconic = facts.rememberedGameAlive && IsIconic(game);
    if (!OverlayLifetime::rememberedGameContextLost(facts)) {
        m_gameContextLostTicks = 0;
        return;
    }
    if (++m_gameContextLostTicks * m_probeTimer->interval() < kContextLostGraceMs)
        return;
    qInfo().noquote() << QStringLiteral(
        "Overlay: the game window 0x%1 is gone (alive=%2 visible=%3 iconic=%4) and no "
        "foreground event could report it — hiding")
        .arg(QString::number(reinterpret_cast<qulonglong>(game), 16))
        .arg(facts.rememberedGameAlive ? 1 : 0)
        .arg(facts.rememberedGameVisible ? 1 : 0)
        .arg(facts.rememberedGameIconic ? 1 : 0);
    hideInternal();
}
