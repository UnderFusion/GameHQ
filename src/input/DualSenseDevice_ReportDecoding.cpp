// DualSenseDevice: report decoding (buttons, stick navigation), report
// parsing, active-pad routing and edge emission. Split out of
// DualSenseDevice.cpp; the class itself is declared in DualSenseDevice.h.
#include "input/DualSenseDevice.h"
#include "input/DualSenseDeviceInternal.h"

#include "input/ControllerArbitration.h"
#include "input/InputDiagnostics.h"
#include "input/SonyReportLayout.h"
#include "input/StickNav.h"

#include <QDebug>
#include <QTimer>

#include <windows.h>

using namespace DualSenseInternal;

// Face buttons, shoulder/trigger edges, Share/Options/PS, and the D-pad hat.
quint32 DualSenseDevice::decodeButtons(const unsigned char* d, int base)
{
    const unsigned char b0 = d[base];        // dpad hat (low nibble) + face buttons
    const unsigned char b1 = d[base + 1];    // L1/R1/Share/Options/...
    const unsigned char b2 = d[base + 2];    // PS/touchpad/mute
    const int hat = b0 & 0x0F;

    quint32 s = 0;
    auto set = [&s](int btn) { s |= (1u << btn); };

    if (b0 & 0x10) set(Square);
    if (b0 & 0x20) set(Cross);
    if (b0 & 0x40) set(Circle);
    if (b0 & 0x80) set(Triangle);
    if (b1 & 0x01) set(L1);
    if (b1 & 0x02) set(R1);
    // The triggers also report a digital edge here alongside their analog axis;
    // that edge is all this app binds, so the axis is left unread.
    if (b1 & 0x04) set(L2);
    if (b1 & 0x08) set(R2);
    if (b1 & 0x10) set(Share);      // "Create" button
    if (b1 & 0x20) set(Options);
    if (b1 & 0x40) set(L3);
    if (b1 & 0x80) set(R3);
    if (b2 & 0x01) set(PS);

    switch (hat) {                  // 0..7 = 8 directions, 8 = neutral
    case 0: set(DpadUp); break;
    case 1: set(DpadUp);   set(DpadRight); break;
    case 2: set(DpadRight); break;
    case 3: set(DpadDown); set(DpadRight); break;
    case 4: set(DpadDown); break;
    case 5: set(DpadDown); set(DpadLeft); break;
    case 6: set(DpadLeft); break;
    case 7: set(DpadUp);   set(DpadLeft); break;
    default: break;
    }
    return s;
}

// Left stick doubles as the D-pad for menu navigation (overlay request:
// "D-pad or left stick"). Axis offsets come from SonyReportLayout::locate(),
// 0..255 with ~128 center and Y growing downward. A wide deadzone avoids
// drift; this is the one backend that runs the hysteresis path, so its
// return zone is tighter than its deadzone (see StickNav.h for why).
// emitEdges() in the caller only fires once per direction crossed, same as
// a real button — no auto-repeat while held, matching the D-pad.
// Hysteresis state comes from st.stick (last frame's stick bits).
quint32 DualSenseDevice::decodeStickNav(const DeviceState& st, const unsigned char* d,
                                        int axisBase, int len)
{
    constexpr StickNav::AxisConfig kNav{ 128, 60, 30, false };

    if (axisBase < 1 || len <= axisBase + 1)
        return 0;

    quint32 out = StickNav::bits(kNav, d[axisBase], d[axisBase + 1], st.stick);
    // Right stick sits two bytes after the left on both families (LX LY RX RY);
    // its vertical directions become the wheel-like scroll controls.
    if (len > axisBase + 3)
        out |= StickNav::verticalBits(kNav, d[axisBase + 3],
                                      Gamepad::RStickUp, Gamepad::RStickDown,
                                      st.stick);
    return out;
}

void DualSenseDevice::parseReport(void* handle, DeviceState& st,
                                  const unsigned char* d, int len, const char* provider)
{
    if (len < 1)
        return;

    const unsigned char reportId = d[0];
    const auto layout = SonyReportLayout::locate(
        reportId,
        st.layout == LayoutDs4 ? SonyReportLayout::Family::Ds4
                               : SonyReportLayout::Family::DualSense,
        len);
    if (!layout.valid() || len < layout.buttons + 3)
        return;

    // One line per pad whenever its report shape changes (first report, or
    // a Bluetooth pad switching simple -> full), so a user log shows which
    // layout was decoded without dumping every report.
    const int shape = (int(reportId) << 16) | (len & 0xFFFF);
    if (shape != st.reportShape) {
        st.reportShape = shape;
        qInfo().noquote() << QStringLiteral(
            "Gamepad: input layout %1 provider=%7 report_id=0x%2 length=%3 "
            "variant=%4 axes=%5 buttons=%6")
            .arg(deviceIdentity(st.vendorId, st.productId))
            .arg(reportId, 2, 16, QLatin1Char('0'))
            .arg(len).arg(QLatin1String(layout.variant))
            .arg(layout.axes).arg(layout.buttons)
            .arg(QLatin1String(provider));
    }

    st.reported = true;
    st.lastReportMs = m_clock.elapsed();

    // Any valid Sony report proves a pad is alive — cancel a pending
    // disconnect. If the active pad was just removed, this report's device
    // becomes the new active pad in routeReport.
    if (m_disconnectTimer->isActive()) {
        m_disconnectTimer->stop();
        qInfo() << "Gamepad:" << padName(st.layout)
                << "reports resumed before disconnect debounce";
    }

    quint32 s = decodeButtons(d, layout.buttons);
    st.stick = decodeStickNav(st, d, layout.axes, len);
    s |= st.stick;

    const bool changed = (s != st.buttons);
    if (changed && m_probing)
        noteProbeButtonChange(st, st.buttons, s);
    st.buttons = s;
    if (changed)
        st.lastChangeMs = st.lastReportMs;

    routeReport(handle, st, s, changed, reportId, d, len);
}

// Decide which pad the decoded state drives: first reporting pad becomes
// active, the active pad just emits, and a non-active pad may steal the
// active role only on a real input change while the active pad is silent.
void DualSenseDevice::routeReport(void* handle, const DeviceState& st, quint32 s, bool changed,
                                  unsigned char reportId, const unsigned char* d, int len)
{
    if (!m_activeHandle) {
        // First reporting pad (or the previous one just vanished) takes over.
        m_activeHandle = handle;
        qInfo() << "Gamepad:" << padName(st.layout) << "active (report id"
                << Qt::hex << reportId << ")";
        if (!m_connectedState) {
            m_connectedState = true;
            emit connected(true);
        }
        emitEdges(s);
        return;
    }

    if (handle == m_activeHandle) {
        // On any button change, dump the raw report so offsets can be
        // confirmed/corrected against real hardware.
        if (changed) {
            const int n = qMin(len, 16);
            QString hex;
            for (int i = 0; i < n; ++i)
                hex += QString::asprintf("%02X ", d[i]);
            qInfo().noquote() << "Gamepad: report" << hex.trimmed();
        }
        emitEdges(s);
        return;
    }

    // Report from a non-active pad: keep its state current. Prefer physical
    // Sony hardware over virtual DS4 devices immediately; otherwise fail over
    // when the active device has stopped producing real control changes.
    // Idle report traffic no longer pins a noisy virtual pad forever.
    auto activeIt = m_devices.constFind(m_activeHandle);
    const bool shouldSwitch = activeIt == m_devices.cend()
        || ControllerArbitration::sonyDeviceMayTakeOver(
            changed,
            devicePriority(st.vendorId, st.productId),
            devicePriority(activeIt->vendorId, activeIt->productId),
            st.lastChangeMs,
            activeIt->lastChangeMs,
            kActiveSilenceMs);
    if (shouldSwitch) {
        qInfo() << "Gamepad: switching active pad to" << padName(st.layout)
                << "(better device or previous control source went idle)";
        m_activeHandle = handle;
        if (!m_connectedState) {
            m_connectedState = true;
            emit connected(true);
        }
        emitEdges(s);
    }
}

void DualSenseDevice::emitEdges(quint32 buttons)
{
    const quint32 changed = buttons ^ m_emittedButtons;
    if (changed == 0)
        return;
    for (int b = 0; b < MaxButtons; ++b) {
        const quint32 mask = 1u << b;
        if (!(changed & mask))
            continue;
        if (buttons & mask)
            publishButtonPressed(b);
        else
            publishButtonReleased(b);
    }
    m_emittedButtons = buttons;
}
