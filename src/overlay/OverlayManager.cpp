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
// Only one OverlayManager exists per process; the WinEvent callback is a
// free function (Win32 API requirement) so it reaches the instance here.
OverlayManager* g_overlayManagerInstance = nullptr;

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

void OverlayManager::onForegroundAcquisitionFinished(const QString& phase, void* target,
                                                     bool acquired, int attempts)
{
    Q_UNUSED(target);
    if (phase == kShowPhase)
        finishShowTrace(acquired, attempts);
    else if (phase == kHidePhase)
        finishHideTrace(acquired);
}

