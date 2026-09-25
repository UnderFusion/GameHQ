#include <QtTest>

#include <atomic>
#include <cstdlib>
#include <new>

#include "input/ControlId.h"
#include "input/ControllerIdentity.h"
#include "input/DualSenseDevice.h"
#include "input/InputDiagnostics.h"
#include "input/RawInputApi.h"
#include "input/SelectiveRawHidFallback.h"

// Allocation counter. Replacing the global operator new is the only way to
// answer "did this path allocate?" for code compiled into this executable.
// Qt's own allocations happen inside Qt6Core.dll and are therefore invisible
// here — which is exactly why the seam's payload-read counter, not this
// number, is the primary evidence in the flood test. Both must be zero.
namespace
{
std::atomic<qint64> g_allocations{ 0 };
std::atomic<bool> g_countAllocations{ false };
} // namespace

void* operator new(std::size_t size)
{
    if (g_countAllocations.load(std::memory_order_relaxed))
        g_allocations.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(size ? size : 1);
    if (!p)
        throw std::bad_alloc();
    return p;
}

void* operator new[](std::size_t size)
{
    return ::operator new(size);
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace
{
constexpr quint16 kUsagePageGeneric = 0x01;
constexpr quint16 kUsageGamepad     = 0x05;
constexpr quint16 kUsageVendor      = 0x00;
constexpr quint32 kSonyVid          = 0x054C;
constexpr quint32 kDualSensePid     = 0x0CE6;
constexpr quint32 kGameSirVid       = 0x3537;
constexpr quint32 kGameSirPid       = 0x1004;

void* handle(quintptr id)
{
    return reinterpret_cast<void*>(id);
}

// A DualSense USB input report (id 0x01): left stick centred, D-pad hat
// neutral (0x8) and Cross held. Offsets per docs/controller-input.md.
QByteArray dualSenseReport(bool crossHeld)
{
    QByteArray report(16, '\0');
    report[0] = 0x01;
    report[1] = char(128);   // LX centre
    report[2] = char(128);   // LY centre
    report[3] = char(128);   // RX centre
    report[4] = char(128);   // RY centre — a zero here would read as stick-up
    report[8] = char(crossHeld ? 0x28 : 0x08);
    return report;
}

// DualSense state reports in every transport, written from the protocol
// (SDL_hidapi_ps5.c / Linux hid-playstation.c), not from SonyReportLayout, so
// the test cannot inherit a wrong offset. b0 = hat+face, b1 = L1 R1 L2 R2
// Create Options L3 R3, b2 = PS. Axes centred unless given.
enum class DsTransport { Usb, BtSimple10, BtSimple78, BtFull };

QByteArray dsReport(DsTransport t, quint8 b0, quint8 b1 = 0, quint8 b2 = 0,
                    quint8 lx = 128, quint8 ly = 128)
{
    int len = 64, axes = 1, buttons = 8;
    char id = 0x01;
    switch (t) {
    case DsTransport::Usb:        break;
    case DsTransport::BtSimple10: len = 10; buttons = 5; break;
    case DsTransport::BtSimple78: len = 78; buttons = 5; break;
    case DsTransport::BtFull:     len = 78; axes = 2; buttons = 9; id = 0x31; break;
    }
    QByteArray r(len, '\0');
    r[0] = id;
    if (t == DsTransport::BtFull)
        r[1] = char(0x10);            // sequence tag byte
    r[axes] = char(lx);
    r[axes + 1] = char(ly);
    r[axes + 2] = char(128);
    r[axes + 3] = char(128);
    r[buttons] = char(b0);
    r[buttons + 1] = char(b1);
    r[buttons + 2] = char(b2);
    return r;
}

QStringList controlIds(const QSignalSpy& spy)
{
    QStringList out;
    for (const auto& args : spy)
        out << args.first().toString();
    out.sort();
    return out;
}

// Stands in for the whole Win32 Raw Input surface and counts every call.
//
// In this fake the WM_INPUT handle IS the device handle. Real HRAWINPUT values
// are minted by the OS and cannot be fabricated, and the backend never
// inspects one — it only hands it back to the API — so the substitution is
// faithful for everything under test.
class FakeRawInputApi final : public RawInputApi
{
public:
    struct Device {
        DeviceInfo info;
        DevicePath path;
        QByteArray report;
        DeviceType type = DeviceType::Hid;
        QList<quint32> pressedUsages;   // (page << 16) | usage for the report visitor
        QList<QList<quint32>> usageReports; // ordered states in one RAWINPUTHID batch
        bool usagesParseable = true;
        int sonyBatch = 0;              // >0: `report` holds this many equal-size Sony reports
        bool present = true;            // listed by enumerateDevices()
        int describeFailures = 0;       // transient RIDI_DEVICEINFO failures to serve first
        int pathFailures = 0;           // transient RIDI_DEVICENAME failures to serve first
    };

    QHash<void*, Device> devices;
    int headerReads = 0;
    int payloadReads = 0;
    int describeCalls = 0;
    int pathCalls = 0;
    int enumerations = 0;

    static Device hidDevice(quint32 vid, quint32 pid, quint16 usage,
                            const QString& path = QStringLiteral("\\\\?\\HID#VID"))
    {
        Device d;
        d.info.queried   = true;
        d.info.isHid     = true;
        d.info.vendorId  = vid;
        d.info.productId = pid;
        d.info.usagePage = kUsagePageGeneric;
        d.info.usage     = usage;
        d.path.queried   = true;
        d.path.value     = path;
        return d;
    }

    void resetCounters()
    {
        headerReads = payloadReads = describeCalls = pathCalls = enumerations = 0;
    }

    bool readHeader(void* rawInputHandle, Header& out) override
    {
        ++headerReads;
        auto it = devices.constFind(rawInputHandle);
        if (it == devices.cend())
            return false;
        out.device = rawInputHandle;
        out.type = it->type;
        return true;
    }

    bool readPayload(void* rawInputHandle, Payload& out) override
    {
        ++payloadReads;
        auto it = devices.constFind(rawInputHandle);
        if (it == devices.cend() || it->report.isEmpty())
            return false;
        out.reports = reinterpret_cast<const unsigned char*>(it->report.constData());
        if (it->sonyBatch > 0) {
            out.reportCount = it->sonyBatch;
            out.reportSize = static_cast<int>(it->report.size()) / it->sonyBatch;
            return true;
        }
        out.reportCount = qMax(1, static_cast<int>(it->usageReports.size()));
        out.reportSize = static_cast<int>(it->report.size());
        return true;
    }

    DeviceInfo describeDevice(void* deviceHandle) override
    {
        ++describeCalls;
        auto it = devices.find(deviceHandle);
        if (it == devices.end())
            return {};
        if (it->describeFailures > 0) {
            --it->describeFailures;
            return {};   // queried == false: the OS refused to answer
        }
        return it->info;
    }

    DevicePath devicePath(void* deviceHandle) override
    {
        ++pathCalls;
        auto it = devices.find(deviceHandle);
        if (it == devices.end())
            return {};
        if (it->pathFailures > 0) {
            --it->pathFailures;
            return {};
        }
        return it->path;
    }

    QList<EnumeratedDevice> enumerateDevices() override
    {
        ++enumerations;
        QList<EnumeratedDevice> out;
        for (auto it = devices.cbegin(); it != devices.cend(); ++it) {
            if (it->present)
                out.append({ it.key(), it->type });
        }
        return out;
    }

    CloakScan scanHiddenPads(const QSet<QString>&) override { return {}; }

    // Selective Raw HID fallback: the fake "parses" the report descriptor by
    // returning the pressed usages staged on the device.
    int usageParses = 0;
    int parserBuilds = 0;
    int parserInvalidations = 0;
    QSet<void*> parserCache;
    bool visitButtonUsageReports(void* deviceHandle, const Payload&, void* context,
                                 const ButtonUsageVisitor& visitor) override
    {
        ++usageParses;
        auto it = devices.constFind(deviceHandle);
        if (it == devices.cend() || !it->usagesParseable)
            return false;
        if (!parserCache.contains(deviceHandle)) {
            parserCache.insert(deviceHandle);
            ++parserBuilds;
        }
        if (it->usageReports.isEmpty())
            visitor(context, it->pressedUsages);
        else
            for (const auto& report : it->usageReports)
                visitor(context, report);
        return true;
    }

    void forgetDevice(void* deviceHandle) override
    {
        if (parserCache.remove(deviceHandle))
            ++parserInvalidations;
    }
};
} // namespace

class RawInputFloodTest : public QObject
{
    Q_OBJECT

private slots:
    // The GameSir report that started this: a pad GameHQ does not drive,
    // polling at up to 8000 Hz, on the GUI thread.
    void ignoredDeviceCostsOneHeaderReadPerEvent_data()
    {
        QTest::addColumn<int>("events");
        QTest::newRow("1000 Hz") << 1000;
        QTest::newRow("4000 Hz") << 4000;
        QTest::newRow("8000 Hz") << 8000;
    }

    void ignoredDeviceCostsOneHeaderReadPerEvent()
    {
        QFETCH(int, events);

        auto* api = new FakeRawInputApi;
        DualSenseDevice pad(api);   // takes ownership
        void* flood = handle(0x8001);
        api->devices.insert(flood,
                            FakeRawInputApi::hidDevice(kGameSirVid, kGameSirPid, kUsageGamepad));

        // First event classifies and obtains the stable anonymized endpoint
        // used by the Raw HID control namespace.
        pad.onRawInput(flood);
        QCOMPARE(api->describeCalls, 1);
        QCOMPARE(api->pathCalls, 1);
        QCOMPARE(api->payloadReads, 0);

        api->resetCounters();
        g_allocations.store(0, std::memory_order_relaxed);
        g_countAllocations.store(true, std::memory_order_relaxed);
        QElapsedTimer timer;
        timer.start();
        for (int i = 0; i < events; ++i)
            pad.onRawInput(flood);
        const qint64 elapsedNs = timer.nsecsElapsed();
        g_countAllocations.store(false, std::memory_order_relaxed);

        QCOMPARE(api->headerReads, events);
        QCOMPARE(api->payloadReads, 0);     // the payload is never fetched again
        QCOMPARE(api->describeCalls, 0);    // nor is the device re-classified
        QCOMPARE(api->pathCalls, 0);
        QCOMPARE(g_allocations.load(std::memory_order_relaxed), 0ll);

        // Reported, not asserted tightly: this number is the input to the
        // plan's decision on whether the backend needs its own thread.
        qInfo("%s: %d events in %.2f ms (%.0f ns/event)", QTest::currentDataTag(), events,
              elapsedNs / 1e6, double(elapsedNs) / events);
        QVERIFY(elapsedNs / events < 50000);   // 50 us/event = something is very wrong
    }

    void nonHidEventsNeverReachClassification()
    {
        auto* api = new FakeRawInputApi;
        DualSenseDevice pad(api);
        void* mouse = handle(0x8002);
        FakeRawInputApi::Device device;
        device.type = RawInputApi::DeviceType::Other;
        api->devices.insert(mouse, device);

        for (int i = 0; i < 100; ++i)
            pad.onRawInput(mouse);

        QCOMPARE(api->headerReads, 100);
        QCOMPARE(api->describeCalls, 0);
        QCOMPARE(api->payloadReads, 0);
    }

    void supportedPadStillParsesItsReports()
    {
        auto* api = new FakeRawInputApi;
        DualSenseDevice pad(api);
        void* dualSense = handle(0x8003);
        auto device = FakeRawInputApi::hidDevice(kSonyVid, kDualSensePid, kUsageGamepad);
        device.report = dualSenseReport(false);
        api->devices.insert(dualSense, device);

        QSignalSpy pressed(&pad, &Gamepad::controlPressed);
        QSignalSpy released(&pad, &Gamepad::controlReleased);
        QSignalSpy connected(&pad, &Gamepad::connected);

        pad.onRawInput(dualSense);
        QCOMPARE(api->describeCalls, 1);
        QCOMPARE(api->pathCalls, 1);        // IG_ check, once
        QCOMPARE(api->payloadReads, 1);
        QCOMPARE(connected.size(), 1);
        QCOMPARE(connected.first().first().toBool(), true);

        api->devices[dualSense].report = dualSenseReport(true);
        pad.onRawInput(dualSense);
        QCOMPARE(pressed.size(), 1);
        QCOMPARE(pressed.first().first().toString(), ControlId::FaceSouth);

        api->devices[dualSense].report = dualSenseReport(false);
        pad.onRawInput(dualSense);
        QCOMPARE(released.size(), 1);
        QCOMPARE(released.first().first().toString(), ControlId::FaceSouth);

        // Still exactly one classification after all that traffic.
        QCOMPARE(api->describeCalls, 1);
        QCOMPARE(api->pathCalls, 1);
    }

    // Every DualSense transport through the production decoder, asserting the
    // canonical controls it emits (Bluetooth offsets were one byte off).
    void dualSenseTransportsDecodeToCanonicalControls_data()
    {
        QTest::addColumn<int>("transport");
        QTest::newRow("usb") << int(DsTransport::Usb);
        QTest::newRow("bt-simple-10") << int(DsTransport::BtSimple10);
        QTest::newRow("bt-simple-78") << int(DsTransport::BtSimple78);
        QTest::newRow("bt-full-0x31") << int(DsTransport::BtFull);
    }

    void dualSenseTransportsDecodeToCanonicalControls()
    {
        QFETCH(int, transport);
        const auto t = DsTransport(transport);
        auto* api = new FakeRawInputApi;
        DualSenseDevice pad(api);
        void* ds = handle(0x8101);
        auto device = FakeRawInputApi::hidDevice(kSonyVid, kDualSensePid, kUsageGamepad);
        device.report = dsReport(t, 0x08);
        api->devices.insert(ds, device);

        QSignalSpy pressed(&pad, &Gamepad::controlPressed);
        QSignalSpy released(&pad, &Gamepad::controlReleased);
        auto feed = [&](const QByteArray& r) {
            api->devices[ds].report = r;
            pad.onRawInput(ds);
        };

        feed(dsReport(t, 0x08));
        QVERIFY2(pressed.isEmpty(), "neutral report produced a control");

        // Left stick up must be Up, never Left; down must be Down.
        feed(dsReport(t, 0x08, 0, 0, 128, 0));
        QCOMPARE(controlIds(pressed), QStringList{ControlId::DpadUp});
        feed(dsReport(t, 0x08));
        feed(dsReport(t, 0x08, 0, 0, 128, 255));
        QCOMPARE(pressed.last().first().toString(), ControlId::DpadDown);
        feed(dsReport(t, 0x08));
        pressed.clear();
        released.clear();

        // Cross + Circle, L3 + R3, Create + Options, PS.
        feed(dsReport(t, 0x08 | 0x20 | 0x40, 0x40 | 0x80 | 0x10 | 0x20, 0x01));
        QStringList expected{ControlId::FaceSouth, ControlId::FaceEast,
                             ControlId::ThumbLeft, ControlId::ThumbRight,
                             ControlId::Capture, ControlId::Menu, ControlId::Guide};
        expected.sort();
        QCOMPARE(controlIds(pressed), expected);
        feed(dsReport(t, 0x08));
        QCOMPARE(controlIds(released), expected);

        pressed.clear();
        feed(dsReport(t, 0x03));   // hat 3 = down-right
        QStringList diag{ControlId::DpadDown, ControlId::DpadRight};
        diag.sort();
        QCOMPARE(controlIds(pressed), diag);
    }

    // A Bluetooth pad can switch simple -> full mid-hold (e.g. another app
    // reads its calibration). The held button must not re-press or stick, and
    // a truncated report must not read as a release.
    void dualSenseSimpleToFullSwitchKeepsHeldButton()
    {
        auto* api = new FakeRawInputApi;
        DualSenseDevice pad(api);
        void* ds = handle(0x8102);
        auto device = FakeRawInputApi::hidDevice(kSonyVid, kDualSensePid, kUsageGamepad);
        device.report = dsReport(DsTransport::BtSimple78, 0x08);
        api->devices.insert(ds, device);
        QSignalSpy pressed(&pad, &Gamepad::controlPressed);
        QSignalSpy released(&pad, &Gamepad::controlReleased);
        auto feed = [&](const QByteArray& r) {
            api->devices[ds].report = r;
            pad.onRawInput(ds);
        };

        feed(dsReport(DsTransport::BtSimple78, 0x08));
        feed(dsReport(DsTransport::BtSimple78, 0x28));
        feed(dsReport(DsTransport::BtFull, 0x28));
        feed(dsReport(DsTransport::BtFull, 0x08).left(8));   // truncated
        QCOMPARE(controlIds(pressed), QStringList{ControlId::FaceSouth});
        QVERIFY(released.isEmpty());

        feed(dsReport(DsTransport::BtFull, 0x08));
        QCOMPARE(controlIds(released), QStringList{ControlId::FaceSouth});
        QCOMPARE(pressed.size(), 1);
    }

    // Two 0x31 reports in one RAWHID batch: press and release both surface.
    void dualSenseBluetoothBatchKeepsPressAndRelease()
    {
        auto* api = new FakeRawInputApi;
        DualSenseDevice pad(api);
        void* ds = handle(0x8103);
        auto device = FakeRawInputApi::hidDevice(kSonyVid, kDualSensePid, kUsageGamepad);
        device.report = dsReport(DsTransport::BtFull, 0x08);
        api->devices.insert(ds, device);
        QSignalSpy pressed(&pad, &Gamepad::controlPressed);
        QSignalSpy released(&pad, &Gamepad::controlReleased);

        pad.onRawInput(ds);
        api->devices[ds].report = dsReport(DsTransport::BtFull, 0x08, 0x40)
                                + dsReport(DsTransport::BtFull, 0x08);
        api->devices[ds].sonyBatch = 2;
        pad.onRawInput(ds);
        QCOMPARE(controlIds(pressed), QStringList{ControlId::ThumbLeft});
        QCOMPARE(controlIds(released), QStringList{ControlId::ThumbLeft});
    }

    void xinputCollectionIsRejectedAndRemembered()
    {
        auto* api = new FakeRawInputApi;
        DualSenseDevice pad(api);
        void* xbox = handle(0x8004);
        api->devices.insert(xbox,
                            FakeRawInputApi::hidDevice(kSonyVid, kDualSensePid, kUsageGamepad,
                                                       QStringLiteral("\\\\?\\HID#VID_045E&IG_00")));

        for (int i = 0; i < 500; ++i)
            pad.onRawInput(xbox);

        QCOMPARE(api->describeCalls, 1);
        QCOMPARE(api->pathCalls, 1);
        QCOMPARE(api->payloadReads, 0);
    }

    void vendorCollectionOnSupportedHardwareIsRejectedAndRemembered()
    {
        auto* api = new FakeRawInputApi;
        DualSenseDevice pad(api);
        void* link = handle(0x8005);
        api->devices.insert(link,
                            FakeRawInputApi::hidDevice(kSonyVid, 0x0ECC, kUsageVendor));

        for (int i = 0; i < 500; ++i)
            pad.onRawInput(link);

        QCOMPARE(api->describeCalls, 1);
        QCOMPARE(api->payloadReads, 0);
    }

    // A query that failed is not a verdict. Caching it as "ignored" would make
    // a pad that was merely mid-enumeration invisible until the next replug.
    void transientQueryFailureIsNeverCachedAsIgnored()
    {
        auto* api = new FakeRawInputApi;
        DualSenseDevice pad(api);
        void* dualSense = handle(0x8006);
        auto device = FakeRawInputApi::hidDevice(kSonyVid, kDualSensePid, kUsageGamepad);
        device.report = dualSenseReport(false);
        device.describeFailures = 2;
        device.pathFailures = 1;
        api->devices.insert(dualSense, device);

        pad.onRawInput(dualSense);   // RIDI_DEVICEINFO fails
        pad.onRawInput(dualSense);   // fails again
        QCOMPARE(api->payloadReads, 0);

        pad.onRawInput(dualSense);   // info succeeds, RIDI_DEVICENAME fails
        QCOMPARE(api->pathCalls, 1);
        QCOMPARE(api->payloadReads, 0);

        pad.onRawInput(dualSense);   // both succeed — the pad is tracked
        QCOMPARE(api->describeCalls, 4);
        QCOMPARE(api->payloadReads, 1);
        QCOMPARE(pad.profile().family, ControlId::ControllerFamily::PlayStation);
    }

    // Windows reuses handle values. A device change on a handle must therefore
    // void whatever was cached about it, or the pad that arrives on a recycled
    // value inherits the previous device's rejection and stays dead.
    void arrivalInvalidatesTheCachedVerdict()
    {
        auto* api = new FakeRawInputApi;
        DualSenseDevice pad(api);
        void* recycled = handle(0x8007);
        api->devices.insert(recycled,
                            FakeRawInputApi::hidDevice(kGameSirVid, kGameSirPid, kUsageGamepad));

        pad.onRawInput(recycled);
        QCOMPARE(api->payloadReads, 0);

        pad.onDeviceChange(false, recycled);
        auto dualSense = FakeRawInputApi::hidDevice(kSonyVid, kDualSensePid, kUsageGamepad);
        dualSense.report = dualSenseReport(false);
        api->devices.insert(recycled, dualSense);
        pad.onDeviceChange(true, recycled);

        api->resetCounters();
        pad.onRawInput(recycled);
        QCOMPARE(api->payloadReads, 1);
        QCOMPARE(pad.profile().family, ControlId::ControllerFamily::PlayStation);
    }

    // The reconciliation pass is the safety net for the removal message that
    // never arrived: anything Windows no longer lists loses its cached verdict.
    void reconcileDropsVerdictsForHandlesWindowsNoLongerLists()
    {
        auto* api = new FakeRawInputApi;
        DualSenseDevice pad(api);
        void* recycled = handle(0x8008);
        api->devices.insert(recycled,
                            FakeRawInputApi::hidDevice(kGameSirVid, kGameSirPid, kUsageGamepad));

        pad.onRawInput(recycled);
        QCOMPARE(api->payloadReads, 0);

        api->devices[recycled].present = false;   // vanished without a message
        pad.rescan();
        QTRY_VERIFY_WITH_TIMEOUT(api->enumerations > 0, 3000);

        auto dualSense = FakeRawInputApi::hidDevice(kSonyVid, kDualSensePid, kUsageGamepad);
        dualSense.report = dualSenseReport(false);
        api->devices.insert(recycled, dualSense);

        api->resetCounters();
        pad.onRawInput(recycled);
        QCOMPARE(api->describeCalls, 1);   // re-classified from scratch
        QCOMPARE(api->payloadReads, 1);
    }

    // The diagnostics probe may look past an ignored verdict — but only for
    // gamepad-usage devices, only inside the window, and the fast path must
    // come back exactly as it was once the window closes.
    void probeReadsIgnoredGamepadPayloadsOnlyDuringWindow()
    {
        auto* api = new FakeRawInputApi;
        DualSenseDevice pad(api);
        void* gamesir = handle(0x8009);
        auto device = FakeRawInputApi::hidDevice(kGameSirVid, kGameSirPid, kUsageGamepad);
        device.report = QByteArray(8, '\0');
        api->devices.insert(gamesir, device);

        pad.onRawInput(gamesir);            // classify -> ignored
        api->resetCounters();
        InputDiagnostics::instance().clear();

        pad.onRawInput(gamesir);            // no probe: fast path only
        QCOMPARE(api->payloadReads, 0);

        InputDiagnostics::instance().startProbe(400);
        pad.beginButtonProbe();
        pad.onRawInput(gamesir);            // baseline payload for the diff
        QCOMPARE(api->describeCalls, 1);    // one eligibility query per handle
        QCOMPARE(api->payloadReads, 1);

        api->devices[gamesir].report[3] = 0x40;   // a button GameHQ knows nothing about
        pad.onRawInput(gamesir);
        QCOMPARE(api->payloadReads, 2);
        QCOMPARE(api->describeCalls, 1);    // eligibility is not re-queried
        const QString summary = InputDiagnostics::instance().probeSummary();
        QVERIFY(summary.contains(ControllerIdentity::endpointFingerprint(
            api->devices[gamesir].path.value)));
        QVERIFY(summary.contains(QStringLiteral("byte 3")));

        QTRY_VERIFY_WITH_TIMEOUT(!InputDiagnostics::instance().probeActive(), 2000);
        api->resetCounters();
        pad.onRawInput(gamesir);            // first event after expiry tears down
        pad.onRawInput(gamesir);
        QCOMPARE(api->payloadReads, 0);
        QCOMPARE(api->describeCalls, 0);
    }

    // The probe's whole job is to catch one button change the user makes by
    // hand, so 1–8 kHz must be read report for report — no sampling, no blind
    // interval, nothing to explain away. Deterministic and timestamp-driven: a
    // real 3-second wall-clock sweep would be slow and flaky.
    void probeReadsEveryReportUpToEightKilohertz()
    {
        for (const int hz : { 1000, 4000, 8000 }) {
            ProbeReadBudget budget;
            const qint64 windowMs = InputDiagnostics::kProbeDurationMs;
            const int events = int(hz * windowMs / 1000);
            int reads = 0;
            for (int i = 0; i < events; ++i) {
                if (budget.allow(qint64(i) * windowMs / events))
                    ++reads;
            }
            QVERIFY2(reads == events, QByteArray::number(hz).constData());
            QVERIFY(!budget.sampled());   // never claims to have sampled
        }
    }

    // The regression this replaces: a greedy per-slice cap spent its whole
    // allowance in the first few milliseconds of each slice and went blind for
    // the rest, so a tap in the wrong phase vanished. Walk a 20 ms press
    // through EVERY phase of a 100 ms slice, at 8 kHz and at a flooded rate
    // that forces sampling, and require a read inside the press each time.
    void probeSeesAShortPressInEveryPhaseOfASlice()
    {
        const qint64 windowMs = InputDiagnostics::kProbeDurationMs;
        const int pressMs = 20;
        for (const int hz : { 8000, 40000 }) {   // 40 kHz = several flooding devices
            for (int phase = 0; phase <= int(ProbeReadBudget::kSliceMs) - pressMs; phase += 5) {
                // Put the press in the last slice of the window: the budget is
                // most likely to be exhausted there, and the "press it right at
                // the end" case is the one users actually hit.
                const qint64 pressStart = windowMs - ProbeReadBudget::kSliceMs + phase;
                ProbeReadBudget budget;
                const int events = int(qint64(hz) * windowMs / 1000);
                int readsInsidePress = 0;
                for (int i = 0; i < events; ++i) {
                    const qint64 nowMs = qint64(i) * windowMs / events;
                    if (budget.allow(nowMs) && nowMs >= pressStart
                        && nowMs < pressStart + pressMs)
                        ++readsInsidePress;
                }
                QVERIFY2(readsInsidePress > 0,
                         QByteArray::number(hz) + " Hz, phase +"
                             + QByteArray::number(phase) + "ms");
            }
        }
    }

    // The bound that keeps a pathological flood affordable — 10 000 reads per
    // second of window, so 30 000 for the standard probe — and the honesty flag
    // that goes with it.
    void probeBudgetStaysBoundedUnderAFlood()
    {
        ProbeReadBudget budget;
        const qint64 windowMs = InputDiagnostics::kProbeDurationMs;
        const int events = int(40000 * windowMs / 1000);   // 120 000 reports
        int reads = 0;
        for (int i = 0; i < events; ++i) {
            if (budget.allow(qint64(i) * windowMs / events))
                ++reads;
        }
        const int slices = int(windowMs / ProbeReadBudget::kSliceMs);
        QVERIFY(reads <= slices * ProbeReadBudget::kReadsPerSlice);   // 30 000 for 3 s
        QCOMPARE(reads, budget.reads());
        QVERIFY(budget.sampled());   // says so instead of implying full coverage
    }

    // The sampling notice reaches the pasteable summary — the honesty half of
    // the budget. (Driving a real backend past 10 000 reports/s from a test
    // loop would be timing-dependent; the budget's own maths is pinned above.)
    void probeSummaryReportsSampling()
    {
        InputDiagnostics::instance().clear();
        InputDiagnostics::instance().startProbe(400);
        InputDiagnostics::instance().noteProbeSampled();
        QVERIFY(InputDiagnostics::instance().probeSummary().contains(
            QStringLiteral("sampled")));
        InputDiagnostics::instance().clear();
    }

    // End to end through the backend: an 8 kHz-class burst is read report for
    // report, and the summary makes no sampling excuse.
    void probeReadsAHighRateBurstInFull()
    {
        auto* api = new FakeRawInputApi;
        DualSenseDevice pad(api);
        void* gamesir = handle(0x800B);
        auto device = FakeRawInputApi::hidDevice(kGameSirVid, kGameSirPid, kUsageGamepad);
        device.report = QByteArray(8, '\0');
        api->devices.insert(gamesir, device);

        pad.onRawInput(gamesir);            // classify -> ignored
        InputDiagnostics::instance().clear();
        InputDiagnostics::instance().startProbe(400);
        pad.beginButtonProbe();
        api->resetCounters();

        // One 100 ms slice's worth of an 8 kHz pad. However the loop lands
        // across slice boundaries, no slice exceeds the full-coverage ceiling,
        // so every report must be read.
        const int burst = 800;
        for (int i = 0; i < burst; ++i)
            pad.onRawInput(gamesir);

        QCOMPARE(api->payloadReads, burst);
        QVERIFY(!InputDiagnostics::instance().probeSummary().contains(
            QStringLiteral("sampled")));
        InputDiagnostics::instance().clear();
    }

    void probeNeverReadsNonGamepadCollections()
    {
        auto* api = new FakeRawInputApi;
        DualSenseDevice pad(api);
        void* link = handle(0x800A);
        auto device = FakeRawInputApi::hidDevice(kSonyVid, 0x0ECC, kUsageVendor);
        device.report = QByteArray(8, '\1');
        api->devices.insert(link, device);

        pad.onRawInput(link);               // classify -> ignored (vendor collection)
        api->resetCounters();
        InputDiagnostics::instance().clear();
        InputDiagnostics::instance().startProbe(400);
        pad.beginButtonProbe();

        pad.onRawInput(link);
        pad.onRawInput(link);
        QCOMPARE(api->payloadReads, 0);     // its reports are never captured
        QCOMPARE(api->describeCalls, 1);    // eligibility settled once
        InputDiagnostics::instance().clear();
    }

    // t26 end-to-end: WM_INPUT payload → HID usage transition →
    // SelectiveRawHidFallback → canonical ControlId → rawHidControl edge.
    // InputEngine routes that edge through ProviderIntegration into the
    // binding runtime (covered by tst_providerintegration); this test proves
    // the production producer itself against the fake Raw Input surface.
    void boundRawHidUsageProducesPressAndReleaseEdges()
    {
        auto& fallback = ModernInput::SelectiveRawHidFallback::instance();
        auto* api = new FakeRawInputApi;
        DualSenseDevice pad(api);
        void* gamesir = handle(0x8010);
        auto device = FakeRawInputApi::hidDevice(kGameSirVid, kGameSirPid, kUsageGamepad);
        device.report = QByteArray(8, '\0');
        api->devices.insert(gamesir, device);
        pad.onRawInput(gamesir);            // classify -> ignored (not a Sony pad)

        const QString identity = ControllerIdentity::endpointFingerprint(
            api->devices[gamesir].path.value);
        const QString control = ControlId::rawHidUsage(identity, 0x09, 0x15);
        fallback.setBoundControls({control});
        QSignalSpy edges(&pad, &DualSenseDevice::rawHidControl);

        api->devices[gamesir].pressedUsages = {(quint32(0x09) << 16) | 0x15};
        pad.onRawInput(gamesir);
        QCOMPARE(edges.size(), 1);
        QCOMPARE(edges.at(0).at(0).toString(), identity);
        QCOMPARE(edges.at(0).at(1).toString(), control);
        QCOMPARE(edges.at(0).at(2).toBool(), true);

        // Held: no repeat edge while the usage stays pressed.
        pad.onRawInput(gamesir);
        QCOMPARE(edges.size(), 1);

        api->devices[gamesir].pressedUsages = {};
        pad.onRawInput(gamesir);
        QCOMPARE(edges.size(), 2);
        QCOMPARE(edges.at(1).at(2).toBool(), false);

        // An UNBOUND usage on the same device produces no edge.
        api->devices[gamesir].pressedUsages = {(quint32(0x09) << 16) | 0x33};
        pad.onRawInput(gamesir);
        api->devices[gamesir].pressedUsages = {};
        pad.onRawInput(gamesir);
        QCOMPARE(edges.size(), 2);

        fallback.setBoundControls({});      // singleton: never leak into other tests
    }

    void rawHidBatchPreservesFastPressAndRelease()
    {
        auto& fallback = ModernInput::SelectiveRawHidFallback::instance();
        auto* api = new FakeRawInputApi;
        DualSenseDevice pad(api);
        void* gamesir = handle(0x8013);
        auto device = FakeRawInputApi::hidDevice(
            kGameSirVid, kGameSirPid, kUsageGamepad,
            QStringLiteral("\\\\?\\HID#VID_3537&PID_1004#batch"));
        device.report = QByteArray(8, '\0');
        api->devices.insert(gamesir, device);
        pad.onRawInput(gamesir);

        const QString identity = ControllerIdentity::endpointFingerprint(device.path.value);
        const quint32 usage = (quint32(0x09) << 16) | 0x15;
        const QString control = ControlId::rawHidUsage(identity, 0x09, 0x15);
        fallback.setBoundControls({control});
        QSignalSpy edges(&pad, &DualSenseDevice::rawHidControl);

        api->devices[gamesir].usageReports = {{}, {usage}, {}};
        pad.onRawInput(gamesir);
        QCOMPARE(edges.size(), 2);
        QCOMPARE(edges.at(0).at(1).toString(), control);
        QCOMPARE(edges.at(0).at(2).toBool(), true);
        QCOMPARE(edges.at(1).at(1).toString(), control);
        QCOMPARE(edges.at(1).at(2).toBool(), false);
        fallback.setBoundControls({});
    }

    // 0.7.3 hardening regression: the FIRST rawHidControl edge of a batch
    // synchronously tears the device down (the shape a binding reload,
    // provider detach or removal would take). Before the fix,
    // routeRawHidUsageReport held a reference into m_rawHidPressed across the
    // emit — dropRawHidState erased that entry and the loop kept using a
    // dangling reference. Now delivery must stop after the teardown, held
    // usages must still release, and no late press edges may leak out.
    void reentrantTeardownDuringRawHidDeliveryIsSafe()
    {
        auto& fallback = ModernInput::SelectiveRawHidFallback::instance();
        auto* api = new FakeRawInputApi;
        DualSenseDevice pad(api);
        void* gamesir = handle(0x8016);
        auto device = FakeRawInputApi::hidDevice(
            kGameSirVid, kGameSirPid, kUsageGamepad,
            QStringLiteral("\\\\?\\HID#VID_3537&PID_1004#reentrant"));
        device.report = QByteArray(8, '\0');
        api->devices.insert(gamesir, device);
        pad.onRawInput(gamesir);            // classify -> ignored (not a Sony pad)

        const QString identity = ControllerIdentity::endpointFingerprint(device.path.value);
        const quint32 usageA = (quint32(0x09) << 16) | 0x15;
        const quint32 usageB = (quint32(0x09) << 16) | 0x16;
        const QString controlA = ControlId::rawHidUsage(identity, 0x09, 0x15);
        const QString controlB = ControlId::rawHidUsage(identity, 0x09, 0x16);
        fallback.setBoundControls({controlA, controlB});

        QSignalSpy edges(&pad, &DualSenseDevice::rawHidControl);
        bool tornDown = false;
        connect(&pad, &DualSenseDevice::rawHidControl, &pad,
                [&tornDown, &pad, gamesir](const QString&, const QString&, bool) {
                    if (tornDown)
                        return;   // the teardown itself re-emits (synthesized releases)
                    tornDown = true;
                    pad.onDeviceChange(false, gamesir);
                });

        // Two transitions pending in one report; the first delivery kills the device.
        api->devices[gamesir].pressedUsages = {usageA, usageB};
        pad.onRawInput(gamesir);

        QVERIFY(tornDown);
        QCOMPARE(edges.size(), 3);
        QCOMPARE(edges.at(0).at(1).toString(), controlA);
        QCOMPARE(edges.at(0).at(2).toBool(), true);
        // dropRawHidState synthesized releases for everything held (A and the
        // committed-but-undelivered B); the pending PRESS for B never fired.
        QVERIFY(!edges.at(1).at(2).toBool());
        QVERIFY(!edges.at(2).at(2).toBool());
        const QSet<QString> releasedControls{ edges.at(1).at(1).toString(),
                                              edges.at(2).at(1).toString() };
        QCOMPARE(releasedControls, (QSet<QString>{ controlA, controlB }));

        // The handle reclassifies from scratch with no ghost pressed state.
        edges.clear();
        api->devices[gamesir].pressedUsages = {};
        pad.onRawInput(gamesir);
        QCOMPARE(edges.size(), 0);
        fallback.setBoundControls({});      // singleton: never leak into other tests
    }

    void identicalRawHidModelsUseDistinctEndpointNamespaces()
    {
        auto& fallback = ModernInput::SelectiveRawHidFallback::instance();
        auto* api = new FakeRawInputApi;
        DualSenseDevice pad(api);
        void* first = handle(0x8014);
        void* second = handle(0x8015);
        auto firstDevice = FakeRawInputApi::hidDevice(
            kGameSirVid, kGameSirPid, kUsageGamepad,
            QStringLiteral("\\\\?\\HID#VID_3537&PID_1004#endpoint-a"));
        auto secondDevice = FakeRawInputApi::hidDevice(
            kGameSirVid, kGameSirPid, kUsageGamepad,
            QStringLiteral("\\\\?\\HID#VID_3537&PID_1004#endpoint-b"));
        firstDevice.report = secondDevice.report = QByteArray(8, '\0');
        api->devices.insert(first, firstDevice);
        api->devices.insert(second, secondDevice);
        pad.onRawInput(first);
        pad.onRawInput(second);

        const QString firstIdentity = ControllerIdentity::endpointFingerprint(
            firstDevice.path.value);
        const QString secondIdentity = ControllerIdentity::endpointFingerprint(
            secondDevice.path.value);
        QVERIFY(firstIdentity != secondIdentity);
        const quint32 usage = (quint32(0x09) << 16) | 0x15;
        const QString firstControl = ControlId::rawHidUsage(firstIdentity, 0x09, 0x15);
        const QString secondControl = ControlId::rawHidUsage(secondIdentity, 0x09, 0x15);
        QVERIFY(firstControl != secondControl);
        fallback.setBoundControls({firstControl, secondControl});
        QSignalSpy edges(&pad, &DualSenseDevice::rawHidControl);

        api->devices[first].pressedUsages = {usage};
        api->devices[second].pressedUsages = {usage};
        pad.onRawInput(first);
        pad.onRawInput(second);
        QCOMPARE(edges.size(), 2);
        QCOMPARE(edges.at(0).at(0).toString(), firstIdentity);
        QCOMPARE(edges.at(1).at(0).toString(), secondIdentity);
        fallback.setBoundControls({});
    }

    void activeBoundRawHidPathIsCachedAndBounded_data()
    {
        QTest::addColumn<int>("events");
        QTest::newRow("1000 Hz active") << 1000;
        QTest::newRow("4000 Hz active") << 4000;
        QTest::newRow("8000 Hz active") << 8000;
    }

    void activeBoundRawHidPathIsCachedAndBounded()
    {
        QFETCH(int, events);
        auto& fallback = ModernInput::SelectiveRawHidFallback::instance();
        auto* api = new FakeRawInputApi;
        DualSenseDevice pad(api);
        void* gamesir = handle(0x8016);
        auto device = FakeRawInputApi::hidDevice(
            kGameSirVid, kGameSirPid, kUsageGamepad,
            QStringLiteral("\\\\?\\HID#VID_3537&PID_1004#benchmark"));
        device.report = QByteArray(8, '\0');
        api->devices.insert(gamesir, device);
        pad.onRawInput(gamesir);
        const QString identity = ControllerIdentity::endpointFingerprint(device.path.value);
        fallback.setBoundControls({ControlId::rawHidUsage(identity, 0x09, 0x15)});

        // Warm the eligibility, descriptor and reusable parser buffers once.
        pad.onRawInput(gamesir);
        QCOMPARE(api->parserBuilds, 1);
        api->resetCounters();
        api->usageParses = 0;
        g_allocations.store(0, std::memory_order_relaxed);
        g_countAllocations.store(true, std::memory_order_relaxed);
        QElapsedTimer timer;
        timer.start();
        for (int i = 0; i < events; ++i)
            pad.onRawInput(gamesir);
        const qint64 elapsedNs = timer.nsecsElapsed();
        g_countAllocations.store(false, std::memory_order_relaxed);

        QCOMPARE(api->headerReads, events);
        QCOMPARE(api->payloadReads, events);
        QCOMPARE(api->usageParses, events);
        QCOMPARE(api->parserBuilds, 1);
        QCOMPARE(api->describeCalls, 0);
        QCOMPARE(api->pathCalls, 0);
        QCOMPARE(g_allocations.load(std::memory_order_relaxed), 0ll);
        qInfo("%s: %d active reports in %.2f ms (%.0f ns/event)",
              QTest::currentDataTag(), events, elapsedNs / 1e6,
              double(elapsedNs) / events);
        QVERIFY(elapsedNs / events < 100000); // comfortably below an 8 kHz interval
        fallback.setBoundControls({});
    }

    void unboundDevicesNeverPayForTheRawHidFallback()
    {
        auto& fallback = ModernInput::SelectiveRawHidFallback::instance();
        auto* api = new FakeRawInputApi;
        DualSenseDevice pad(api);
        void* gamesir = handle(0x8011);
        auto device = FakeRawInputApi::hidDevice(kGameSirVid, kGameSirPid, kUsageGamepad);
        device.report = QByteArray(8, '\0');
        api->devices.insert(gamesir, device);
        pad.onRawInput(gamesir);            // classify -> ignored

        // A binding for a DIFFERENT device identity: this device settles its
        // ineligibility once and returns to the one-lookup fast path.
        fallback.setBoundControls(
            {ControlId::rawHidUsage(QStringLiteral("8bdo:3106"), 0x09, 0x01)});
        api->resetCounters();
        for (int i = 0; i < 1000; ++i)
            pad.onRawInput(gamesir);
        QCOMPARE(api->payloadReads, 0);
        QCOMPARE(api->usageParses, 0);
        QCOMPARE(api->describeCalls, 0);    // identity mismatch settled without an OS query

        fallback.setBoundControls({});
    }

    void rawHidStateSurvivesBindingEditsAndSynthesizesReleaseOnRemoval()
    {
        auto& fallback = ModernInput::SelectiveRawHidFallback::instance();
        auto* api = new FakeRawInputApi;
        DualSenseDevice pad(api);
        void* gamesir = handle(0x8012);
        auto device = FakeRawInputApi::hidDevice(kGameSirVid, kGameSirPid, kUsageGamepad);
        device.report = QByteArray(8, '\0');
        api->devices.insert(gamesir, device);
        pad.onRawInput(gamesir);

        const QString identity = ControllerIdentity::endpointFingerprint(
            api->devices[gamesir].path.value);
        const QString control = ControlId::rawHidUsage(identity, 0x09, 0x15);
        fallback.setBoundControls({control});
        QSignalSpy edges(&pad, &DualSenseDevice::rawHidControl);
        QSignalSpy removed(&pad, &DualSenseDevice::rawHidDeviceRemoved);

        api->devices[gamesir].pressedUsages = {(quint32(0x09) << 16) | 0x15};
        pad.onRawInput(gamesir);
        QCOMPARE(edges.size(), 1);

        // Device disappears while the button is held: the fallback must
        // synthesize the release instead of leaving a stuck control.
        api->devices[gamesir].present = false;
        pad.onDeviceChange(false, gamesir);
        QTRY_COMPARE(edges.size(), 2);
        QCOMPARE(edges.at(1).at(2).toBool(), false);
        QTRY_COMPARE(removed.size(), 1);
        QCOMPARE(removed.at(0).at(0).toString(), identity);
        QCOMPARE(api->parserBuilds, 1);
        QCOMPARE(api->parserInvalidations, 1);

        fallback.setBoundControls({});
    }
};

QTEST_GUILESS_MAIN(RawInputFloodTest)
#include "tst_rawinputflood.moc"
