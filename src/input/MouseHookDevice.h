#pragma once
#include <QObject>
#include <QSet>
#include <QString>

#include <windows.h>

#include <future>
#include <mutex>
#include <thread>

// Global source for extra (non-primary) mouse buttons only. Installs a
// low-level Windows mouse hook (WH_MOUSE_LL)
// that OBSERVES XButton1/XButton2/middle-click and unconditionally passes
// every event through (CallNextHookEx) — it never swallows or injects
// input. Left/right clicks are never read here at all.
//
// The hook lives on a DEDICATED worker thread with its own message loop.
// Windows delivers every WH_MOUSE_LL callback synchronously to the thread
// that installed the hook, and the system pointer waits on that delivery —
// so a hook owned by the GUI thread turns any busy GUI frame into a
// system-wide mouse stall (GitHub stutter report). The worker thread does
// nothing but sleep in GetMessage(), so callbacks run promptly no matter
// what the rest of GameHQ is doing. Normal thread priority is enough for a
// thread that is asleep whenever the hook is idle.
//
// start()/stop() are idempotent. The caller (InputEngine) keeps the hook
// installed only while a mouse binding or a mouse capture can actually
// consume events — users without mouse bindings never get a hook at all.
class MouseHookDevice : public QObject
{
    Q_OBJECT
public:
    explicit MouseHookDevice(QObject* parent = nullptr);
    ~MouseHookDevice() override;

    // Canonical codes for the buttons this device can report.
    static const QString ButtonBack;      // "mouse.button4" (XBUTTON1 / Back)
    static const QString ButtonForward;   // "mouse.button5" (XBUTTON2 / Forward)
    static const QString ButtonMiddle;    // "mouse.middle"

    // Installs the hook on the worker thread. Returns true when the hook is
    // (already) active; false only on a hard setup failure (another instance
    // active in this process, or SetWindowsHookEx refusing the install).
    bool start();

    // Uninstalls the hook and joins the worker thread, then emits
    // buttonReleased for every button still logically held — a binding
    // removed mid-hold must not leave a hold gesture armed forever. Safe to
    // call repeatedly and before the first start().
    void stop();

    bool isRunning() const { return m_running; }

    // Test seam: feeds one hook event through the real parsing/tracking path
    // (tst_mousehooklazy). `mouseData` is the MSLLHOOKSTRUCT field, so
    // XBUTTON1/XBUTTON2 go in the high word.
    void simulateEventForTest(WPARAM message, DWORD mouseData);

signals:
    // Emitted from the worker thread; cross-thread connections deliver them
    // queued on the receiver's (GUI) thread.
    void buttonPressed(const QString& code);
    void buttonReleased(const QString& code);

private:
    static LRESULT CALLBACK lowLevelProc(int nCode, WPARAM wParam, LPARAM lParam);
    void hookThreadMain(std::promise<bool>& installed);
    void handleEvent(WPARAM wParam, LPARAM lParam);
    void trackAndEmit(const QString& code, bool pressed);

    std::thread m_thread;
    DWORD m_threadId = 0;      // worker thread id, target for the WM_QUIT stop signal
    bool m_running = false;    // owned by the caller's (GUI) thread

    // Buttons currently down, tracked so stop() can release them logically.
    // Written on the worker thread, drained on the GUI thread in stop().
    std::mutex m_pressedMutex;
    QSet<QString> m_pressed;

    // WH_MOUSE_LL requires a plain function pointer, so the single active
    // instance is reached through this — SetWindowsHookEx allows only one
    // hook chain per thread anyway, so one process-wide instance is the
    // natural limit, not an artificial one.
    static MouseHookDevice* s_instance;
};
