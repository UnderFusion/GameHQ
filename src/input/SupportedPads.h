#pragma once

#include <QtGlobal>

// The VID/PID values of the Sony and DS4-compatible pads GameHQ reads
// directly. One place for the numbers; each consumer keeps its own named set
// because the sets differ on purpose:
//   - DualSenseDevice (supportedReportLayout) decodes every pad below,
//     including the 054C:0ECC virtual DualSense.
//   - HidCloakMonitor compares PnP against Raw Input on kCloakComparablePads,
//     which omits 054C:0ECC (see the comment on that list).
//   - WinMMDevice picks the Sony button layout for any Sony VID plus the two
//     virtual DS4 VID:PID pairs (usesSonyButtonLayout).
namespace SupportedPads
{
inline constexpr quint32 kSonyVid             = 0x054C;
inline constexpr quint32 kDualSensePid        = 0x0CE6;
inline constexpr quint32 kDualSenseEdgePid    = 0x0DF2;
inline constexpr quint32 kVirtualDualSensePid = 0x0ECC;   // DSX/ViGEm virtual DualSense
inline constexpr quint32 kDualShock4V1Pid     = 0x05C4;
inline constexpr quint32 kDualShock4V2Pid     = 0x09CC;
inline constexpr quint32 kVirtualDs4VidA      = 0x11FF;
inline constexpr quint32 kVirtualDs4PidA      = 0x0847;
inline constexpr quint32 kVirtualDs4VidB      = 0x3670;
inline constexpr quint32 kVirtualDs4PidB      = 0x0902;

// The two non-Sony VID:PID pairs that speak the DS4 report format.
inline constexpr bool isVirtualDs4(quint32 vendorId, quint32 productId)
{
    return (vendorId == kVirtualDs4VidA && productId == kVirtualDs4PidA)
        || (vendorId == kVirtualDs4VidB && productId == kVirtualDs4PidB);
}

// WinMMDevice: the joystick reports Sony button numbering.
inline constexpr bool usesSonyButtonLayout(quint32 vendorId, quint32 productId)
{
    return vendorId == kSonyVid || isVirtualDs4(vendorId, productId);
}

struct KnownPad { quint32 vid; quint32 pid; const char* name; };

// HidCloakMonitor: mirrors supportedReportLayout() in DualSenseDevice. 054C:0ECC
// is deliberately absent: on PlayStation Link hardware that PID exposes only
// vendor-defined collections which Raw Input legitimately lists, so a
// PnP-vs-Raw-Input comparison on it would never be meaningful.
inline constexpr KnownPad kCloakComparablePads[] = {
    { kSonyVid,        kDualSensePid,     "DualSense" },
    { kSonyVid,        kDualSenseEdgePid, "DualSense Edge" },
    { kSonyVid,        kDualShock4V1Pid,  "DualShock 4" },
    { kSonyVid,        kDualShock4V2Pid,  "DualShock 4 v2" },
    { kVirtualDs4VidA, kVirtualDs4PidA,   "DSX virtual DS4" },
    { kVirtualDs4VidB, kVirtualDs4PidB,   "DSX virtual DS4" },
};
} // namespace SupportedPads
