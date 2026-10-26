#pragma once

// Injectable seam over the Win32 foreground surface, so the overlay's
// acquisition/retry logic is testable: real foreground behavior cannot be
// forced deterministically on a live desktop (the shell arbitrates focus),
// but a fake can refuse exactly N times.
class ForegroundApi
{
public:
    virtual ~ForegroundApi() = default;

    virtual void* foregroundWindow() = 0;
    // One attempt to move the OS foreground to `target`, using the
    // AttachThreadInput foreground-lock bypass. Returns what
    // SetForegroundWindow reported; the caller re-reads foregroundWindow()
    // for the truth — Windows may report success and still not move focus.
    virtual bool forceForeground(void* target) = 0;

    // True when `window` belongs to a thread that has stopped pumping
    // messages (IsHungAppWindow). A frozen game must not be touched: focus
    // calls against it can block the GUI thread for as long as it is frozen.
    virtual bool isHung(void* window) { (void)window; return false; }

    // Owning process id of `window`, 0 when it no longer exists. Lets a
    // remembered HWND be checked against recycling before it is reused.
    virtual unsigned long windowProcessId(void* window) { (void)window; return 0; }

    // Production implementation (Win32). Caller owns the result.
    static ForegroundApi* createSystem();
};

// A remembered window is still the one that was remembered only while it
// exists and belongs to the same process: HWNDs are recycled, so a stale
// handle could hand focus to an unrelated window.
inline bool isSameWindow(ForegroundApi& api, void* window, unsigned long rememberedPid)
{
    return window && rememberedPid != 0 && api.windowProcessId(window) == rememberedPid;
}
