#pragma once
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QtGlobal>

// Injectable seam over every OS query the Sony Raw Input backend performs.
//
// WHY THIS EXISTS: `DualSenseDevice` receives WM_INPUT on the GUI thread, and
// controllers sold today advertise 8000 Hz polling. A device this app does not
// drive must therefore cost one header read plus one hash lookup — no payload
// fetch, no allocation, no log line. That guarantee is only worth anything if
// it can be measured, and a real `HRAWINPUT` cannot be fabricated in a test:
// the OS hands them out and invalidates them when the message is dispatched.
// So all of Win32 lives behind this interface. Production wraps the real API
// (`RawInputApi::createSystem()`); tests substitute a fake that counts calls
// (tests/tst_rawinputflood.cpp).
//
// Everything here is deliberately platform-clean: no windows.h in the header.
class RawInputApi
{
public:
    virtual ~RawInputApi();

    enum class DeviceType {
        Other,   // mouse, keyboard, or anything else Raw Input reports
        Hid      // RIM_TYPEHID — the only type this backend parses
    };

    // What a WM_INPUT event is, without touching its payload.
    struct Header {
        void* device = nullptr;
        DeviceType type = DeviceType::Other;
    };

    // RIDI_DEVICEINFO answer. `queried == false` means the OS refused to
    // answer (device unplugged mid-query, transient failure) — that is NOT a
    // classification and must never be cached as "ignored".
    struct DeviceInfo {
        bool queried = false;
        bool isHid = false;
        quint32 vendorId = 0;
        quint32 productId = 0;
        quint16 usagePage = 0;
        quint16 usage = 0;
    };

    // RIDI_DEVICENAME answer, same `queried` contract.
    struct DevicePath {
        bool queried = false;
        QString value;
    };

    // One WM_INPUT payload: `reportCount` HID reports of `reportSize` bytes
    // each, laid out back to back. The buffer belongs to the API object and
    // stays valid only until the next readPayload() call — parse it, don't
    // keep it.
    struct Payload {
        const unsigned char* reports = nullptr;
        int reportCount = 0;
        int reportSize = 0;
    };

    struct EnumeratedDevice {
        void* handle = nullptr;
        DeviceType type = DeviceType::Other;
    };

    // Result of cross-checking Windows PnP against Raw Input (HidHide cloak
    // detection). Mirrors HidCloakMonitor::ScanResult, kept behind the seam so
    // the tests never touch the machine's real device tree.
    struct CloakScan {
        QStringList hiddenPads;
        bool hidHidePresent = false;
    };

    // Hot path — called once per WM_INPUT, must not allocate.
    virtual bool readHeader(void* rawInputHandle, Header& out) = 0;
    // Only reached for devices this backend actually drives.
    virtual bool readPayload(void* rawInputHandle, Payload& out) = 0;

    // Classification queries — called once per handle, then cached by the
    // caller. Both report transient failure separately from a real answer.
    virtual DeviceInfo describeDevice(void* deviceHandle) = 0;
    virtual DevicePath devicePath(void* deviceHandle) = 0;

    // Every device Raw Input currently lists (all types, so the caller can
    // prune cached classifications for handles Windows no longer knows).
    virtual QList<EnumeratedDevice> enumerateDevices() = 0;

    virtual CloakScan scanHiddenPads(const QSet<QString>& visibleRawPathsLower) = 0;

    // Physical Sony pad HID interfaces present in Windows PnP but absent from
    // Raw Input (HidHide-cloaked). Raw Input stays blind to them even after
    // GameHQ is whitelisted, because the system opens Raw Input devices, not
    // this process; only a direct CreateFile from GameHQ passes the whitelist.
    struct HiddenInterface {
        QString path;          // HID device interface path ("\\?\HID#...")
        quint32 vendorId = 0;
        quint32 productId = 0;
    };
    virtual QList<HiddenInterface> hiddenPadInterfaces(const QSet<QString>& visibleRawPathsLower)
    {
        Q_UNUSED(visibleRawPathsLower)
        return {};
    }

    using ButtonUsageVisitor = void (*)(void* context,
                                        const QList<quint32>& pressedUsages);

    // Pressed button usages ((usagePage << 16) | usage) decoded from EVERY
    // report in `payload`, in order, using the device's own HID report
    // descriptor. Returns false when
    // the device's descriptor cannot be obtained or parsed — the caller must
    // treat that as "no fallback available", never as "all released".
    // Only reached for devices the selective Raw HID fallback explicitly
    // observes (bound controls or an active probe), never on the idle path.
    virtual bool visitButtonUsageReports(void* deviceHandle, const Payload& payload,
                                         void* context,
                                         const ButtonUsageVisitor& visitor)
    {
        Q_UNUSED(deviceHandle) Q_UNUSED(payload) Q_UNUSED(context) Q_UNUSED(visitor)
        return false;
    }

    // Invalidates every descriptor/parser buffer associated with a live Raw
    // Input handle. The caller invokes this on removal, arrival re-evaluation,
    // and reconcile pruning because Windows can reuse handle values.
    virtual void forgetDevice(void* deviceHandle) { Q_UNUSED(deviceHandle) }

    // Win32-backed implementation. Caller owns the returned object.
    static RawInputApi* createSystem();
};
