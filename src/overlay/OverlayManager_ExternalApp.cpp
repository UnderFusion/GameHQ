// OverlayManager: close the overlay and raise an external app (Share targets).
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

namespace
{
struct AppWindowSearch {
    const QStringList* exeNames = nullptr;
    HWND found = nullptr;
};

QString processImageName(DWORD pid)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return {};
    wchar_t path[MAX_PATH]{};
    DWORD size = MAX_PATH;
    const bool ok = QueryFullProcessImageNameW(process, 0, path, &size) != 0;
    CloseHandle(process);
    if (!ok)
        return {};
    const QString full = QString::fromWCharArray(path, int(size));
    return full.mid(full.lastIndexOf(QLatin1Char('\\')) + 1).toLower();
}

// Topmost (EnumWindows runs in z-order) visible, uncloaked, titled top-level
// window of one of the named processes.
BOOL CALLBACK findAppWindow(HWND hwnd, LPARAM param)
{
    auto* search = reinterpret_cast<AppWindowSearch*>(param);
    if (!IsWindowVisible(hwnd) && !IsIconic(hwnd))
        return TRUE;
    if (GetWindowTextLengthW(hwnd) == 0
        || (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW))
        return TRUE;
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))
        && cloaked && !IsIconic(hwnd))
        return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid || !search->exeNames->contains(processImageName(pid)))
        return TRUE;
    search->found = hwnd;
    return FALSE;
}

constexpr int kRaisePollMs = 150;
constexpr int kRaiseTimeoutMs = 8000;   // a cold Telegram/Discord start takes seconds
} // namespace

void OverlayManager::hideForExternalApp(const QString& targetId)
{
    QStringList names;
    if (targetId == QLatin1String("telegram-desktop"))
        names = { QStringLiteral("telegram.exe") };
    else if (targetId == QLatin1String("discord-desktop"))
        names = { QStringLiteral("discord.exe"), QStringLiteral("discordptb.exe"),
                  QStringLiteral("discordcanary.exe") };
    if (names.isEmpty()) {
        hideInternal();
        return;
    }
    hideInternal(ForegroundReturn::LeaveAlone);

    m_raiseExeNames = names;
    m_raiseElapsedMs = 0;
    if (!m_raiseTimer) {
        m_raiseTimer = new QTimer(this);
        m_raiseTimer->setInterval(kRaisePollMs);
        connect(m_raiseTimer, &QTimer::timeout, this, &OverlayManager::raiseExternalAppTick);
    }
    qInfo().noquote() << "Overlay: hidden for share hand-off, raising" << names.join(QLatin1Char('/'));
    m_raiseTimer->start();
    raiseExternalAppTick();
}

void OverlayManager::raiseExternalAppTick()
{
    m_raiseElapsedMs += kRaisePollMs;
    // Done once the app owns the foreground (it may have raised itself).
    if (HWND fg = GetForegroundWindow()) {
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        if (pid && m_raiseExeNames.contains(processImageName(pid))) {
            qInfo() << "Overlay: share app is in the foreground after" << m_raiseElapsedMs << "ms";
            m_raiseTimer->stop();
            return;
        }
    }
    if (m_raiseElapsedMs > kRaiseTimeoutMs) {
        qWarning() << "Overlay: share app window did not come to the foreground within"
                   << kRaiseTimeoutMs << "ms";
        m_raiseTimer->stop();
        return;
    }
    AppWindowSearch search;
    search.exeNames = &m_raiseExeNames;
    EnumWindows(findAppWindow, reinterpret_cast<LPARAM>(&search));
    if (!search.found)
        return;   // not started yet; poll again
    ForegroundApi& api = m_focusAcquirer->api();
    // A share app that stopped responding would block every forced attempt
    // for as long as it is frozen; give up instead of retrying for 8 s.
    if (api.isHung(search.found)) {
        qWarning() << "Overlay: share app window is not responding - stopped raising it";
        m_raiseTimer->stop();
        return;
    }
    if (IsIconic(search.found))
        ShowWindow(search.found, SW_RESTORE);
    api.forceForeground(search.found);
    // Stop as soon as the foreground actually moved rather than re-forcing
    // it on the next tick.
    if (api.foregroundWindow() == search.found) {
        qInfo() << "Overlay: share app is in the foreground after" << m_raiseElapsedMs << "ms";
        m_raiseTimer->stop();
    }
}
