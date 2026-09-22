// cpo-o06d: the three observation paths of the external receiver.
//
// Every path is a plain input consumer. No injection, no hooks, no drivers, no
// hidden devices, no game modification: the receiver may only ever observe what
// Windows already delivers to an ordinary second process, because that is the
// only kind of evidence that says anything about a real game.
#include "ReceiverProviders.h"

#include <windows.h>

#include "GameInput.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

namespace GameReceiver
{

namespace
{

namespace GI = GameInput::v3;

std::string wideToUtf8(const wchar_t* text)
{
    if (!text || !*text)
        return std::string();
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1)
        return std::string();
    std::string result(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), needed, nullptr, nullptr);
    return result;
}

std::string hexWindow(void* window)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%08llx",
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(window)));
    return std::string(buffer);
}

std::string formatDeviceId(unsigned short vendorId, unsigned short productId)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04x:%04x", vendorId, productId);
    return std::string(buffer);
}

std::string formatSeconds(double seconds)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.3fs", seconds);
    return std::string(buffer);
}

// Which slots a provider currently sees, for the presence lines.
std::string formatSlot(int index)
{
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "slot%d", index);
    return std::string(buffer);
}

}  // namespace

ForegroundInfo foregroundSnapshot()
{
    ForegroundInfo info;
    HWND window = GetForegroundWindow();
    if (!window)
        return info;
    info.window = window;
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    info.processId = processId;
    wchar_t title[256]{};
    GetWindowTextW(window, title, 256);
    info.title = wideToUtf8(title);
    return info;
}

std::vector<std::pair<std::string, std::string>> foregroundFields(const ForegroundInfo& info,
                                                                  bool selfIsForeground)
{
    std::vector<std::pair<std::string, std::string>> fields;
    fields.emplace_back("self", selfIsForeground ? "foreground" : "background");
    fields.emplace_back("fg", info.window ? hexWindow(info.window) : "none");
    fields.emplace_back("fgpid", std::to_string(info.processId));
    fields.emplace_back("fgtitle", info.title.empty() ? std::string("(untitled)") : info.title);
    return fields;
}

// ---------------------------------------------------------------------------
// GameInput. The receiver is a second GameInput client on purpose, and it asks
// the runtime for BACKGROUND ordinary input: without that explicit request a
// gap during the exclusive phase would only prove that we lost the foreground.
// ---------------------------------------------------------------------------
class GameInputProvider final : public ReceiverProvider
{
public:
    explicit GameInputProvider(ReceiverRun& run) : m_run(run) {}
    ~GameInputProvider() override { stop(); }

    const char* name() const override { return providerName(Provider::GameInput); }

    bool start(std::string& error) override
    {
        using InitializeFn = HRESULT(WINAPI*)(REFIID, void**);

        // Same runtime resolution as the app's own GameInput client: the
        // app-local redistributable first, then the machine-wide one. The
        // receiver is a separate process and must talk to the same runtime
        // GameHQ loads, otherwise the comparison is meaningless.
        wchar_t appLocal[MAX_PATH]{};
        GetModuleFileNameW(nullptr, appLocal, MAX_PATH);
        std::wstring local(appLocal);
        const size_t slash = local.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
            local = local.substr(0, slash + 1) + L"GameInputRedist.dll";
        else
            local.clear();

        m_module = local.empty()
            ? nullptr
            : LoadLibraryExW(local.c_str(), nullptr,
                             LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        m_runtime = "app-local GameInputRedist.dll";
        if (!m_module) {
            m_module = LoadLibraryExW(L"GameInputRedist.dll", nullptr,
                                      LOAD_LIBRARY_SEARCH_SYSTEM32);
            m_runtime = "system GameInputRedist.dll";
        }
        if (!m_module) {
            m_module = LoadLibraryExW(L"GameInput.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            m_runtime = "system GameInput.dll";
        }
        if (!m_module) {
            error = "GameInput runtime not loadable (Win32 " + std::to_string(GetLastError())
                + ")";
            return false;
        }

        const auto initialize =
            reinterpret_cast<InitializeFn>(GetProcAddress(m_module, "GameInputInitialize"));
        if (!initialize) {
            error = "runtime has no GameInputInitialize export";
            stop();
            return false;
        }

        const HRESULT hr = initialize(GI::IID_IGameInput, reinterpret_cast<void**>(&m_input));
        if (FAILED(hr) || !m_input) {
            error = "GameInputInitialize failed";
            stop();
            return false;
        }

        // The deliberate part: ordinary background input plus both background
        // system-button flags. This is the policy a normal background app needs
        // for the runtime to keep delivering while somebody else owns the
        // foreground, and it is what makes "the stream stopped" evidence about
        // the other process's exclusive request instead of about our own focus.
        const auto policy = GI::GameInputFocusPolicy(GI::GameInputEnableBackgroundInput
                                                    | GI::GameInputEnableBackgroundGuideButton
                                                    | GI::GameInputEnableBackgroundShareButton);
        m_input->SetFocusPolicy(policy);
        m_policyDetail = "background-input+background-guide+background-share";

        const auto kinds = GI::GameInputKind(GI::GameInputKindController
                                            | GI::GameInputKindGamepad
                                            | GI::GameInputKindArcadeStick
                                            | GI::GameInputKindFlightStick
                                            | GI::GameInputKindRacingWheel);
        m_active.store(true, std::memory_order_release);
        if (FAILED(m_input->RegisterDeviceCallback(nullptr, kinds,
                                                   GI::GameInputDeviceAnyStatus,
                                                   GI::GameInputAsyncEnumeration, this,
                                                   &GameInputProvider::deviceCallback,
                                                   &m_deviceToken))) {
            error = "RegisterDeviceCallback failed";
            stop();
            return false;
        }
        if (FAILED(m_input->RegisterReadingCallback(nullptr, kinds, this,
                                                    &GameInputProvider::readingCallback,
                                                    &m_readingToken))) {
            error = "RegisterReadingCallback failed";
            stop();
            return false;
        }
        const auto systemButtons =
            GI::GameInputSystemButtons(GI::GameInputSystemButtonGuide
                                       | GI::GameInputSystemButtonShare);
        if (FAILED(m_input->RegisterSystemButtonCallback(nullptr, systemButtons, this,
                                                         &GameInputProvider::systemButtonCallback,
                                                         &m_systemToken))) {
            // Guide/Share is measured separately by design; losing that callback
            // must not cost us the ordinary-input measurement.
            m_systemToken = 0;
            m_run.log("provider-note",
                      { { "provider", providerName(Provider::GameInput) },
                        { "note", "system-button callback unavailable" } });
        }
        return true;
    }

    void stop() override
    {
        m_active.store(false, std::memory_order_release);
        if (!m_input)
            return;
        if (m_systemToken)
            m_input->StopCallback(m_systemToken);
        if (m_readingToken)
            m_input->StopCallback(m_readingToken);
        if (m_deviceToken)
            m_input->StopCallback(m_deviceToken);
        if (m_systemToken)
            m_input->UnregisterCallback(m_systemToken);
        if (m_readingToken)
            m_input->UnregisterCallback(m_readingToken);
        if (m_deviceToken)
            m_input->UnregisterCallback(m_deviceToken);
        m_systemToken = m_readingToken = m_deviceToken = 0;
        m_input->Release();
        m_input = nullptr;
        if (m_module) {
            FreeLibrary(m_module);
            m_module = nullptr;
        }
    }

    std::string statusDetail() const override
    {
        std::string detail = "runtime=" + quote(m_runtime) + " policy=" + quote(m_policyDetail);
        if (!m_deviceId.empty())
            detail += " device=" + m_deviceId;
        return detail;
    }

    ProviderCounters takeCounters() override
    {
        ProviderCounters counters;
        counters.arrivals = m_arrivals.exchange(0);
        counters.transitions = m_transitions.exchange(0);
        counters.systemButtons = m_systemButtons.exchange(0);
        counters.presence = m_presence.exchange(0);
        return counters;
    }

private:
    static void CALLBACK deviceCallback(GI::GameInputCallbackToken token, void* context,
                                        GI::IGameInputDevice* device, uint64_t timestamp,
                                        GI::GameInputDeviceStatus currentStatus,
                                        GI::GameInputDeviceStatus previousStatus)
    {
        (void)token;
        (void)timestamp;
        (void)previousStatus;
        static_cast<GameInputProvider*>(context)->onDevice(device, currentStatus);
    }

    static void CALLBACK readingCallback(GI::GameInputCallbackToken token, void* context,
                                         GI::IGameInputReading* reading)
    {
        (void)token;
        static_cast<GameInputProvider*>(context)->onReading(reading);
    }

    static void CALLBACK systemButtonCallback(GI::GameInputCallbackToken token, void* context,
                                              GI::IGameInputDevice* device, uint64_t timestamp,
                                              GI::GameInputSystemButtons currentButtons,
                                              GI::GameInputSystemButtons previousButtons)
    {
        (void)token;
        (void)device;
        (void)timestamp;
        (void)previousButtons;
        static_cast<GameInputProvider*>(context)->onSystemButton(currentButtons);
    }

    void onDevice(GI::IGameInputDevice* device, GI::GameInputDeviceStatus status)
    {
        if (!m_active.load(std::memory_order_acquire) || !device)
            return;
        const GI::GameInputDeviceInfo* info = nullptr;
        if (FAILED(device->GetDeviceInfo(&info)) || !info)
            return;
        const std::string id = formatDeviceId(info->vendorId, info->productId);
        const bool connected = (status & GI::GameInputDeviceConnected) != 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_deviceId = id;
        }
        ++m_presence;
        const double now = m_run.nowSeconds();
        m_run.notePresence(Provider::GameInput, now);
        m_run.log("presence",
                  { { "provider", providerName(Provider::GameInput) },
                    { "device", id },
                    { "state", connected ? "attached" : "detached" },
                    { "name", info->displayName ? std::string(info->displayName) : std::string() } });
    }

    void onReading(GI::IGameInputReading* reading)
    {
        if (!m_active.load(std::memory_order_acquire) || !reading)
            return;
        const double now = m_run.nowSeconds();
        ++m_arrivals;
        m_run.noteArrival(Provider::GameInput, now);

        GI::GameInputGamepadState gamepad{};
        if (!reading->GetGamepadState(&gamepad))
            return;

        PadState state;
        state.buttons = mapButtons(gamepad.buttons);
        state.axes[AxisLeftX] = gamepad.leftThumbstickX;
        state.axes[AxisLeftY] = gamepad.leftThumbstickY;
        state.axes[AxisRightX] = gamepad.rightThumbstickX;
        state.axes[AxisRightY] = gamepad.rightThumbstickY;
        state.axes[AxisLeftTrigger] = gamepad.leftTrigger;
        state.axes[AxisRightTrigger] = gamepad.rightTrigger;

        std::string diff;
        std::string deviceId;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            diff = m_detector.update(state);
            deviceId = m_deviceId;
        }
        if (diff.empty())
            return;
        ++m_transitions;
        m_run.noteTransition(Provider::GameInput, now);
        m_run.log("transition",
                  { { "provider", providerName(Provider::GameInput) },
                    { "device", deviceId },
                    { "change", diff } });
    }

    void onSystemButton(GI::GameInputSystemButtons buttons)
    {
        if (!m_active.load(std::memory_order_acquire) || !buttons)
            return;
        const double now = m_run.nowSeconds();
        ++m_systemButtons;
        m_run.noteSystemButton(Provider::GameInput, now);
        std::string names;
        if (buttons & GI::GameInputSystemButtonGuide)
            names = "Guide";
        if (buttons & GI::GameInputSystemButtonShare)
            names += names.empty() ? "Share" : "+Share";
        m_run.log("system-button",
                  { { "provider", providerName(Provider::GameInput) }, { "buttons", names } });
    }

    static unsigned mapButtons(GI::GameInputGamepadButtons buttons)
    {
        unsigned mask = 0;
        const unsigned raw = static_cast<unsigned>(buttons);
        const struct
        {
            unsigned from;
            unsigned to;
        } mapping[] = {
            { GI::GameInputGamepadDPadUp, Button::DpadUp },
            { GI::GameInputGamepadDPadDown, Button::DpadDown },
            { GI::GameInputGamepadDPadLeft, Button::DpadLeft },
            { GI::GameInputGamepadDPadRight, Button::DpadRight },
            { GI::GameInputGamepadA, Button::A },
            { GI::GameInputGamepadB, Button::B },
            { GI::GameInputGamepadX, Button::X },
            { GI::GameInputGamepadY, Button::Y },
            { GI::GameInputGamepadLeftShoulder, Button::LeftShoulder },
            { GI::GameInputGamepadRightShoulder, Button::RightShoulder },
            { GI::GameInputGamepadLeftThumbstick, Button::LeftThumb },
            { GI::GameInputGamepadRightThumbstick, Button::RightThumb },
            { GI::GameInputGamepadMenu, Button::Menu },
            { GI::GameInputGamepadView, Button::View },
            { GI::GameInputGamepadPaddleLeft1, Button::PaddleLeft1 },
            { GI::GameInputGamepadPaddleLeft2, Button::PaddleLeft2 },
            { GI::GameInputGamepadPaddleRight1, Button::PaddleRight1 },
            { GI::GameInputGamepadPaddleRight2, Button::PaddleRight2 },
        };
        for (const auto& entry : mapping) {
            if ((raw & entry.from) != 0)
                mask |= entry.to;
        }
        return mask;
    }

    ReceiverRun& m_run;
    HMODULE m_module = nullptr;
    GI::IGameInput* m_input = nullptr;
    GI::GameInputCallbackToken m_deviceToken = 0;
    GI::GameInputCallbackToken m_readingToken = 0;
    GI::GameInputCallbackToken m_systemToken = 0;
    std::atomic_bool m_active{ false };
    std::atomic<long long> m_arrivals{ 0 };
    std::atomic<long long> m_transitions{ 0 };
    std::atomic<long long> m_systemButtons{ 0 };
    std::atomic<long long> m_presence{ 0 };
    std::mutex m_mutex;
    TransitionDetector m_detector;
    std::string m_deviceId;
    std::string m_runtime;
    std::string m_policyDetail = "default";
};

// ---------------------------------------------------------------------------
// XInput. Polled, because that is what a game does. The packet number is the
// activity signal: it increments for every report the driver delivered, so an
// idle-but-streaming pad still shows liveness here.
// ---------------------------------------------------------------------------
class XInputProvider final : public ReceiverProvider
{
public:
    explicit XInputProvider(ReceiverRun& run) : m_run(run) {}
    ~XInputProvider() override { stop(); }

    const char* name() const override { return providerName(Provider::XInput); }

    bool start(std::string& error) override
    {
        m_getStateEx = nullptr;
        m_getState = nullptr;
        m_module = LoadLibraryW(L"xinput1_4.dll");
        if (m_module)
            m_library = "xinput1_4.dll";
        if (!m_module) {
            m_module = LoadLibraryW(L"xinput1_3.dll");
            if (m_module)
                m_library = "xinput1_3.dll";
        }
        if (!m_module) {
            error = "no xinput1_4.dll or xinput1_3.dll on this machine";
            return false;
        }
        // Ordinal 100 (XInputGetStateEx) also reports the Guide button; the
        // named export does not. Guide matters here because GameHQ deliberately
        // does not request exclusive system buttons.
        m_getStateEx = reinterpret_cast<GetStateFn>(
            GetProcAddress(m_module, MAKEINTRESOURCEA(100)));
        m_getState = reinterpret_cast<GetStateFn>(GetProcAddress(m_module, "XInputGetState"));
        if (!m_getState && !m_getStateEx) {
            error = "xinput module has no state export";
            stop();
            return false;
        }
        m_running.store(true, std::memory_order_release);
        m_thread = std::thread([this] { pollLoop(); });
        return true;
    }

    void stop() override
    {
        m_running.store(false, std::memory_order_release);
        if (m_thread.joinable())
            m_thread.join();
        if (m_module) {
            FreeLibrary(m_module);
            m_module = nullptr;
        }
    }

    std::string statusDetail() const override
    {
        return "library=" + quote(m_library)
            + " state=" + quote(m_getStateEx ? "XInputGetStateEx(ordinal 100)" : "XInputGetState")
            + " slots=" + std::to_string(m_slots);
    }

    ProviderCounters takeCounters() override
    {
        ProviderCounters counters;
        counters.arrivals = m_arrivals.exchange(0);
        counters.transitions = m_transitions.exchange(0);
        counters.presence = m_presence.exchange(0);
        return counters;
    }

private:
    struct XInputGamepadState
    {
        WORD buttons;
        BYTE leftTrigger;
        BYTE rightTrigger;
        SHORT leftThumbX;
        SHORT leftThumbY;
        SHORT rightThumbX;
        SHORT rightThumbY;
    };
    struct XInputState
    {
        DWORD packetNumber;
        XInputGamepadState gamepad;
    };
    using GetStateFn = DWORD(WINAPI*)(DWORD, XInputState*);

    void pollLoop()
    {
        constexpr int kSlots = 4;
        DWORD lastPacket[kSlots] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
        bool attached[kSlots] = { false, false, false, false };
        TransitionDetector detectors[kSlots];
        while (m_running.load(std::memory_order_acquire)) {
            for (int slot = 0; slot < kSlots; ++slot) {
                XInputState state{};
                const DWORD result = m_getStateEx ? m_getStateEx(DWORD(slot), &state)
                                                  : m_getState(DWORD(slot), &state);
                const double now = m_run.nowSeconds();
                if (result != ERROR_SUCCESS) {
                    if (attached[slot]) {
                        attached[slot] = false;
                        ++m_presence;
                        m_run.notePresence(Provider::XInput, now);
                        m_run.log("presence",
                                  { { "provider", providerName(Provider::XInput) },
                                    { "device", formatSlot(slot) },
                                    { "state", "detached" } });
                    }
                    continue;
                }
                if (!attached[slot]) {
                    attached[slot] = true;
                    ++m_slots;
                    ++m_presence;
                    m_run.notePresence(Provider::XInput, now);
                    m_run.log("presence",
                              { { "provider", providerName(Provider::XInput) },
                                { "device", formatSlot(slot) },
                                { "state", "attached" } });
                }
                if (state.packetNumber == lastPacket[slot]) {
                    // Connected, but the driver delivered nothing new: that is
                    // an absence of reports, which is exactly the signal the
                    // exclusive phase is compared against the baseline with.
                    continue;
                }
                lastPacket[slot] = state.packetNumber;
                ++m_arrivals;
                m_run.noteArrival(Provider::XInput, now);

                PadState normalized;
                normalized.buttons = mapButtons(state.gamepad.buttons);
                normalized.axes[AxisLeftX] = float(state.gamepad.leftThumbX) / 32767.f;
                normalized.axes[AxisLeftY] = float(state.gamepad.leftThumbY) / 32767.f;
                normalized.axes[AxisRightX] = float(state.gamepad.rightThumbX) / 32767.f;
                normalized.axes[AxisRightY] = float(state.gamepad.rightThumbY) / 32767.f;
                normalized.axes[AxisLeftTrigger] = float(state.gamepad.leftTrigger) / 255.f;
                normalized.axes[AxisRightTrigger] = float(state.gamepad.rightTrigger) / 255.f;

                const std::string diff = detectors[slot].update(normalized);
                if (diff.empty())
                    continue;
                ++m_transitions;
                m_run.noteTransition(Provider::XInput, now);
                m_run.log("transition",
                          { { "provider", providerName(Provider::XInput) },
                            { "device", formatSlot(slot) },
                            { "change", diff } });
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    static unsigned mapButtons(WORD buttons)
    {
        unsigned mask = 0;
        if (buttons & 0x0001)
            mask |= Button::DpadUp;
        if (buttons & 0x0002)
            mask |= Button::DpadDown;
        if (buttons & 0x0004)
            mask |= Button::DpadLeft;
        if (buttons & 0x0008)
            mask |= Button::DpadRight;
        if (buttons & 0x0010)
            mask |= Button::Menu;
        if (buttons & 0x0020)
            mask |= Button::View;
        if (buttons & 0x0040)
            mask |= Button::LeftThumb;
        if (buttons & 0x0080)
            mask |= Button::RightThumb;
        if (buttons & 0x0100)
            mask |= Button::LeftShoulder;
        if (buttons & 0x0200)
            mask |= Button::RightShoulder;
        if (buttons & 0x0400)
            mask |= Button::Guide;
        if (buttons & 0x1000)
            mask |= Button::A;
        if (buttons & 0x2000)
            mask |= Button::B;
        if (buttons & 0x4000)
            mask |= Button::X;
        if (buttons & 0x8000)
            mask |= Button::Y;
        return mask;
    }

    ReceiverRun& m_run;
    HMODULE m_module = nullptr;
    GetStateFn m_getState = nullptr;
    GetStateFn m_getStateEx = nullptr;
    std::thread m_thread;
    std::atomic_bool m_running{ false };
    std::atomic<long long> m_arrivals{ 0 };
    std::atomic<long long> m_transitions{ 0 };
    std::atomic<long long> m_presence{ 0 };
    std::atomic<int> m_slots{ 0 };
    std::string m_library = "none";
};

// ---------------------------------------------------------------------------
// Raw Input, registered with RIDEV_INPUTSINK: a hidden window in the background
// still receives HID reports for the game-control usages. Its button names are
// reported as raw HID usages on purpose — a per-device button map would be a
// guess, and a guess cannot be evidence.
// ---------------------------------------------------------------------------
class RawInputProvider final : public ReceiverProvider
{
public:
    RawInputProvider(ReceiverRun& run, std::vector<RawUsage> usages)
        : m_run(run), m_usages(std::move(usages))
    {
        if (m_usages.empty())
            m_usages = { { 0x01, 0x04 }, { 0x01, 0x05 }, { 0x01, 0x08 } };
    }
    ~RawInputProvider() override { stop(); }

    const char* name() const override { return providerName(Provider::RawInput); }

    bool start(std::string& error) override
    {
        m_hid = LoadLibraryW(L"hid.dll");
        if (m_hid) {
            m_getUsages = reinterpret_cast<GetUsagesFn>(GetProcAddress(m_hid, "HidP_GetUsages"));
            m_getUsageValue =
                reinterpret_cast<GetUsageValueFn>(GetProcAddress(m_hid, "HidP_GetUsageValue"));
            if (m_getUsages && m_getUsageValue)
                m_decode = "hid";
        }

        WNDCLASSEXW windowClass{ sizeof(WNDCLASSEXW) };
        windowClass.lpfnWndProc = &RawInputProvider::windowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = L"GameHQInputReceiver.RawInput";
        if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            error = "RegisterClassEx failed";
            return false;
        }
        // Hidden, never activated: the receiver must never become the foreground
        // window, because that is the state a gap could otherwise be blamed on.
        m_window = CreateWindowExW(0, windowClass.lpszClassName, L"GameHQInputReceiver",
                                   WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, windowClass.hInstance,
                                   this);
        if (!m_window) {
            error = "CreateWindowEx failed";
            return false;
        }

        std::vector<RAWINPUTDEVICE> devices;
        devices.reserve(m_usages.size());
        for (const RawUsage& usage : m_usages) {
            RAWINPUTDEVICE device{};
            device.usUsagePage = USHORT(usage.page);
            device.usUsage = USHORT(usage.usage);
            device.dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY;
            device.hwndTarget = m_window;
            devices.push_back(device);
        }
        if (!RegisterRawInputDevices(devices.data(), UINT(devices.size()),
                                     sizeof(RAWINPUTDEVICE))) {
            error = "RegisterRawInputDevices failed (Win32 " + std::to_string(GetLastError())
                + ")";
            stop();
            return false;
        }
        m_running.store(true, std::memory_order_release);
        m_thread = std::thread([this] {
            m_threadId = GetCurrentThreadId();
            messageLoop();
        });
        return true;
    }

    void stop() override
    {
        m_running.store(false, std::memory_order_release);
        if (m_window)
            PostMessageW(m_window, WM_CLOSE, 0, 0);
        if (m_thread.joinable()) {
            // Belt and braces: if the window is already gone the WM_CLOSE never
            // arrives, and a message loop with no message would wait forever.
            if (m_threadId)
                PostThreadMessageW(m_threadId, WM_QUIT, 0, 0);
            m_thread.join();
        }
        m_window = nullptr;
        if (m_hid) {
            FreeLibrary(m_hid);
            m_hid = nullptr;
        }
    }

    std::string statusDetail() const override
    {
        std::string registered;
        for (const RawUsage& usage : m_usages) {
            if (!registered.empty())
                registered += ',';
            char buffer[24];
            std::snprintf(buffer, sizeof(buffer), "%04x:%04x", usage.page, usage.usage);
            registered += buffer;
        }
        return "decode=" + quote(m_decode) + " flags=" + quote("RIDEV_INPUTSINK|RIDEV_DEVNOTIFY")
            + " usages=" + quote(registered) + " devices=" + std::to_string(m_devices);
    }

    ProviderCounters takeCounters() override
    {
        ProviderCounters counters;
        counters.arrivals = m_arrivals.exchange(0);
        counters.transitions = m_transitions.exchange(0);
        counters.presence = m_presence.exchange(0);
        return counters;
    }

private:
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
    {
        RawInputProvider* self = reinterpret_cast<RawInputProvider*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<RawInputProvider*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self)
            return DefWindowProcW(window, message, wparam, lparam);
        if (message == WM_INPUT)
            self->onRawInput(reinterpret_cast<HRAWINPUT>(lparam));
        else if (message == WM_INPUT_DEVICE_CHANGE)
            self->onDeviceChange(wparam, reinterpret_cast<HANDLE>(lparam));
        else if (message == WM_CLOSE)
            DestroyWindow(window);
        else if (message == WM_DESTROY)
            PostQuitMessage(0);
        return DefWindowProcW(window, message, wparam, lparam);
    }

    void messageLoop()
    {
        MSG message{};
        while (m_running.load(std::memory_order_acquire)
               && GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    void onDeviceChange(WPARAM wparam, HANDLE device)
    {
        // Presence is measured separately from activity: a phase that suddenly
        // goes quiet because the pad was unplugged must not be mistaken for a
        // policy taking effect.
        ++m_presence;
        const double now = m_run.nowSeconds();
        m_run.notePresence(Provider::RawInput, now);
        const std::string path = devicePath(device);
        if (wparam == GIDC_ARRIVAL)
            ++m_devices;
        else if (wparam == GIDC_REMOVAL && m_devices > 0)
            --m_devices;
        m_run.log("presence",
                  { { "provider", providerName(Provider::RawInput) },
                    { "device", path },
                    { "state", wparam == GIDC_ARRIVAL ? "attached" : "detached" } });
    }

    void onRawInput(HRAWINPUT handle)
    {
        UINT size = 0;
        if (GetRawInputData(handle, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0
            || size == 0)
            return;
        std::vector<unsigned char> buffer(size);
        if (GetRawInputData(handle, RID_INPUT, buffer.data(), &size, sizeof(RAWINPUTHEADER))
            != size)
            return;
        const auto* raw = reinterpret_cast<const RAWINPUT*>(buffer.data());
        if (raw->header.dwType != RIM_TYPEHID)
            return;

        const double now = m_run.nowSeconds();
        ++m_arrivals;
        m_run.noteArrival(Provider::RawInput, now);

        const unsigned char* report = raw->data.hid.bRawData;
        const size_t reportSize = raw->data.hid.dwSizeHid * raw->data.hid.dwCount;
        const std::string device = devicePath(raw->header.hDevice);

        // The change signal is the report body itself: whichever byte moved,
        // moved because the device said so.
        std::vector<unsigned char>& previous = m_lastReport[raw->header.hDevice];
        if (previous.size() != reportSize) {
            previous.assign(report, report + reportSize);
            ++m_transitions;
            m_run.noteTransition(Provider::RawInput, now);
            m_run.log("transition",
                      { { "provider", providerName(Provider::RawInput) },
                        { "device", device },
                        { "size", std::to_string(reportSize) },
                        { "change", "first-report" },
                        { "decode", decodeReport(raw->header.hDevice, report, reportSize) } });
            return;
        }
        std::string changed;
        for (size_t index = 0; index < reportSize; ++index) {
            if (previous[index] == report[index])
                continue;
            if (!changed.empty())
                changed += ',';
            changed += std::to_string(index);
        }
        if (changed.empty())
            return;
        std::memcpy(previous.data(), report, reportSize);
        ++m_transitions;
        m_run.noteTransition(Provider::RawInput, now);
        m_run.log("transition",
                  { { "provider", providerName(Provider::RawInput) },
                    { "device", device },
                    { "size", std::to_string(reportSize) },
                    { "change", "bytes=" + changed },
                    { "decode", decodeReport(raw->header.hDevice, report, reportSize) } });
    }

    // Best effort, and labelled as such in the log: button usages and the hat
    // switch, straight out of Windows' own HID parser.
    std::string decodeReport(HANDLE device, const unsigned char* report, size_t reportSize)
    {
        if (!m_getUsages || !m_getUsageValue)
            return std::string();
        void* preparsed = preparsedData(device);
        if (!preparsed)
            return std::string();

        std::string decoded;
        USHORT usages[64]{};
        ULONG usageLength = 64;
        if (m_getUsages(0 /* HidP_Input */, 0x09 /* Button page */, 0, usages, &usageLength,
                        preparsed, reinterpret_cast<char*>(const_cast<unsigned char*>(report)),
                        ULONG(reportSize)) >= 0) {
            decoded = "usages=";
            for (ULONG index = 0; index < usageLength; ++index) {
                if (index)
                    decoded += ',';
                decoded += std::to_string(usages[index]);
            }
            if (usageLength == 0)
                decoded += "none";
        }
        ULONG hat = 0;
        if (m_getUsageValue(0, 0x01 /* Generic Desktop */, 0, 0x39 /* Hat switch */, &hat,
                            preparsed, reinterpret_cast<char*>(const_cast<unsigned char*>(report)),
                            ULONG(reportSize)) >= 0) {
            decoded += (decoded.empty() ? "" : " ");
            decoded += "hat=";
            decoded += std::to_string(hat);
            const unsigned dpad = hatSwitchToButtons(LONG(hat));
            decoded += " dpad=";
            decoded += dpad ? buttonNames(dpad) : std::string("centred");
        }
        return decoded;
    }

    void* preparsedData(HANDLE device)
    {
        auto found = m_preparsed.find(device);
        if (found != m_preparsed.end())
            return found->second.data();
        UINT size = 0;
        if (GetRawInputDeviceInfoW(device, RIDI_PREPARSEDDATA, nullptr, &size) != 0 || size == 0)
            return nullptr;
        std::vector<unsigned char> data(size);
        if (GetRawInputDeviceInfoW(device, RIDI_PREPARSEDDATA, data.data(), &size) != size)
            return nullptr;
        auto inserted = m_preparsed.emplace(device, std::move(data));
        return inserted.first->second.data();
    }

    static std::string devicePath(HANDLE device)
    {
        UINT size = 0;
        if (GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, nullptr, &size) != 0 || size == 0)
            return std::string();
        std::wstring path(size, L'\0');
        if (GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, path.data(), &size) < 0)
            return std::string();
        if (!path.empty() && path.back() == L'\0')
            path.pop_back();
        return wideToUtf8(path.c_str());
    }

    using GetUsagesFn = LONG(WINAPI*)(int, USHORT, USHORT, USHORT*, ULONG*, void*, char*, ULONG);
    using GetUsageValueFn =
        LONG(WINAPI*)(int, USHORT, USHORT, USHORT, ULONG*, void*, char*, ULONG);

    ReceiverRun& m_run;
    std::vector<RawUsage> m_usages;
    HMODULE m_hid = nullptr;
    GetUsagesFn m_getUsages = nullptr;
    GetUsageValueFn m_getUsageValue = nullptr;
    HWND m_window = nullptr;
    DWORD m_threadId = 0;
    std::thread m_thread;
    std::atomic_bool m_running{ false };
    std::atomic<long long> m_arrivals{ 0 };
    std::atomic<long long> m_transitions{ 0 };
    std::atomic<long long> m_presence{ 0 };
    std::atomic<int> m_devices{ 0 };
    std::map<HANDLE, std::vector<unsigned char>> m_preparsed;
    std::map<HANDLE, std::vector<unsigned char>> m_lastReport;
    std::string m_decode = "raw-bytes-only";
};

std::unique_ptr<ReceiverProvider> makeGameInputProvider(ReceiverRun& run)
{
    return std::unique_ptr<ReceiverProvider>(new GameInputProvider(run));
}

std::unique_ptr<ReceiverProvider> makeXInputProvider(ReceiverRun& run)
{
    return std::unique_ptr<ReceiverProvider>(new XInputProvider(run));
}

std::unique_ptr<ReceiverProvider> makeRawInputProvider(ReceiverRun& run,
                                                       const std::vector<RawUsage>& usages)
{
    return std::unique_ptr<ReceiverProvider>(new RawInputProvider(run, usages));
}

}  // namespace GameReceiver
