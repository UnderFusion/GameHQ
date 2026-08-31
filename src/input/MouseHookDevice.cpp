#include "input/MouseHookDevice.h"

#include <QDebug>

const QString MouseHookDevice::ButtonBack    = QStringLiteral("mouse.button4");
const QString MouseHookDevice::ButtonForward = QStringLiteral("mouse.button5");
const QString MouseHookDevice::ButtonMiddle  = QStringLiteral("mouse.middle");

MouseHookDevice* MouseHookDevice::s_instance = nullptr;

MouseHookDevice::MouseHookDevice(QObject* parent)
    : QObject(parent)
{
}

MouseHookDevice::~MouseHookDevice()
{
    stop();
}

bool MouseHookDevice::start()
{
    if (m_running)
        return true;
    if (s_instance && s_instance != this) {
        qWarning() << "MouseHook: another instance is already active in this process";
        return false;
    }
    s_instance = this;

    // The install verdict comes from the worker thread: SetWindowsHookEx must
    // run on the thread whose message loop will service the hook.
    std::promise<bool> installed;
    auto verdict = installed.get_future();
    m_thread = std::thread([this, &installed] { hookThreadMain(installed); });
    if (!verdict.get()) {
        m_thread.join();
        m_thread = std::thread();
        s_instance = nullptr;
        return false;
    }

    m_running = true;
    qInfo() << "MouseHook: extra mouse buttons (Back/Forward/Middle) observed"
            << "globally on a dedicated hook thread";
    return true;
}

void MouseHookDevice::stop()
{
    if (!m_running)
        return;
    PostThreadMessageW(m_threadId, WM_QUIT, 0, 0);
    m_thread.join();
    m_thread = std::thread();
    m_threadId = 0;
    m_running = false;
    if (s_instance == this)
        s_instance = nullptr;
    qInfo() << "MouseHook: hook removed (no mouse binding or capture needs it)";

    // Release anything still logically held. Emitted from the caller's
    // thread, so InputEngine's handlers run synchronously here — a press
    // queued from the worker thread but not yet delivered is dropped by the
    // isRunning() guard on the receiving side, so no press can outlive its
    // release.
    QSet<QString> held;
    {
        std::lock_guard<std::mutex> lock(m_pressedMutex);
        held.swap(m_pressed);
    }
    for (const QString& code : held)
        emit buttonReleased(code);
}

void MouseHookDevice::hookThreadMain(std::promise<bool>& installed)
{
    // Touch the (lazily created) message queue before reporting readiness, so
    // stop()'s PostThreadMessage cannot race a queue that does not exist yet.
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    HHOOK hook = SetWindowsHookExW(WH_MOUSE_LL, &MouseHookDevice::lowLevelProc, nullptr, 0);
    if (!hook)
        qWarning() << "MouseHook: SetWindowsHookEx failed, error" << GetLastError();
    m_threadId = GetCurrentThreadId();
    installed.set_value(hook != nullptr);   // `installed` lives in start()'s frame,
    if (!hook)                              // which blocks on the future until here
        return;

    // The hook callback fires from inside GetMessage — this loop exists so
    // the thread is always ready to service it. It sleeps otherwise.
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    UnhookWindowsHookEx(hook);
}

LRESULT CALLBACK MouseHookDevice::lowLevelProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && s_instance)
        s_instance->handleEvent(wParam, lParam);
    // Always pass through unmodified — this hook only observes. Left/right
    // clicks are never inspected above, let alone blocked.
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

void MouseHookDevice::handleEvent(WPARAM wParam, LPARAM lParam)
{
    switch (wParam) {
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP: {
        const auto* info = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        const WORD xButton = HIWORD(info->mouseData);
        const QString& code = (xButton == XBUTTON1) ? ButtonBack : ButtonForward;
        trackAndEmit(code, wParam == WM_XBUTTONDOWN);
        break;
    }
    case WM_MBUTTONDOWN:
        trackAndEmit(ButtonMiddle, true);
        break;
    case WM_MBUTTONUP:
        trackAndEmit(ButtonMiddle, false);
        break;
    default:
        break;   // WM_MOUSEMOVE and everything else — cheap no-op, never inspected further
    }
}

void MouseHookDevice::trackAndEmit(const QString& code, bool pressed)
{
    {
        std::lock_guard<std::mutex> lock(m_pressedMutex);
        if (pressed)
            m_pressed.insert(code);
        else
            m_pressed.remove(code);
    }
    if (pressed)
        emit buttonPressed(code);
    else
        emit buttonReleased(code);
}

void MouseHookDevice::simulateEventForTest(WPARAM message, DWORD mouseData)
{
    MSLLHOOKSTRUCT info{};
    info.mouseData = mouseData;
    handleEvent(message, reinterpret_cast<LPARAM>(&info));
}
