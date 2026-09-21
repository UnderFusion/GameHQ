#include "overlay/OverlayManager.h"

#include "input/InputDiagnostics.h"
#include "overlay/OverlayLifetimePolicy.h"
#include "overlay/OverlayPresenter.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QScreen>
#include <QTimer>
#include <QDebug>

#include <windows.h>

// Overlay visibility never changes the game's foreground ownership.
namespace {
// Only one OverlayManager exists per process; the WinEvent callback is a
// free function (Win32 API requirement) so it reaches the instance here.
OverlayManager* g_overlayManagerInstance = nullptr;

// 0 for a missing or already-destroyed handle, so "no remembered game" and
// "remembered game is gone" both fail closed instead of matching anything.
unsigned long processIdOfWindow(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd))
        return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid;
}

// Fires for EVERY OS foreground-window change, system-wide — this is how we
// catch the Windows key (opens Start), Alt-Tab / the task switcher, and
// clicking another app, without hard-coding any specific key combo. When the
// new foreground window isn't the overlay itself and the overlay is showing,
// treat it as "something stole focus" and close the overlay.
void CALLBACK onForegroundEvent(HWINEVENTHOOK, DWORD event, HWND hwnd,
                                 LONG idObject, LONG idChild, DWORD, DWORD)
{
    if (event != EVENT_SYSTEM_FOREGROUND || idObject != OBJID_WINDOW || idChild != CHILDID_SELF)
        return;
    if (g_overlayManagerInstance)
        g_overlayManagerInstance->onForegroundWindowChanged(hwnd);
}
}  // namespace

// The four effects a lifetime decision can have, all performed through the one
// presenter that owns the never-activate guarantee — no second show path.
class OverlayManager::LifetimeActions final : public OverlayLifetime::Actions
{
public:
    explicit LifetimeActions(OverlayManager& manager)
        : m_manager(manager)
    {
    }

    void rebindGameWindow(void* newWindow) override { m_manager.rebindGameWindow(newWindow); }
    void reassertOverlay() override { m_manager.reassertOverlay(); }
    void repositionOverlay() override { m_manager.repositionOverlay(); }
    void hideOverlay() override { m_manager.hideInternal(); }

private:
    OverlayManager& m_manager;
};

OverlayManager::OverlayManager(QQmlApplicationEngine* engine, QObject* parent)
    : QObject(parent)
    , m_engine(engine)
    , m_lifetimeActions(std::make_unique<LifetimeActions>(*this))
{
    g_overlayManagerInstance = this;
    // WINEVENT_OUTOFCONTEXT: delivered via this thread's message queue, no
    // DLL injection into other processes needed — safe for the "never inject
    // into game processes" rule (docs/overlay.md).
    m_focusHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                                   nullptr, onForegroundEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
    if (!m_focusHook)
        qWarning() << "Overlay: SetWinEventHook failed — auto-hide on focus loss disabled";

}

OverlayManager::~OverlayManager()
{
    if (m_focusHook)
        UnhookWinEvent(static_cast<HWINEVENTHOOK>(m_focusHook));
    if (g_overlayManagerInstance == this)
        g_overlayManagerInstance = nullptr;
}

bool OverlayManager::isVisible() const
{
    return m_window && m_window->isVisible();
}

bool OverlayManager::ensureLoaded()
{
    if (m_window)
        return true;
    m_engine->loadFromModule("GameHQ", "OverlayWindow");
    const auto roots = m_engine->rootObjects();
    for (QObject* root : roots) {
        if (root->objectName() == QLatin1String("gamehqOverlay")) {
            m_window = qobject_cast<QQuickWindow*>(root);
            break;
        }
    }
    if (!m_window) {
        qCritical() << "Overlay: failed to load OverlayWindow.qml";
        return false;
    }
    m_window->setFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                       | Qt::Tool | Qt::WindowDoesNotAcceptFocus);
    m_presenter = std::make_unique<OverlayPresenter>(makeQWindowOverlayApi(m_window));
    // Moving between screens is one of the transitions where Qt rebuilds the
    // native window, and a rebuilt window starts without WS_EX_NOACTIVATE.
    connect(m_window, &QWindow::screenChanged, this, [this] {
        if (!isVisible() || !m_presenter)
            return;
        const OverlayPresentReport report = m_presenter->reassert();
        qInfo().noquote() << "Overlay: re-asserted after screen change |"
                          << report.toLogString();
    });
    // Style the native window immediately: it must be unactivatable before
    // anything — including Windows itself — can decide to show it.
    m_presenter->reassert();
    return true;
}

void OverlayManager::toggle()
{
    if (isVisible())
        hide();
    else
        show();
}

void OverlayManager::show()
{
    if (!ensureLoaded() || isVisible())
        return;

    // Remember the app that owns the screen right now (usually the game): its
    // handle for the Win32 calls that need one, and its process id as the
    // continuity evidence that survives a window recreation.
    m_previousForeground = GetForegroundWindow();
    m_previousForegroundPid = processIdOfWindow(static_cast<HWND>(m_previousForeground));
    emit aboutToShow();

    // Cover the screen the previous app is on; fall back to primary.
    QScreen* target = targetScreenForGameWindow(m_previousForeground);
    // Non-activating overlay: the game keeps the foreground. Controller
    // navigation is routed by overlay visibility, not by OS focus. Do not
    // use raise/requestActivate or the force-foreground retry path.
    if (m_foregroundAcquired) {
        m_foregroundAcquired = false;
        emit foregroundAcquiredChanged();
    }
    // Every geometry, visibility and z-order change goes through the single
    // presenter, so no show path can drop the never-activate guarantee.
    const OverlayPresentReport report = m_presenter->present(target->geometry());
    const HWND game = static_cast<HWND>(m_previousForeground);
    const bool gameAlive = game && IsWindow(game);
    qInfo().noquote() << "Overlay: shown without activation |" << report.toLogString()
                      << QStringLiteral("| game=0x%1 pid=%2 minimized=%3")
                             .arg(QString::number(reinterpret_cast<qulonglong>(m_previousForeground), 16))
                             .arg(m_previousForegroundPid)
                             .arg(gameAlive && IsIconic(game) ? 1 : 0);
    if (!report.foregroundPreserved())
        qWarning() << "Overlay: showing the overlay changed the foreground window";
    startShowProbe();
    emit visibleChanged();
    // cpo-x01: the export states the observed fact - whether the game window
    // actually kept the foreground through presentation. m_foregroundAcquired
    // is the non-activating policy contract, not evidence, and is never
    // exported as proof.
    InputDiagnostics::instance().noteOverlayShow(report.foregroundPreserved());
}

// --- post-show diagnostic probe -------------------------------------------
// Every 2026-09-12 failure had the same shape: overlay shown with the game
// still foreground, then ~0.5 s later the game window was gone (foreground
// NULL or handed to whatever was next in z-order) although nothing in GameHQ
// touches the game window. This samples the game window for 3 s after show()
// and logs each state change with its timestamp, so the next repro tells us
// whether the game hides, minimizes, resizes or destroys its window.
namespace {
QString describeWindow(HWND hwnd)
{
    if (!hwnd)
        return QStringLiteral("none");
    if (!IsWindow(hwnd))
        return QStringLiteral("destroyed");
    RECT r{};
    GetWindowRect(hwnd, &r);
    return QStringLiteral("visible=%1 iconic=%2 rect=%3,%4-%5,%6 style=0x%7 ex=0x%8")
        .arg(IsWindowVisible(hwnd) ? 1 : 0)
        .arg(IsIconic(hwnd) ? 1 : 0)
        .arg(r.left).arg(r.top).arg(r.right).arg(r.bottom)
        .arg(QString::number(static_cast<qulonglong>(GetWindowLongPtrW(hwnd, GWL_STYLE)), 16))
        .arg(QString::number(static_cast<qulonglong>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)), 16));
}
}  // namespace

void OverlayManager::startShowProbe()
{
    if (!m_probeTimer) {
        m_probeTimer = new QTimer(this);
        m_probeTimer->setInterval(50);
        connect(m_probeTimer, &QTimer::timeout, this, &OverlayManager::probeTick);
    }
    m_probeElapsedMs = 0;
    m_probeLastState.clear();
    probeTick();
    m_probeTimer->start();
}

void OverlayManager::probeTick()
{
    const HWND game = static_cast<HWND>(m_previousForeground);
    const HWND fg = GetForegroundWindow();
    const QString state = QStringLiteral("game %1 | foreground=0x%2 | overlay %3")
        .arg(describeWindow(game))
        .arg(QString::number(reinterpret_cast<qulonglong>(fg), 16))
        .arg(m_window ? describeWindow(reinterpret_cast<HWND>(m_window->winId()))
                      : QStringLiteral("none"));
    if (state != m_probeLastState) {
        qInfo().noquote() << QStringLiteral("Overlay probe +%1ms:").arg(m_probeElapsedMs) << state;
        m_probeLastState = state;
    }
    m_probeElapsedMs += m_probeTimer->interval();
    if (m_probeElapsedMs > 3000 || !isVisible())
        m_probeTimer->stop();
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
    hideInternal();
    return previous;
}

void OverlayManager::hideInternal()
{
    if (!isVisible())
        return;
    if (m_probeTimer)
        m_probeTimer->stop();
    // The overlay never takes focus, so closing it has nothing to restore.
    // In particular, do not attach input queues or retry foreground changes.
    m_window->hide();
    m_previousForeground = nullptr;
    m_previousForegroundPid = 0;
    qInfo() << "Overlay: hidden without changing foreground";
    // A closed overlay makes no isolation claim; clear any stale warning.
    if (!m_foregroundAcquired) {
        m_foregroundAcquired = true;
        emit foregroundAcquiredChanged();
    }
    emit visibleChanged();
    // cpo-x01: closing the overlay is a real transition the export shows; it
    // makes no foreground claim, so no preservation value is recorded.
    InputDiagnostics::instance().noteOverlayHide();
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
    qInfo().noquote() << QStringLiteral(
        "Overlay: game window replaced by 0x%1 (pid %2) — rebinding, overlay stays open")
        .arg(QString::number(reinterpret_cast<qulonglong>(newWindow), 16))
        .arg(m_previousForegroundPid);
}

void OverlayManager::reassertOverlay()
{
    if (!m_presenter)
        return;
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
