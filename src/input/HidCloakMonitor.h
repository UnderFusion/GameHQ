#pragma once
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

// Detects supported Sony/DS4 pads that Windows PnP knows about but that Raw
// Input cannot see — the signature of a HID filter driver (Nefarius HidHide,
// installed alongside DSX / DS4Windows / reWASD) cloaking the device from
// non-whitelisted applications. Also provides the one-click remedy: adding
// this executable to HidHide's application allow-list through the driver's
// documented control-device interface (docs/controller-input.md).
// Win32 lives in the .cpp only.
namespace HidCloakMonitor
{
struct ScanResult {
    QStringList hiddenPads;       // display names, e.g. "DualSense (VID_054C&PID_0CE6)"
    bool hidHidePresent = false;  // HidHide class filter installed on this machine
};

// Cross-checks the PnP device tree against what Raw Input enumerates.
// visibleRawPathsLower: RIDI_DEVICENAME of every HID device Raw Input
// currently lists, lowercased by the caller.
ScanResult scan(const QSet<QString>& visibleRawPathsLower);

// Device interface paths of physical Sony pads (DualSense, Edge, DS4) that
// PnP lists but Raw Input does not. Virtual DSX pads are excluded: they
// mirror the physical pad and would only duplicate its input. IG_ (XInput)
// collections are excluded too.
struct HiddenInterface { QString path; quint32 vendorId = 0; quint32 productId = 0; };
QList<HiddenInterface> hiddenPadInterfaces(const QSet<QString>& visibleRawPathsLower);

// True if the HidHide upper filter is registered on the HID device class
// (plain registry read, no admin rights needed).
bool hidHideInstalled();

// Body of the elevated helper mode (--hidhide-allow-self): appends this
// executable's NT image path to HidHide's application whitelist via the
// \\.\HidHide control device. Returns 0 on success (process exit code).
int applyWhitelistSelfElevated();

// Relaunches this executable elevated with --hidhide-allow-self (one UAC
// prompt). Returns the process HANDLE for the caller to poll, or nullptr if
// the user declined elevation / launch failed. Caller owns the handle.
void* launchElevatedWhitelistHelper();
}
