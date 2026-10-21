#include "overlay/ForegroundApi.h"

#include <windows.h>

namespace
{
// Qt's requestActivate() ends in SetForegroundWindow, which Win32 routinely
// rejects with a foreground-lock denial when our thread isn't the active
// input one (e.g. the game owns focus). AttachThreadInput briefly marries our
// input queue to the current foreground's and the target's, which makes
// SetForegroundWindow behave as if the user clicked the target — bypassing
// the lock. Symmetric: used both when stealing focus for the overlay (show)
// and when handing focus back to the game (hide).
class Win32ForegroundApi final : public ForegroundApi
{
public:
    void* foregroundWindow() override
    {
        return GetForegroundWindow();
    }

    bool isHung(void* window) override
    {
        HWND hwnd = static_cast<HWND>(window);
        return hwnd && IsWindow(hwnd) && IsHungAppWindow(hwnd);
    }

    unsigned long windowProcessId(void* window) override
    {
        HWND hwnd = static_cast<HWND>(window);
        if (!hwnd || !IsWindow(hwnd))
            return 0;
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        return pid;
    }

    bool forceForeground(void* targetV) override
    {
        HWND target = static_cast<HWND>(targetV);
        if (!target)
            return false;
        if (GetForegroundWindow() == target)
            return true;
        // A frozen target would block us inside the attached input queue or
        // the synchronous z-order change for as long as it stays frozen.
        if (IsHungAppWindow(target))
            return false;

        const DWORD myThread  = GetCurrentThreadId();
        const HWND  currentFg = GetForegroundWindow();
        const DWORD fgThread  = currentFg ? GetWindowThreadProcessId(currentFg, nullptr) : 0;
        const DWORD tgtThread = GetWindowThreadProcessId(target, nullptr);

        // Never marry our input queue to a frozen foreground thread (a hung
        // game being left for the overlay): that would make our own input
        // processing wait on it.
        const bool fgHung = currentFg && IsHungAppWindow(currentFg);
        const bool a1 = (fgThread && fgThread != myThread && !fgHung)
                        ? AttachThreadInput(myThread, fgThread, TRUE) : false;
        const bool a2 = (tgtThread != myThread && tgtThread != fgThread)
                        ? AttachThreadInput(myThread, tgtThread, TRUE) : false;

        const bool ok = SetForegroundWindow(target) != 0;
        // Z-order changes on another thread's window are sent synchronously
        // and wait for that thread; ASYNCWINDOWPOS posts them instead, so a
        // game that freezes mid-call cannot stall the GUI thread. Same-thread
        // windows keep the synchronous path (BringWindowToTop equivalent).
        const UINT async = tgtThread != myThread ? SWP_ASYNCWINDOWPOS : 0;
        SetWindowPos(target, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | async);
        SetWindowPos(target, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | async);

        if (a2) AttachThreadInput(myThread, tgtThread, FALSE);
        if (a1) AttachThreadInput(myThread, fgThread, FALSE);

        return ok;
    }
};
} // namespace

ForegroundApi* ForegroundApi::createSystem()
{
    return new Win32ForegroundApi;
}
