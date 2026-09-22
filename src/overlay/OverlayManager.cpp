#include "overlay/OverlayManager.h"

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

namespace {
// The two acquisition phases, spelled once: ForegroundAcquirer logs them and
// InputDiagnostics records them, so the completion handler must match exactly.
const QString kShowPhase = QStringLiteral("overlay show");
const QString kHidePhase = QStringLiteral("overlay hide");

// Post-show probe: dense sampling while the game reacts to the overlay, then
// the same timer slows down and becomes the game-context watch.
constexpr int kProbeIntervalMs = 50;
constexpr int kProbeDetailMs = 3000;
constexpr int kWatchIntervalMs = 250;
// How long the game has to look gone before the overlay acts on it.
constexpr int kContextLostGraceMs = 750;

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

// cpo-o06a: resolve one window to the plain facts OverlayFocus records. Every
// Win32 query for the trace happens here, so the trace unit itself stays pure
// and testable without a desktop session.
OverlayFocus::WindowFacts describeWindowFacts(HWND hwnd)
{
    OverlayFocus::WindowFacts facts;
    facts.handle = hwnd;
    if (!hwnd || !IsWindow(hwnd))
        return facts;   // exists stays false: a destroyed handle states nothing else
    facts.exists = true;
    facts.visible = IsWindowVisible(hwnd);
    facts.iconic = IsIconic(hwnd);
    facts.pid = processIdOfWindow(hwnd);
    RECT rect{};
    GetWindowRect(hwnd, &rect);
    facts.left = rect.left;
    facts.top = rect.top;
    facts.right = rect.right;
    facts.bottom = rect.bottom;
    return facts;
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
    : OverlayManager(engine, nullptr, parent)
{
}

OverlayManager::OverlayManager(QQmlApplicationEngine* engine, ForegroundApi* foregroundApi,
                               QObject* parent)
    : QObject(parent)
    , m_engine(engine)
    , m_lifetimeActions(std::make_unique<LifetimeActions>(*this))
    , m_focusAcquirer(std::make_unique<ForegroundAcquirer>(
          foregroundApi ? foregroundApi : ForegroundApi::createSystem(), nullptr))
    , m_releaseHandoff(std::make_unique<ModernInput::NeutralHandoffRunner>())
{
    g_overlayManagerInstance = this;
    // cpo-o06e: the close's release transition is owned by that one runner; the
    // close continues from its verdict (which may arrive synchronously when the
    // pad is already neutral).
    connect(m_releaseHandoff.get(), &ModernInput::NeutralHandoffRunner::finished, this,
            &OverlayManager::onReleaseHandoffFinished);
    // cpo-o06b: the request is bounded and asynchronous on retry, so both the
    // open and the close record are completed from here rather than stating a
    // result before Windows has produced one.
    connect(m_focusAcquirer.get(), &ForegroundAcquirer::finished, this,
            &OverlayManager::onForegroundAcquisitionFinished);
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
    m_groupWithGame = false;
    syncGameWindowOwner();
    // A close that never finished must not keep the input layer quiesced.
    m_releaseHandoff->cancel();
    if (m_focusHook)
        UnhookWinEvent(static_cast<HWINEVENTHOOK>(m_focusHook));
    if (g_overlayManagerInstance == this)
        g_overlayManagerInstance = nullptr;
}

void OverlayManager::setGameInputFocusRequestSink(ModernInput::GameInputFocusRequestSink* sink)
{
    m_focusPolicySink = sink;
    m_releaseHandoff->setPolicySink(sink);
}

void OverlayManager::setNeutralHandoffSource(ModernInput::NeutralHandoffSource* source)
{
    m_releaseHandoff->setSource(source);
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
    // Qt::Tool keeps the overlay out of the taskbar and the Alt-Tab list.
    // Qt::WindowDoesNotAcceptFocus is deliberately NOT set (cpo-o06b): it
    // would make Qt refuse keyboard focus even after Windows handed us the
    // foreground. The never-activate guarantee of the presentation path does
    // not depend on it — WS_EX_NOACTIVATE plus SWP_NOACTIVATE carry it, and
    // the presenter re-applies them on every present/reassert.
    m_window->setFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    m_presenter = std::make_unique<OverlayPresenter>(makeQWindowOverlayApi(m_window));
    // Moving between screens is one of the transitions where Qt rebuilds the
    // native window, and a rebuilt window starts without WS_EX_NOACTIVATE.
    connect(m_window, &QWindow::screenChanged, this, [this] {
        if (!isVisible() || !m_presenter)
            return;
        syncGameWindowOwner();
        const OverlayPresentReport report = m_presenter->reassert();
        qInfo().noquote() << "Overlay: re-asserted after screen change |"
                          << report.toLogString();
    });
    // Style the native window immediately: it must be unactivatable before
    // anything — including Windows itself — can decide to show it.
    m_presenter->reassert();
    return true;
}

void OverlayManager::toggleFromInput()
{
    // One physical press can reach here twice: the same PS/Guide edge arrives
    // from two input providers up to ~110 ms apart (observed in the owner's
    // log), and the second copy used to close the overlay it had just opened.
    // Deliberate open/close taps are several hundred ms apart, so a toggle
    // inside this window of the previous one is a duplicate and is dropped.
    constexpr qint64 kToggleGuardMs = 250;
    if (m_lastToggle.isValid() && m_lastToggle.elapsed() < kToggleGuardMs) {
        qInfo() << "Overlay: toggle ignored -" << m_lastToggle.elapsed()
                << "ms after the previous one (duplicate press)";
        return;
    }
    m_lastToggle.start();
    toggle();
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
    // cpo-o06e: a close is in flight (bounded, <= the handoff timeout). Re-opening
    // now would race the release that is handing the controller back, so the
    // request is refused rather than half-applied.
    if (m_closeStage == CloseStage::ReleasingInput) {
        qInfo() << "Overlay: show ignored - a close is still handing the controller back";
        return;
    }
    if (!ensureLoaded() || isVisible())
        return;

    // Remember the app that owns the screen right now (usually the game): its
    // handle for the Win32 calls that need one, and its process id as the
    // continuity evidence that survives a window recreation. This stays the
    // authoritative game context for presets, captures, gallery and session
    // even once GameHQ owns the OS foreground — foreground ownership is not
    // game identity.
    m_previousForeground = GetForegroundWindow();
    m_previousForegroundPid = processIdOfWindow(static_cast<HWND>(m_previousForeground));
    // Some borderless games drop TOPMOST on deactivation. An unrelated tool
    // window then leaves them below other apps. Group OUR popup with the live
    // game, so Windows keeps the pair together without restyling the game.
    m_groupWithGame = m_previousForegroundPid != 0
        && m_previousForegroundPid != GetCurrentProcessId()
        && (GetWindowLongPtrW(static_cast<HWND>(m_previousForeground), GWL_EXSTYLE)
            & WS_EX_TOPMOST) != 0;
    m_ownerTopmostRepaired = false;
    syncGameWindowOwner();
    m_loggedOverlayForeground = false;
    // Only a context that existed can be lost: opening the overlay with no
    // foreground window at all must not make it close itself.
    m_watchGameContext = m_previousForeground != nullptr
        && IsWindow(static_cast<HWND>(m_previousForeground));
    emit aboutToShow();

    // Cover the screen the previous app is on; fall back to primary.
    QScreen* target = targetScreenForGameWindow(m_previousForeground);
    // Every open starts from the proven non-activating presentation: the
    // window appears without disturbing the game, and only then does the
    // explicit foreground request below run. No claim is made until it settles.
    m_presenter->resetActivationPolicy();
    if (m_foregroundAcquired) {
        m_foregroundAcquired = false;
        emit foregroundAcquiredChanged();
    }
    // cpo-o06c: an open always starts from the background policy. In the normal
    // flow this is a no-op (the close path already released it), and it is
    // idempotent by contract — but it means a session that somehow still holds
    // the exclusive state cannot carry it into a new open.
    if (m_focusPolicySink)
        m_focusPolicySink->restoreBackground(QStringLiteral("overlay opening"));
    // Every geometry, visibility and z-order change goes through the single
    // presenter, so no show path can drop the never-activate guarantee.
    const OverlayPresentReport report = m_presenter->present(target->geometry());
    syncGameWindowOwner(); // geometry/screen changes can recreate our HWND
    const HWND game = static_cast<HWND>(m_previousForeground);
    const bool gameAlive = game && IsWindow(game);

    // cpo-o06a/b: one bounded record of what Windows and the controller stack
    // actually did during this open. It is completed — not written — here: the
    // foreground request may still retry, and the fields that matter most
    // (who owns the foreground, whether the game is still on screen) are only
    // true once it has settled.
    auto trace = std::make_unique<OverlayFocus::ShowTrace>();
    trace->game = describeWindowFacts(game);
    trace->overlay = describeWindowFacts(static_cast<HWND>(report.handle));
    trace->foregroundBefore = report.foregroundBefore;
    trace->foregroundAfterPresent = report.foregroundAfter;
    trace->foregroundAfterActivation = report.foregroundAfter;
    m_pendingShowTrace = std::move(trace);

    qInfo().noquote() << "Overlay: presented |" << report.toLogString()
                      << QStringLiteral("| game=0x%1 pid=%2 minimized=%3")
                             .arg(QString::number(reinterpret_cast<qulonglong>(m_previousForeground), 16))
                             .arg(m_previousForegroundPid)
                             .arg(gameAlive && IsIconic(game) ? 1 : 0);
    if (!report.foregroundPreserved())
        qWarning() << "Overlay: presentation itself changed the foreground window";
    startShowProbe();
    emit visibleChanged();

    // cpo-o06b, variant B. Order is the point: the overlay is on screen and
    // topmost FIRST, then it becomes activatable, then one bounded request
    // (1 attempt + 2 retries) asks Windows for the foreground. The game window
    // is never minimized, restored or restyled, and nothing re-forces the
    // foreground afterwards — a game that takes it back keeps it.
    if (!report.handle) {
        finishShowTrace(false, 0);
        return;
    }
    const OverlayPresentReport activation = m_presenter->makeActivatable();
    qInfo().noquote() << "Overlay: made activatable |" << activation.toLogString();
    m_pendingShowTrace->activationRequested = true;
    m_focusAcquirer->acquire(report.handle, kShowPhase);
}

// Completes the open record once the bounded foreground request has settled —
// including the cancelled case, where it settles at "not acquired". Both
// windows are re-sampled here, because that is the only moment at which
// "the overlay owns the foreground AND the game is still on screen" is a fact
// rather than an expectation.
void OverlayManager::finishShowTrace(bool acquired, int attempts)
{
    if (!m_pendingShowTrace)
        return;
    const std::unique_ptr<OverlayFocus::ShowTrace> owned = std::move(m_pendingShowTrace);
    OverlayFocus::ShowTrace& trace = *owned;

    const HWND game = static_cast<HWND>(m_previousForeground);
    const HWND overlayHwnd = m_window ? reinterpret_cast<HWND>(m_window->winId()) : nullptr;
    const HWND foreground = GetForegroundWindow();

    trace.acquisitionAttempts = attempts;
    trace.acquisitionSucceeded = acquired;
    trace.foregroundAfterActivation = foreground;
    trace.gameAfterAcquisition = describeWindowFacts(game);
    trace.overlayAfterAcquisition = describeWindowFacts(overlayHwnd);
    trace.overlayActiveQt = m_window && m_window->isActive();
    trace.overlayForegroundWin32 = overlayHwnd && foreground == overlayHwnd;
    // Observed, not intended: the overlay holds the foreground and the
    // lifetime rules have not dismissed it. A rule that had mistaken our own
    // foreground for "the user left the game" would have hidden the window
    // before this line ran.
    trace.lifetimeAcceptedOverlayForeground = isVisible() && trace.overlayOwnsForeground();

    // cpo-o06c: the policy request is made here — after the acquisition settled
    // and both windows were re-sampled — because it is gated on those facts.
    askForExclusiveGameInputPolicy(trace);

    // cpo-o06f: what this open may honestly claim about controller isolation.
    // Classified from the policy that ended up in force plus the evidence
    // recorded outside this process, so a request that was refused, or a run
    // with no external evidence behind it, cannot read as isolation. An API
    // call alone never yields full_native (gameinput/IsolationCapability.h).
    GameInputIsolation::Facts isolationFacts;
    isolationFacts.policyInForce = trace.gameInputPolicy.exclusiveApplied();
    isolationFacts.evidence = GameInputIsolation::recordedEvidence();
    trace.isolation = GameInputIsolation::classify(isolationFacts).toLogString();

    const InputDiagnostics& diagnostics = InputDiagnostics::instance();
    trace.controllerProvider = diagnostics.servingProvider();
    trace.controllerProfile = diagnostics.controllerProfileId();
    trace.gameInputFocusPolicy = diagnostics.gameInputFocusPolicy();

    const QString traceLine = trace.toLogString();
    qInfo().noquote() << "Overlay focus trace (open):" << traceLine;
    // Only the misleading direction is a warning. Qt lagging behind Windows is
    // an ordering artefact — WM_ACTIVATE has not been processed yet at this
    // instant — while Qt claiming the window is active when Windows says it is
    // not is exactly the illusion that made earlier focus attempts look like
    // they had worked.
    if (trace.overlayActiveQt && !trace.overlayForegroundWin32)
        qWarning() << "Overlay: Qt believes the overlay is active but Windows does not";
    if (trace.activationRequested && !trace.interactiveForegroundTruth()) {
        // Says which clause failed, because "foreground denied" and "the game
        // minimized itself in reaction" are different findings.
        qWarning().noquote() << "Overlay: interactive foreground NOT achieved |"
                             << traceLine;
    }

    if (m_foregroundAcquired != acquired) {
        m_foregroundAcquired = acquired;
        emit foregroundAcquiredChanged();
    }
    // cpo-x01: the export states the observed fact - whether the game window
    // actually kept the foreground through presentation. m_foregroundAcquired
    // is the acquisition result, not a presentation claim, and is never
    // exported as proof.
    InputDiagnostics::instance().noteOverlayShow(trace.foregroundPreserved(), traceLine);
}

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

// cpo-o06c: ONE request per open, and only when this leaf's own truth condition
// holds. The gate is the point: a denied foreground request, a game that
// vanished, or a game that minimized itself in reaction must never narrow
// anybody's delivery — and the record names the clause that refused it.
void OverlayManager::askForExclusiveGameInputPolicy(OverlayFocus::ShowTrace& trace)
{
    OverlayFocus::GameInputPolicyFacts& facts = trace.gameInputPolicy;
    if (!m_focusPolicySink) {
        // Overlay-only session (tests, or no input stack): no owner exists, so
        // nothing is asked for and nothing is claimed.
        facts.mode = QStringLiteral("uncontrolled");
        facts.request = QStringLiteral("refused");
        facts.reason = QStringLiteral("no GameInput focus-policy owner in this session");
        return;
    }

    const OverlayFocus::GameInputPolicyDecision decision =
        OverlayFocus::decideGameInputPolicy(trace);
    const int transitionsBefore = m_focusPolicySink->focusTransitionCount();
    if (!decision.request) {
        facts.mode = m_focusPolicySink->focusModeName();
        facts.request = QStringLiteral("refused");
        facts.reason = decision.reason;
        facts.transitions = m_focusPolicySink->focusTransitionCount() - transitionsBefore;
        return;
    }

    const bool applied = m_focusPolicySink->requestExclusiveForeground(decision.reason);
    facts.mode = m_focusPolicySink->focusModeName();
    facts.request = applied ? QStringLiteral("requested") : QStringLiteral("refused");
    facts.reason = applied
        ? decision.reason
        : decision.reason + QStringLiteral(" (no GameInput runtime attached)");
    facts.transitions = m_focusPolicySink->focusTransitionCount() - transitionsBefore;
}

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

void OverlayManager::onForegroundAcquisitionFinished(const QString& phase, void* target,
                                                     bool acquired, int attempts)
{
    Q_UNUSED(target);
    if (phase == kShowPhase)
        finishShowTrace(acquired, attempts);
    else if (phase == kHidePhase)
        finishHideTrace(acquired);
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
