// cpo-o05 native acceptance target. A borderless top-level window in its own
// process, plus a never-shown control window that survives the target window
// being destroyed and recreated on command — the native equivalent of a game
// that swaps its window (borderless toggles, resolution changes) or closes it.
//
// The overlay acceptance test (tests/tst_overlaynative.cpp) drives this process
// through the registered command message and reads the target window's title,
// which carries the process id, the window generation and a message counter.
// Deliberately Win32-only: a fixture that linked Qt could not tell us whether
// the overlay's no-activation behaviour is real or an artefact of Qt's own
// activation handling in the same process.
#include <windows.h>

namespace
{
constexpr wchar_t kControlClass[] = L"GameHQOverlayNativeFixture.Control";
constexpr wchar_t kTargetClass[] = L"GameHQOverlayNativeFixture.Target";
constexpr wchar_t kCommandMessageName[] = L"GameHQ.OverlayNativeFixture.Command";

// wParam of the registered command message.
enum Command : WPARAM {
    CmdRecreate = 1,          // destroy + recreate the target window (same process), then foreground it
    CmdDestroyWindow = 2,     // destroy the target window, keep the process alive (stale-handle case)
    CmdMinimize = 3,          // minimize the target window
    CmdRestoreForeground = 4, // restore the target window and give it the foreground
    CmdPing = 5,              // target window processed a message: bump the title counter
    CmdQuit = 6,
    CmdDemoteOnDeactivate = 7,
};

HWND g_control = nullptr;
HWND g_target = nullptr;
int g_generation = 0;
unsigned g_pings = 0;
UINT g_command = 0;
bool g_demoteOnDeactivate = false;

void updateTargetTitle()
{
    if (!g_target)
        return;
    wchar_t title[128]{};
    wsprintfW(title, L"GameHQOverlayTarget#%lu#%d#p%u",
              static_cast<unsigned long>(GetCurrentProcessId()), g_generation, g_pings);
    SetWindowTextW(g_target, title);
}

// The same AttachThreadInput bypass the app itself uses for the desktop summon
// (ForegroundApi): after the old window is destroyed the OS may hand the
// foreground to an unrelated window for a moment, and the fixture must win it
// back within the process without relying on user input.
bool forceOwnForeground(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd))
        return false;
    if (GetForegroundWindow() == hwnd)
        return true;
    for (int attempt = 0; attempt < 5; ++attempt) {
        const HWND foreground = GetForegroundWindow();
        const DWORD foregroundThread = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
        const DWORD targetThread = GetWindowThreadProcessId(hwnd, nullptr);
        const DWORD thisThread = GetCurrentThreadId();
        const BOOL joinedForeground = (foregroundThread && foregroundThread != thisThread)
            ? AttachThreadInput(thisThread, foregroundThread, TRUE) : FALSE;
        const BOOL joinedTarget = (targetThread && targetThread != thisThread)
            ? AttachThreadInput(thisThread, targetThread, TRUE) : FALSE;
        keybd_event(VK_MENU, 0, 0, 0);
        keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
        SetForegroundWindow(hwnd);
        if (joinedTarget)
            AttachThreadInput(thisThread, targetThread, FALSE);
        if (joinedForeground)
            AttachThreadInput(thisThread, foregroundThread, FALSE);
        if (GetForegroundWindow() == hwnd)
            return true;
        Sleep(20);
    }
    return GetForegroundWindow() == hwnd;
}

HWND createTarget()
{
    g_demoteOnDeactivate = false;
    ++g_generation;
    g_target = CreateWindowExW(0, kTargetClass, L"", WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN,
                               60, 60, 640, 480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    updateTargetTitle();
    if (g_target)
        forceOwnForeground(g_target);
    return g_target;
}

void handleCommand(WPARAM command)
{
    switch (command) {
    case CmdRecreate:
        if (g_target) {
            HWND old = g_target;
            g_target = nullptr;
            DestroyWindow(old);
        }
        // The replacement is created and foregrounded in the same handler, so
        // the queued "no foreground window" event that the destruction causes
        // is stale by the time the overlay's hook processes it — exactly the
        // ordering a game that recreates its own window produces.
        createTarget();
        break;
    case CmdDestroyWindow:
        if (g_target) {
            HWND old = g_target;
            g_target = nullptr;
            DestroyWindow(old);
        }
        break;
    case CmdMinimize:
        if (g_target)
            ShowWindow(g_target, SW_MINIMIZE);
        break;
    case CmdRestoreForeground:
        if (g_target) {
            ShowWindow(g_target, SW_RESTORE);
            forceOwnForeground(g_target);
        }
        break;
    case CmdQuit:
        if (g_target) {
            HWND old = g_target;
            g_target = nullptr;
            DestroyWindow(old);
        }
        if (g_control) {
            HWND old = g_control;
            g_control = nullptr;
            DestroyWindow(old);
        }
        break;
    case CmdDemoteOnDeactivate:
        g_demoteOnDeactivate = true;
        if (g_target)
            SetWindowPos(g_target, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        break;
    default:
        break;
    }
}

LRESULT CALLBACK targetProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_ACTIVATE && g_demoteOnDeactivate) {
        SetWindowPos(hwnd, LOWORD(wParam) == WA_INACTIVE ? HWND_NOTOPMOST : HWND_TOPMOST,
                     0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    if (g_command && message == g_command) {
        if (wParam == CmdPing) {
            ++g_pings;
            updateTargetTitle();
            return 0;
        }
        if (wParam == CmdQuit || wParam == CmdDestroyWindow) {
            handleCommand(wParam);
            return 0;
        }
    }
    if (message == WM_CLOSE) {
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK controlProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (g_command && message == g_command) {
        handleCommand(wParam);
        return 0;
    }
    if (message == WM_CLOSE) {
        DestroyWindow(hwnd);
        return 0;
    }
    if (message == WM_DESTROY && hwnd == g_control) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool registerClasses(HINSTANCE instance)
{
    WNDCLASSEXW target{};
    target.cbSize = sizeof(target);
    target.lpfnWndProc = targetProc;
    target.hInstance = instance;
    target.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    target.lpszClassName = kTargetClass;
    if (!RegisterClassExW(&target))
        return false;

    WNDCLASSEXW control{};
    control.cbSize = sizeof(control);
    control.lpfnWndProc = controlProc;
    control.hInstance = instance;
    control.lpszClassName = kControlClass;
    return RegisterClassExW(&control) != 0;
}
}  // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int)
{
    g_command = RegisterWindowMessageW(kCommandMessageName);
    if (!g_command || !registerClasses(instance))
        return 1;

    wchar_t controlTitle[96]{};
    wsprintfW(controlTitle, L"GameHQOverlayControl#%lu",
              static_cast<unsigned long>(GetCurrentProcessId()));
    g_control = CreateWindowExW(0, kControlClass, controlTitle, WS_POPUP,
                                0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!g_control)
        return 1;

    if (!createTarget())
        return 1;

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
