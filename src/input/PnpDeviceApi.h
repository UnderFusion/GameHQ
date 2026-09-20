#pragma once

#include <QString>

// Injectable seam over the Windows PnP topology queries that answer "which
// physical device is this?" with evidence the OS itself maintains.
//
// WHY THIS EXISTS: every provider GameHQ speaks sees the same pad under a
// different name — Sony Raw Input has a HID interface path, XInput has a slot
// number, GameInput has an app-local device ID — and none of those names can
// be compared across providers. Windows keeps one piece of per-device topology
// that all of them share: the PnP *container ID* of the devnode, which groups
// every interface that physically belongs to one device. cpo-c03 needs exactly
// that evidence, and this class is the only place allowed to ask for it.
//
// Two rules decide the contract:
//   * Fail closed. `queried == false` means the OS refused to answer (device
//     gone, property missing, cloaked node). It is NOT "no container", and the
//     caller must never turn it into a correlation guess.
//   * Answer fresh. The seam caches nothing. A container read during an
//     earlier arrival says nothing about the device attached now — Windows
//     reuses interface paths and device instance IDs.
//
// Production wraps CfgMgr32 (`PnpDeviceApi::createSystem()`); tests substitute
// a fake (tests/tst_pnpdeviceapi.cpp). The header stays platform-clean: no
// windows.h, no CONFIGRET.
class PnpDeviceApi
{
public:
    virtual ~PnpDeviceApi();

    // DEVPKEY_Device_InstanceId answer for one interface path.
    struct InstanceId {
        bool queried = false;
        QString value;  // canonical device instance ID, e.g. HID\VID_054C&PID_0CE6\7&...
    };

    // DEVPKEY_Device_ContainerId answer for one device instance.
    struct Container {
        bool queried = false;
        QString value;  // canonical "{guid}", lower case
    };

    virtual InstanceId deviceInstanceIdForInterfacePath(const QString& interfacePath) = 0;
    virtual Container containerIdForDeviceInstance(const QString& deviceInstanceId) = 0;

    // The two OS queries above, chained. Fail-closed in both directions: an
    // unanswerable interface path never reaches the container query, and the
    // path text itself is never parsed — provider spelling is not topology
    // evidence, so there is no lexical fallback here by design.
    Container containerIdForInterfacePath(const QString& interfacePath);

    // Canonical comparison forms. These are pure, and they are the only
    // sanctioned way to decide whether two identities that came from different
    // Windows APIs mean the same device.
    static QString normalizeInterfacePath(const QString& raw);
    static QString normalizeDeviceInstanceId(const QString& raw);
    // Canonicalises any GUID spelling ("{...}", bare, any case); returns an
    // empty string when the input is not a GUID, so a malformed value can
    // never compare equal to a real container.
    static QString normalizeContainerId(const QString& raw);
    // WinRT and GameInput hand the container out as the 16 raw bytes of the
    // GUID, and the app stores those bytes hex-encoded (see `bytesId` in
    // ProductionGameInputApi.cpp). Converts such a hex string into the same
    // "{guid}" text CfgMgr32 reports, so a container observed on either side
    // can be compared byte for byte.
    static QString containerFromRawBytesHex(const QString& hex);

    // CfgMgr32-backed implementation. Caller owns the returned object.
    static PnpDeviceApi* createSystem();
};
