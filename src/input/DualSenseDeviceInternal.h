#pragma once

// Private to the DualSenseDevice translation units (DualSenseDevice*.cpp):
// the constants and small helpers they share. Not part of the class API.

#include "input/SupportedPads.h"

#include <QString>

#include <windows.h>

namespace DualSenseInternal
{
inline constexpr USHORT kUsagePageGeneric = 0x01;
inline constexpr USHORT kUsageJoystick    = 0x04;
inline constexpr USHORT kUsageGamepad     = 0x05;
inline constexpr USHORT kUsageMultiAxis   = 0x08;

// A pad streaming reports sends one every few milliseconds even when idle.
// If the active pad has been silent this long while ANOTHER tracked pad
// shows a real button/stick change, the active role fails over to it.
inline constexpr qint64 kActiveSilenceMs = 1000;
// A removed/silent pad is only declared disconnected after this debounce —
// USB/Bluetooth re-enumeration makes pads blink out for a moment.
inline constexpr int kDisconnectDebounceMs = 1500;
// Arrival/removal bursts (Windows re-enumerates the whole HID tree at once)
// collapse into one reconciliation pass and one topology hint.
inline constexpr int kTopologyDebounceMs = 400;
// How often the aggregated per-device event-rate counters are turned into (at
// most) one log line each. Never log per event: an 8 kHz pad would write 8000
// lines a second and the diagnostic would become the outage.
inline constexpr int kRateSampleMs = 5000;
inline constexpr int kSilenceFollowupMs = 15000;
// Direct HID reader arbitration. The reader forwards a heartbeat at least
// every SonyHidReader::kHeartbeatMs while the pad streams, so a reader that
// forwarded something this recently is healthy and Raw Input is dropped.
inline constexpr qint64 kHidFreshMs = 250;
// Raw Input kept delivering this long without the reader matching it: the
// reader is stuck, Raw Input becomes the source again. The grace also covers
// the ordinary race where WM_INPUT for a report is dispatched before the
// reader's queued copy of the same report.
inline constexpr qint64 kHidFailoverMs = 300;
// A rescue reader hands the endpoint back once Raw Input has delivered for
// this long without a gap above kRescueRawGapMs; a stray late WM_INPUT
// must not bounce the pad back to a source that is still mostly silent.
inline constexpr qint64 kRescueHandbackMs = 1000;
inline constexpr qint64 kRescueRawGapMs = 250;

enum ReportLayout {
    LayoutUnknown = 0,
    LayoutDualSense,
    LayoutDs4
};

inline int supportedReportLayout(quint32 vendorId, quint32 productId)
{
    using namespace SupportedPads;
    if (vendorId == kSonyVid) {
        if (productId == kDualSensePid || productId == kDualSenseEdgePid
            || productId == kVirtualDualSensePid)
            return LayoutDualSense;
        if (productId == kDualShock4V1Pid || productId == kDualShock4V2Pid)
            return LayoutDs4;
    }

    if ((vendorId == kVirtualDs4VidA && productId == kVirtualDs4PidA)
        || (vendorId == kVirtualDs4VidB && productId == kVirtualDs4PidB))
        return LayoutDs4;

    return LayoutUnknown;
}

inline bool isGamepadUsage(quint16 usagePage, quint16 usage)
{
    return usagePage == kUsagePageGeneric
        && (usage == kUsageGamepad
            || usage == kUsageJoystick
            || usage == kUsageMultiAxis);
}

inline const char* padName(int layout)
{
    return layout == LayoutDs4 ? "DS4-compatible" : "DualSense";
}

inline int devicePriority(quint32 vendorId, quint32 productId)
{
    using namespace SupportedPads;
    if (vendorId == kSonyVid) {
        if (productId == kVirtualDualSensePid)
            return 2;
        return 3;
    }
    return 1;
}

// m_devices keys for HID-only endpoints. Raw Input hDevice values are kernel
// handle values far below this tag, so the two key spaces never collide, and
// synthetic keys never reach a Raw Input API call.
inline constexpr quintptr kHidOnlyKeyTag = quintptr(0xC10A) << 48;
inline void* hidOnlyKey(quint64 n) { return reinterpret_cast<void*>(kHidOnlyKeyTag | quintptr(n)); }

inline QString deviceIdentity(quint32 vendorId, quint32 productId)
{
    return QString::asprintf("%04x:%04x", vendorId, productId);
}
} // namespace DualSenseInternal
