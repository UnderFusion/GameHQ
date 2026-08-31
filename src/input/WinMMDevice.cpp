#include "input/WinMMDevice.h"

#include "input/PerfTrace.h"
#include "input/StickNav.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QTimer>

#include <windows.h>
#include <mmsystem.h>

namespace {
constexpr int kPollMs = 50;
constexpr int kRescanMs = 2000;
constexpr UINT kMaxSlots = 16;

quint32 mapWinMMState(const JOYINFOEX& info, bool ds4Layout)
{
    quint32 s = 0;
    auto set = [&s](int btn) { s |= (1u << btn); };
    const quint32 b = info.dwButtons;

    if (ds4Layout) {
        // Sony DS4/DualSense DirectInput button order (also used by DSX's
        // virtual Sony pads):
        //   0=Square 1=Cross 2=Circle 3=Triangle 4=L1 5=R1 6=L2 7=R2
        //   8=Share 9=Options 10=L3 11=R3 12=PS 13=Touchpad
        // This is what makes Share-screenshot and PS-overlay work when the
        // pad is only reachable through WinMM.
        if (b & 0x0001) set(Gamepad::Square);
        if (b & 0x0002) set(Gamepad::Cross);
        if (b & 0x0004) set(Gamepad::Circle);
        if (b & 0x0008) set(Gamepad::Triangle);
        if (b & 0x0010) set(Gamepad::L1);
        if (b & 0x0020) set(Gamepad::R1);
        if (b & 0x0100) set(Gamepad::Share);
        if (b & 0x0200) set(Gamepad::Options);
        if (b & 0x1000) set(Gamepad::PS);
        // Previously discarded — now exposed as generic (unnamed-position)
        // buttons so they stay bindable: 6=L2 7=R2 10=L3 11=R3 13=Touchpad.
        if (b & 0x0040) set(Gamepad::GenericButtonBase + 0);
        if (b & 0x0080) set(Gamepad::GenericButtonBase + 1);
        if (b & 0x0400) set(Gamepad::GenericButtonBase + 2);
        if (b & 0x0800) set(Gamepad::GenericButtonBase + 3);
        if (b & 0x2000) set(Gamepad::GenericButtonBase + 4);
    } else {
        // Xbox-style joystick button numbering:
        //   0=A, 1=B, 2=X, 3=Y, 4=LB, 5=RB, 6=Back, 7=Start, 8=LThumb, 9=RThumb
        // Covers DSX virtual Xbox mode and generic DirectInput pads.
        if (b & 0x01) set(Gamepad::Cross);       // A
        if (b & 0x02) set(Gamepad::Circle);      // B
        if (b & 0x04) set(Gamepad::Square);      // X
        if (b & 0x08) set(Gamepad::Triangle);    // Y
        if (b & 0x10) set(Gamepad::L1);          // LB
        if (b & 0x20) set(Gamepad::R1);          // RB
        if (b & 0x40) set(Gamepad::Share);       // Back
        if (b & 0x80) set(Gamepad::Options);     // Start
        // Previously discarded — now exposed as generic buttons.
        if (b & 0x100) set(Gamepad::GenericButtonBase + 0);   // LThumb
        if (b & 0x200) set(Gamepad::GenericButtonBase + 1);   // RThumb
    }

    // Some drivers report "centered" as 0xFFFFFFFF instead of the 16-bit
    // JOY_POVCENTERED (0xFFFF) — treat anything outside 0..35999 as neutral.
    if (info.dwPOV < 36000) {
        if (info.dwPOV >= 31500 || info.dwPOV <= 4500) set(Gamepad::DpadUp);
        if (info.dwPOV >= 4500 && info.dwPOV <= 13500) set(Gamepad::DpadRight);
        if (info.dwPOV >= 13500 && info.dwPOV <= 22500) set(Gamepad::DpadDown);
        if (info.dwPOV >= 22500 && info.dwPOV <= 31500) set(Gamepad::DpadLeft);
    }

    // Unsigned 0..65535 axes centered mid-range, Y growing downward. No
    // hysteresis here (return zone == deadzone), matching how this backend has
    // always behaved. The right stick is deliberately NOT decoded here: which
    // of the Z/R/U/V axes it lands on is not standardized across generic pads,
    // and a wrong guess would turn a trigger into phantom scrolling.
    constexpr StickNav::AxisConfig kNav{ 32767, 16000, 16000, false };
    s |= StickNav::bits(kNav, static_cast<int>(info.dwXpos), static_cast<int>(info.dwYpos));

    return s;
}
} // namespace

WinMMDevice::WinMMDevice(QObject* parent)
    : Gamepad(parent)
    , m_pollTimer(new QTimer(this))
    , m_rescanTimer(new QTimer(this))
{
    m_pollTimer->setTimerType(Qt::PreciseTimer);
    m_pollTimer->setInterval(kPollMs);
    connect(m_pollTimer, &QTimer::timeout, this, &WinMMDevice::poll);

    m_rescanTimer->setInterval(kRescanMs);
    connect(m_rescanTimer, &QTimer::timeout, this, &WinMMDevice::rescan);
}

WinMMDevice::~WinMMDevice()
{
    if (m_scanThread.joinable())
        m_scanThread.join();
}

bool WinMMDevice::start()
{
    // Discovery is asynchronous, so the connect verdict lands one event-loop
    // turn later at the earliest; the safety-net timer runs until then and
    // applyScanResult() stops it on success.
    qInfo() << "Gamepad: WinMM scanning for joysticks in the background";
    m_rescanTimer->start();
    rescan();
    return true;
}

ControlId::DeviceProfile WinMMDevice::profile() const
{
    if (!m_connected)
        return {};
    const QString model = QStringLiteral("%1:%2")
                              .arg(m_vendorId, 4, 16, QLatin1Char('0'))
                              .arg(m_productId, 4, 16, QLatin1Char('0'));
    const QString endpoint = QStringLiteral("winmm.slot%1").arg(m_activeId);
    return ControlId::DeviceProfile{
        QStringLiteral("WinMM joystick"),
        endpoint,
        m_ds4Layout ? ControlId::ControllerFamily::PlayStation : ControlId::ControllerFamily::Xbox,
        m_ds4Layout ? QStringLiteral("WinMM joystick (Sony layout)")
                    : QStringLiteral("WinMM joystick (Xbox layout)"),
        model,
        endpoint,
    };
}

void WinMMDevice::rescan()
{
    if (m_connected || m_scanInFlight)
        return;

    // The sweep asks the driver stack about up to 16 mostly absent devices —
    // measured at 161 ms on a real machine, far too slow for the GUI thread.
    // The safety-net timer and all state stay on this thread; only the
    // joyGetNumDevs/joyGetPosEx/joyGetDevCaps work runs on the worker.
    m_scanInFlight = true;
    if (m_scanThread.joinable())
        m_scanThread.join();   // the previous worker already posted its result
    m_scanThread = std::thread([this] {
        const ScanResult result = scanSlots();
        // Queued metacall onto the owning thread. The destructor joins this
        // worker, so `this` outlives the call; a metacall posted to an
        // object that is destroyed before delivery is discarded by Qt.
        QMetaObject::invokeMethod(
            this, [this, result] { applyScanResult(result); }, Qt::QueuedConnection);
    });
}

WinMMDevice::ScanResult WinMMDevice::scanSlots()
{
    ScanResult result;
    QElapsedTimer pass;
    pass.start();
    const UINT numDevs = joyGetNumDevs();
    for (UINT id = 0; id < numDevs && id < kMaxSlots; ++id) {
        JOYINFOEX info{};
        info.dwSize = sizeof(info);
        info.dwFlags = JOY_RETURNALL;

        if (joyGetPosEx(id, &info) != JOYERR_NOERROR)
            continue;

        // The button ORDER depends on the pad family: Sony pads (and DSX's
        // virtual Sony pads) put Share at button 8 and PS at button 12,
        // Xbox-style pads put Back/Start at 6/7. joyGetDevCaps exposes the
        // vendor/product id, so pick the mapping from that.
        JOYCAPSW caps{};
        if (joyGetDevCapsW(id, &caps, sizeof(caps)) == JOYERR_NOERROR) {
            result.mid = caps.wMid;
            result.pid = caps.wPid;
        }
        result.found = true;
        result.id = id;
        break;
    }
    result.elapsedUs = pass.nsecsElapsed() / 1000;
    return result;
}

void WinMMDevice::applyScanResult(const ScanResult& result)
{
    m_scanInFlight = false;
    // Off the GUI thread the sweep duration is diagnostic only — report it
    // when clearly pathological, not at the GUI-blocking threshold (a
    // joystick-less machine sweeps every 2 s and must not fill the log).
    PerfTrace::reportSlow("WinMM background device rescan", result.elapsedUs, 500000);
    if (m_connected || !result.found)
        return;

    // The WinMM view of an XInput pad is NOT filtered out here: VID:PID
    // names a model, not a physical endpoint, so suppressing on it could
    // hide a legacy-only pad that shares its model with an XInput one.
    // The mirrored event stream is harmless — backend arbitration drops
    // cross-backend presses inside the duplicate window at birth
    // (InputEngine::onControlPressed + heldPressSurvives).

    m_activeId = result.id;
    m_connected = true;
    m_prevButtons = 0;

    m_ds4Layout = (result.mid == 0x054C)
        || (result.mid == 0x11FF && result.pid == 0x0847)
        || (result.mid == 0x3670 && result.pid == 0x0902);
    m_vendorId = result.mid;
    m_productId = result.pid;
    qInfo() << "Gamepad: WinMM joystick connected (JOYSTICKID" << (result.id + 1)
            << ") VID" << Qt::hex << result.mid << "PID" << result.pid << Qt::dec
            << (m_ds4Layout ? "— Sony button layout" : "— Xbox button layout");
    m_rescanTimer->stop();
    m_pollTimer->start();
    emit connected(true);
}

void WinMMDevice::poll()
{
    if (!m_connected)
        return;

    JOYINFOEX info{};
    info.dwSize = sizeof(info);
    info.dwFlags = JOY_RETURNALL;

    QElapsedTimer call;
    call.start();
    const MMRESULT result = joyGetPosEx(m_activeId, &info);
    PerfTrace::reportSlow("WinMM joyGetPosEx poll", call.nsecsElapsed() / 1000);

    if (result == JOYERR_UNPLUGGED) {
        disconnectActive();
        return;
    }

    if (result != JOYERR_NOERROR)
        return;   // transient error — keep the slot, skip this tick

    emitEdges(mapWinMMState(info, m_ds4Layout));
}

void WinMMDevice::disconnectActive()
{
    qInfo() << "Gamepad: WinMM joystick disconnected (JOYSTICKID" << (m_activeId + 1) << ")";
    // Release anything still held before announcing the disconnect, so no
    // button can stay logically stuck across an unplug.
    emitEdges(0);
    m_connected = false;
    m_activeId = UINT_MAX;
    m_pollTimer->stop();
    m_rescanTimer->start();
    emit connected(false);
}

void WinMMDevice::emitEdges(quint32 buttons)
{
    const quint32 changed = buttons ^ m_prevButtons;
    if (changed == 0)
        return;
    for (int b = 0; b < MaxButtons; ++b) {
        const quint32 mask = 1u << b;
        if (!(changed & mask))
            continue;
        const QString control = (b == Share && !m_ds4Layout)
            ? ControlId::ViewBack : controlIdFor(b);
        if (buttons & mask)
            publishControlPressed(control);
        else
            publishControlReleased(control);
    }
    m_prevButtons = buttons;
}
