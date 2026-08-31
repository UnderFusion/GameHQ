#include "input/XInputDevice.h"

#include "input/ControllerIdentity.h"
#include "input/InputDiagnostics.h"
#include "input/PerfTrace.h"
#include "input/StickNav.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QTimer>

#include <windows.h>
#include <xinput.h>

namespace {
constexpr int kPollMs = 33;
constexpr int kRescanMs = 3000;

// Reported in wButtons by XInputGetStateEx (ordinal 100) only.
constexpr WORD kGuideButton = 0x0400;

quint32 mapButtons(const XINPUT_GAMEPAD& pad)
{
    quint32 s = 0;
    auto set = [&s](int btn) { s |= (1u << btn); };

    if (pad.wButtons & XINPUT_GAMEPAD_BACK) set(Gamepad::Share);
    if (pad.wButtons & XINPUT_GAMEPAD_START) set(Gamepad::Options);
    if (pad.wButtons & XINPUT_GAMEPAD_A) set(Gamepad::Cross);
    if (pad.wButtons & XINPUT_GAMEPAD_B) set(Gamepad::Circle);
    if (pad.wButtons & XINPUT_GAMEPAD_Y) set(Gamepad::Triangle);
    if (pad.wButtons & XINPUT_GAMEPAD_X) set(Gamepad::Square);
    if (pad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) set(Gamepad::L1);
    if (pad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) set(Gamepad::R1);
    // XInput reports the triggers as 0..255 analog, with no digital edge, so
    // they are thresholded into buttons here — every action bound to a trigger
    // is a discrete step. XINPUT_GAMEPAD_TRIGGER_THRESHOLD (30) is Microsoft's
    // own "pressed" floor.
    if (pad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) set(Gamepad::L2);
    if (pad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) set(Gamepad::R2);
    if (pad.wButtons & XINPUT_GAMEPAD_DPAD_UP) set(Gamepad::DpadUp);
    if (pad.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) set(Gamepad::DpadDown);
    if (pad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) set(Gamepad::DpadLeft);
    if (pad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) set(Gamepad::DpadRight);
    if (pad.wButtons & kGuideButton) set(Gamepad::PS);
    // Previously discarded stick clicks — now exposed as generic buttons.
    if (pad.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) set(Gamepad::GenericButtonBase + 0);
    if (pad.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) set(Gamepad::GenericButtonBase + 1);

    // Signed axes centered on 0, Y growing upward. No hysteresis here (return
    // zone == deadzone), matching how this backend has always behaved.
    constexpr StickNav::AxisConfig kNav{ 0, 12000, 12000, true };
    s |= StickNav::bits(kNav, pad.sThumbLX, pad.sThumbLY);
    // Right stick vertical → wheel-like scroll controls.
    s |= StickNav::verticalBits(kNav, pad.sThumbRY,
                                Gamepad::RStickUp, Gamepad::RStickDown);

    return s;
}
}

XInputDevice::XInputDevice(QObject* parent)
    : Gamepad(parent)
    , m_pollTimer(new QTimer(this))
    , m_rescanTimer(new QTimer(this))
{
    m_pollTimer->setTimerType(Qt::PreciseTimer);
    m_pollTimer->setInterval(kPollMs);
    connect(m_pollTimer, &QTimer::timeout, this, &XInputDevice::poll);

    m_rescanTimer->setInterval(kRescanMs);
    connect(m_rescanTimer, &QTimer::timeout, this, &XInputDevice::rescan);
}

XInputDevice::~XInputDevice()
{
    if (m_library)
        FreeLibrary(static_cast<HMODULE>(m_library));
}

bool XInputDevice::start()
{
    const wchar_t* dlls[] = { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" };
    for (const wchar_t* dll : dlls) {
        HMODULE lib = LoadLibraryW(dll);
        if (!lib)
            continue;
        // Ordinal 100 = XInputGetStateEx: same signature as XInputGetState
        // but also reports the Guide button (mapped to PS). Not exported by
        // xinput9_1_0 — fall back to the named function there.
        auto ex = reinterpret_cast<XInputGetStateFn>(
            GetProcAddress(lib, reinterpret_cast<LPCSTR>(MAKEINTRESOURCEA(100))));
        auto fn = ex ? ex
                     : reinterpret_cast<XInputGetStateFn>(GetProcAddress(lib, "XInputGetState"));
        if (!fn) {
            FreeLibrary(lib);
            continue;
        }
        m_library = lib;
        m_getState = fn;
        qInfo() << "Gamepad: XInput backend ready"
                << (ex ? "(Guide button available as PS)" : "(no Guide button)");
        break;
    }

    if (!m_getState) {
        qWarning() << "Gamepad: XInput fallback unavailable";
        return true;
    }

    m_rescanTimer->start();
    rescan();
    return true;
}

void XInputDevice::rescan()
{
    if (!m_getState)
        return;

    // Probing empty slots is the documented slow path of XInputGetState
    // (device enumeration under the hood) — measure it, don't guess.
    QElapsedTimer pass;
    pass.start();
    for (int slot = 0; slot < 4; ++slot) {
        if (m_connected[slot])
            continue;
        XINPUT_STATE state{};
        if (m_getState(static_cast<DWORD>(slot), &state) == ERROR_SUCCESS)
            setSlotState(slot, mapButtons(state.Gamepad), true);
    }
    PerfTrace::reportSlow("XInput empty-slot rescan", pass.nsecsElapsed() / 1000);
}

void XInputDevice::poll()
{
    if (!m_getState)
        return;

    QElapsedTimer pass;
    pass.start();
    for (int slot = 0; slot < 4; ++slot) {
        if (!m_connected[slot])
            continue;
        XINPUT_STATE state{};
        if (m_getState(static_cast<DWORD>(slot), &state) != ERROR_SUCCESS) {
            setSlotState(slot, 0, false);
        } else {
            // Diagnostics probe: report the RAW wButtons diff — the mapped
            // mask drops bits GameHQ has no binding for, which are exactly
            // the ones an unsupported-button report needs to show.
            const quint16 raw = state.Gamepad.wButtons;
            if (raw != m_prevRawButtons[slot]
                && InputDiagnostics::instance().probeActive()) {
                InputDiagnostics::instance().noteProbeEvent(
                    QStringLiteral("XInput slot %1").arg(slot),
                    QStringLiteral("XInput"),
                    QStringLiteral("wButtons %1 -> %2")
                        .arg(m_prevRawButtons[slot], 0, 16).arg(raw, 0, 16));
            }
            m_prevRawButtons[slot] = raw;
            setSlotState(slot, mapButtons(state.Gamepad), true);
        }
    }
    PerfTrace::reportSlow("XInput connected-slot poll", pass.nsecsElapsed() / 1000);
}

void XInputDevice::setSlotState(int slot, quint32 buttons, bool connected)
{
    const bool slotChanged = (m_connected[slot] != connected);
    if (slotChanged) {
        m_connected[slot] = connected;
        qInfo() << "Gamepad: XInput slot" << slot
                << (connected ? "connected" : "disconnected");
        if (connected)
            emit slotConnectionChanged(slot, true);
    }

    // Aggregate connect is announced BEFORE the first edges so InputEngine
    // already treats this backend as available when they arrive; on
    // disconnect the release edges go out first (buttons == 0 synthesizes
    // releases for anything still held), then the aggregate drop.
    if (connected && slotChanged && !m_anyConnected) {
        m_anyConnected = true;
        emit Gamepad::connected(true);
    }

    const quint32 changed = buttons ^ m_prevButtons[slot];
    for (int b = 0; changed && b < MaxButtons; ++b) {
        const quint32 mask = 1u << b;
        if (!(changed & mask))
            continue;
        const QString control = b == Share ? ControlId::ViewBack : controlIdFor(b);
        if (buttons & mask)
            publishControlPressed(control, profileForSlot(slot));
        else
            publishControlReleased(control, profileForSlot(slot));
    }
    m_prevButtons[slot] = buttons;

    if (!connected && slotChanged)
        emit slotConnectionChanged(slot, false);

    if (!connected && slotChanged && m_anyConnected && connectedCount() == 0) {
        m_anyConnected = false;
        emit Gamepad::connected(false);
    }

    // Fast poll runs only while something is connected; empty slots are
    // handled by rescan() (device-change hints + slow safety net).
    if (m_anyConnected) {
        if (!m_pollTimer->isActive())
            m_pollTimer->start();
    } else {
        m_pollTimer->stop();
    }
}

void XInputDevice::setKnownDeviceIdentity(int slot, const QString& endpointId,
                                          const QString& modelFingerprint)
{
    if (slot < 0 || slot >= 4)
        return;
    if (m_knownEndpoint[slot] == endpointId
        && m_modelFingerprint[slot] == modelFingerprint)
        return;
    m_knownEndpoint[slot] = endpointId;
    m_modelFingerprint[slot] = modelFingerprint.toLower();
    if (!endpointId.isEmpty())
        qInfo().noquote() << "Gamepad: XInput slot" << slot
                          << "correlated to anonymized endpoint" << endpointId;
}

int XInputDevice::firstConnectedSlot() const
{
    for (int slot = 0; slot < 4; ++slot) {
        if (m_connected[slot])
            return slot;
    }
    return -1;
}

ControlId::DeviceProfile XInputDevice::profile() const
{
    const int slot = firstConnectedSlot();
    if (slot < 0)
        return {};
    // No stable identity is obtainable: say so instead of pretending. The
    // fingerprint stays slot-keyed, so overrides saved now follow the slot,
    // not the physical pad.
    return ControlId::DeviceProfile{
        QStringLiteral("XInput"),
        profileForSlot(slot).fingerprint,
        ControlId::ControllerFamily::Xbox,
        QStringLiteral("Xbox controller (per-slot profile — model cannot be identified)"),
        profileForSlot(slot).modelFingerprint,
        profileForSlot(slot).endpointId,
    };
}

ControlId::DeviceProfile XInputDevice::profileForSlot(int slot) const
{
    if (slot < 0 || slot >= 4)
        return {};
    const QString endpoint = m_knownEndpoint[slot];
    return ControlId::DeviceProfile{
        QStringLiteral("XInput"),
        ControllerIdentity::legacySlotFingerprint(slot),
        ControlId::ControllerFamily::Xbox,
        endpoint.isEmpty()
            ? QStringLiteral("Xbox controller (slot %1; hardware identity unavailable)")
                  .arg(slot + 1)
            : QStringLiteral("Xbox-compatible controller"),
        m_modelFingerprint[slot],
        endpoint,
    };
}

int XInputDevice::connectedCount() const
{
    int n = 0;
    for (bool c : m_connected)
        n += c ? 1 : 0;
    return n;
}
