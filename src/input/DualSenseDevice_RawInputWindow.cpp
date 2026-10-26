// DualSenseDevice: the message-only Raw Input window (window procedure,
// Raw Input registration and startup). Split out of DualSenseDevice.cpp; the
// class itself is declared in DualSenseDevice.h.
#include "input/DualSenseDevice.h"
#include "input/DualSenseDeviceInternal.h"

#include <QDebug>

#include <windows.h>
#include <dbt.h>

using namespace DualSenseInternal;

namespace
{
const wchar_t* kWndClassName       = L"GameHQRawInputWindow";

DualSenseDevice* deviceFor(HWND hwnd)
{
    return reinterpret_cast<DualSenseDevice*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

LRESULT CALLBACK rawInputWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_INPUT:
        if (auto* dev = deviceFor(hwnd)) {
            dev->noteWmInput(GET_RAWINPUT_CODE_WPARAM(wParam) == RIM_INPUTSINK);
            dev->onRawInput(reinterpret_cast<void*>(lParam));
        }
        // Documented WM_INPUT contract: a foreground event (RIM_INPUT) must be
        // passed to DefWindowProc so the system can perform its cleanup pass,
        // while a sink event (RIM_INPUTSINK) returns 0. Returning 0 for both —
        // as this did — leaks the OS-side buffer for every foreground event.
        if (GET_RAWINPUT_CODE_WPARAM(wParam) == RIM_INPUT)
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        return 0;
    case WM_DEVICECHANGE:
        if ((wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE) && lParam
            && reinterpret_cast<const DEV_BROADCAST_HDR*>(lParam)->dbch_devicetype
                   == DBT_DEVTYP_DEVICEINTERFACE) {
            if (auto* dev = deviceFor(hwnd))
                dev->onPnpInterfaceChange();
        }
        return TRUE;
    case WM_INPUT_DEVICE_CHANGE:
        if (auto* dev = deviceFor(hwnd))
            dev->onDeviceChange(wParam == GIDC_ARRIVAL, reinterpret_cast<void*>(lParam));
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}
} // namespace

bool DualSenseDevice::registerRawInput(bool remove)
{
    RAWINPUTDEVICE rid[3]{};
    rid[0].usUsagePage = kUsagePageGeneric;
    rid[0].usUsage     = kUsageGamepad;
    rid[0].dwFlags     = remove ? RIDEV_REMOVE : (RIDEV_INPUTSINK | RIDEV_DEVNOTIFY);
    rid[0].hwndTarget  = remove ? nullptr : static_cast<HWND>(m_hwnd);
    rid[1] = rid[0];
    rid[1].usUsage     = kUsageJoystick;
    rid[2] = rid[0];
    rid[2].usUsage     = kUsageMultiAxis;

    return RegisterRawInputDevices(rid, 3, sizeof(rid[0]));
}

bool DualSenseDevice::start()
{
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = rawInputWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = kWndClassName;
    RegisterClassExW(&wc);   // ERROR_CLASS_ALREADY_EXISTS is harmless

    HWND hwnd = CreateWindowExW(0, kWndClassName, L"GameHQ Raw Input", 0,
                                0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                wc.hInstance, nullptr);
    if (!hwnd) {
        qWarning() << "Gamepad: message-only window creation failed";
        return false;
    }
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    m_hwnd = hwnd;

    // Register for Joystick/Gamepad/MultiAxis usages — pads enumerate as any.
    if (!registerRawInput()) {
        qWarning() << "Gamepad: RegisterRawInputDevices failed, error" << GetLastError();
        return false;
    }
    qInfo() << "Gamepad: Raw Input registered (Sony HID optional - none required to run)";
    if (m_hidReaderFactory) {
        DEV_BROADCAST_DEVICEINTERFACE_W filter{};
        filter.dbcc_size = sizeof(filter);
        filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
        filter.dbcc_classguid =   // GUID_DEVINTERFACE_HID
            { 0x4D1E55B2, 0xF16F, 0x11CF, { 0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 } };
        m_devNotify = RegisterDeviceNotificationW(hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
    }
    logDeliveryReceipt("registered");
    // RIDEV_DEVNOTIFY also delivers arrival messages for already-connected
    // devices, but do one synchronous scan so startup logs the initial set.
    reconcileDevices();
    return true;
}
