#include "overlay/OverlayManager.h"

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
    : QObject(parent)
    , m_engine(engine)
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
    applyNoActivateStyle();
    return true;
}

// Qt's Windows backend does NOT translate Qt::WindowDoesNotAcceptFocus into
// WS_EX_NOACTIVATE (verified in Qt 6.8 qwindowswindow.cpp: it only shows the
// window with SW_SHOWNOACTIVATE and rejects WM_MOUSEACTIVATE). Without the
// ex-style, Windows is free to activate the overlay on its own — notably it
// picks the next topmost window when the current foreground window hides or
// minimizes — which is exactly what stole the foreground from the game in
// the 2026-09-12 logs. Applied natively; harmless if already present.
void OverlayManager::applyNoActivateStyle()
{
    const HWND hwnd = reinterpret_cast<HWND>(m_window->winId());
    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_NOACTIVATE)
        return;
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle | WS_EX_NOACTIVATE);
    // Frame-cache the style change without moving, resizing or activating.
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE
                 | SWP_FRAMECHANGED);
    qInfo() << "Overlay: WS_EX_NOACTIVATE applied to" << hwnd
            << "| exstyle now" << Qt::hex
            << GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
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

    // Remember the app that owns the screen right now (usually the game).
    m_previousForeground = GetForegroundWindow();
    emit aboutToShow();

    // Cover the screen the previous app is on; fall back to primary.
    QScreen* target = QGuiApplication::primaryScreen();
    if (m_previousForeground) {
        const HMONITOR monitor = MonitorFromWindow(
            static_cast<HWND>(m_previousForeground), MONITOR_DEFAULTTOPRIMARY);
        const auto screens = QGuiApplication::screens();
        for (QScreen* screen : screens) {
            if (MonitorFromPoint(POINT{ screen->geometry().center().x(),
                                        screen->geometry().center().y() },
                                 MONITOR_DEFAULTTONULL) == monitor) {
                target = screen;
                break;
            }
        }
    }
    m_window->setGeometry(target->geometry());
    // Experimental non-activating overlay: keep the game in the foreground.
    // Controller navigation is routed by overlay visibility, not OS focus.
    // Do not use raise/requestActivate or the force-foreground retry path.
    if (m_foregroundAcquired) {
        m_foregroundAcquired = false;
        emit foregroundAcquiredChanged();
    }
    // Qt may rebuild the native styles on flag/geometry changes; make sure
    // the no-activate ex-style is in place BEFORE the window becomes visible.
    applyNoActivateStyle();
    m_window->show();
    const HWND overlayHwnd = reinterpret_cast<HWND>(m_window->winId());
    SetWindowPos(overlayHwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    qInfo() << "Overlay: shown without activation over" << m_previousForeground
            << "| overlay hwnd=" << overlayHwnd
            << "| exstyle=" << Qt::hex << GetWindowLongPtrW(overlayHwnd, GWL_EXSTYLE) << Qt::dec
            << "| foreground=" << GetForegroundWindow()
            << "| game minimized=" << (m_previousForeground
                && IsIconic(static_cast<HWND>(m_previousForeground)));
    startShowProbe();
    emit visibleChanged();
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
    void* previous = isVisible() ? m_previousForeground : nullptr;
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
    qInfo() << "Overlay: hidden without changing foreground";
    // A closed overlay makes no isolation claim; clear any stale warning.
    if (!m_foregroundAcquired) {
        m_foregroundAcquired = true;
        emit foregroundAcquiredChanged();
    }
    emit visibleChanged();
}

void OverlayManager::onForegroundWindowChanged(void* newForeground)
{
    if (!isVisible())
        return;
    // Out-of-context WinEvents are queued: an earlier away event can arrive
    // after show() has already acquired focus. Only act on the current owner.
    const HWND currentForeground = GetForegroundWindow();
    if (!currentForeground || currentForeground != static_cast<HWND>(newForeground))
        return;
    const HWND overlayHwnd = reinterpret_cast<HWND>(m_window->winId());
    if (static_cast<HWND>(newForeground) == overlayHwnd)
        return;  // the overlay grabbing its own foreground during show() — expected, not a focus loss
    if (newForeground == m_previousForeground)
        return;  // non-activating overlay intentionally leaves the game focused

    qInfo() << "Overlay: foreground moved away to" << newForeground
            << "(Windows key / Alt-Tab / task switch / other app) — auto-hiding";
    hideInternal();
}
