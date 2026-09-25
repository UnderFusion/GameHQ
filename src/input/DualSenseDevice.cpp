#include "input/DualSenseDevice.h"

#include "input/ControllerArbitration.h"
#include "input/ControllerIdentity.h"
#include "input/InputDiagnostics.h"
#include "input/SelectiveRawHidFallback.h"
#include "input/SonyReportLayout.h"
#include "input/StickNav.h"

#include <QDebug>
#include <QTimer>
#include <QVarLengthArray>

#include <windows.h>

namespace
{
constexpr USHORT kUsagePageGeneric = 0x01;
constexpr USHORT kUsageJoystick    = 0x04;
constexpr USHORT kUsageGamepad     = 0x05;
constexpr USHORT kUsageMultiAxis   = 0x08;
constexpr quint32 kSonyVid         = 0x054C;
constexpr quint32 kDualSensePid    = 0x0CE6;
constexpr quint32 kDualSenseEdgePid = 0x0DF2;
constexpr quint32 kVirtualDualSensePid = 0x0ECC;   // DSX/ViGEm virtual DualSense
constexpr quint32 kDualShock4V1Pid = 0x05C4;
constexpr quint32 kDualShock4V2Pid = 0x09CC;
constexpr quint32 kVirtualDs4VidA = 0x11FF;
constexpr quint32 kVirtualDs4PidA = 0x0847;
constexpr quint32 kVirtualDs4VidB = 0x3670;
constexpr quint32 kVirtualDs4PidB = 0x0902;
const wchar_t* kWndClassName       = L"GameHQRawInputWindow";

// A pad streaming reports sends one every few milliseconds even when idle.
// If the active pad has been silent this long while ANOTHER tracked pad
// shows a real button/stick change, the active role fails over to it.
constexpr qint64 kActiveSilenceMs = 1000;
// A removed/silent pad is only declared disconnected after this debounce —
// USB/Bluetooth re-enumeration makes pads blink out for a moment.
constexpr int kDisconnectDebounceMs = 1500;
// Arrival/removal bursts (Windows re-enumerates the whole HID tree at once)
// collapse into one reconciliation pass and one topology hint.
constexpr int kTopologyDebounceMs = 400;
// How often the aggregated per-device event-rate counters are turned into (at
// most) one log line each. Never log per event: an 8 kHz pad would write 8000
// lines a second and the diagnostic would become the outage.
constexpr int kRateSampleMs = 5000;

enum ReportLayout {
    LayoutUnknown = 0,
    LayoutDualSense,
    LayoutDs4
};

DualSenseDevice* deviceFor(HWND hwnd)
{
    return reinterpret_cast<DualSenseDevice*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

int supportedReportLayout(quint32 vendorId, quint32 productId)
{
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

bool isGamepadUsage(quint16 usagePage, quint16 usage)
{
    return usagePage == kUsagePageGeneric
        && (usage == kUsageGamepad
            || usage == kUsageJoystick
            || usage == kUsageMultiAxis);
}

const char* padName(int layout)
{
    return layout == LayoutDs4 ? "DS4-compatible" : "DualSense";
}

int devicePriority(quint32 vendorId, quint32 productId)
{
    if (vendorId == kSonyVid) {
        if (productId == kVirtualDualSensePid)
            return 2;
        return 3;
    }
    return 1;
}

QString deviceIdentity(quint32 vendorId, quint32 productId)
{
    return QString::asprintf("%04x:%04x", vendorId, productId);
}

LRESULT CALLBACK rawInputWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_INPUT:
        if (auto* dev = deviceFor(hwnd))
            dev->onRawInput(reinterpret_cast<void*>(lParam));
        // Documented WM_INPUT contract: a foreground event (RIM_INPUT) must be
        // passed to DefWindowProc so the system can perform its cleanup pass,
        // while a sink event (RIM_INPUTSINK) returns 0. Returning 0 for both —
        // as this did — leaks the OS-side buffer for every foreground event.
        if (GET_RAWINPUT_CODE_WPARAM(wParam) == RIM_INPUT)
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        return 0;
    case WM_INPUT_DEVICE_CHANGE:
        if (auto* dev = deviceFor(hwnd))
            dev->onDeviceChange(wParam == GIDC_ARRIVAL, reinterpret_cast<void*>(lParam));
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}
} // namespace

DualSenseDevice::DualSenseDevice(QObject* parent)
    : DualSenseDevice(RawInputApi::createSystem(), parent)
{
}

DualSenseDevice::DualSenseDevice(RawInputApi* api, QObject* parent)
    : Gamepad(parent)
    , m_api(api)
    , m_disconnectTimer(new QTimer(this))
    , m_reconcileTimer(new QTimer(this))
    , m_topologyTimer(new QTimer(this))
    , m_rateTimer(new QTimer(this))
{
    m_clock.start();

    m_disconnectTimer->setSingleShot(true);
    m_disconnectTimer->setInterval(kDisconnectDebounceMs);
    connect(m_disconnectTimer, &QTimer::timeout, this, &DualSenseDevice::finishDisconnect);

    m_reconcileTimer->setSingleShot(true);
    m_reconcileTimer->setInterval(kTopologyDebounceMs);
    connect(m_reconcileTimer, &QTimer::timeout, this, &DualSenseDevice::reconcileDevices);

    m_topologyTimer->setSingleShot(true);
    m_topologyTimer->setInterval(kTopologyDebounceMs);
    connect(m_topologyTimer, &QTimer::timeout, this, &DualSenseDevice::deviceTopologyChanged);

    // Runs only while devices are actually reporting; the sample that finds
    // everything quiet stops it again.
    m_rateTimer->setInterval(kRateSampleMs);
    connect(m_rateTimer, &QTimer::timeout, this, &DualSenseDevice::logInputRates);
}

DualSenseDevice::~DualSenseDevice()
{
    // Unregister so Windows stops routing WM_INPUT to a dead window.
    registerRawInput(true);

    if (m_hwnd)
        DestroyWindow(static_cast<HWND>(m_hwnd));
}

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
    // RIDEV_DEVNOTIFY also delivers arrival messages for already-connected
    // devices, but do one synchronous scan so startup logs the initial set.
    reconcileDevices();
    return true;
}

// Query a Raw Input handle and start tracking it if it is a supported
// Sony/DS4-compatible pad. Returns the tracked state, or null for anything
// else (XInput collections, unsupported pads, non-HID handles). Safe to call
// repeatedly with the same handle — arrival bursts and per-report lookups
// hit the "already tracked" / "already ignored" fast paths, and a definitive
// rejection is remembered so a flooding device is classified exactly once.
DualSenseDevice::DeviceState* DualSenseDevice::probeDevice(void* handle)
{
    if (!handle)
        return nullptr;

    auto it = m_devices.find(handle);
    if (it != m_devices.end())
        return &it.value();
    if (m_ignoredHandles.contains(handle))
        return nullptr;

    const RawInputApi::DeviceInfo info = m_api->describeDevice(handle);
    // A failed query is not a verdict — the device may simply have been
    // unplugged mid-call. Caching it as ignored would blind the backend to a
    // pad that is about to come back, so leave the handle unclassified.
    if (!info.queried)
        return nullptr;
    if (!info.isHid)
        return ignoreDevice(handle, QStringLiteral("non-HID"), {});

    const int layout = supportedReportLayout(info.vendorId, info.productId);
    const bool gamepadUsage = isGamepadUsage(info.usagePage, info.usage);
    const QString id = deviceIdentity(info.vendorId, info.productId);

    // A supported VID/PID alone is NOT enough: the same hardware IDs appear
    // on non-input collections (the PlayStation Link adapter exposes
    // 054C:0ECC vendor-defined pages). Tracking those produced phantom
    // "DualSense" entries that never send a report — require a real
    // Joystick/Gamepad/MultiAxis collection.
    if (!gamepadUsage) {
        // Only hardware that at least looks like a pad earns a log line;
        // everything else is silently remembered as somebody else's device.
        const QString reason = layout == LayoutUnknown
            ? QString() : QStringLiteral("non-gamepad collection on supported hardware");
        return ignoreDevice(handle, id, reason);
    }

    // Every gamepad-class collection needs its endpoint path before it is
    // cached. The selective Raw HID namespace is derived from this anonymized
    // endpoint, never from VID/PID, so two identical pads remain distinct.
    // Xbox-type pads expose
    // HID collections whose interface path contains "IG_"; they never send
    // usable WM_INPUT reports and are handled by the XInput backend — skip
    // them here so one pad can't drive both backends.
    const RawInputApi::DevicePath path = m_api->devicePath(handle);
    if (!path.queried)
        return nullptr;   // transient again: re-query on the next report
    const QString endpoint = ControllerIdentity::endpointFingerprint(path.value);
    if (endpoint.isEmpty())
        return nullptr;
    if (layout == LayoutUnknown)
        return ignoreDevice(handle, endpoint, QStringLiteral("unsupported report layout"));
    if (path.value.contains(QLatin1String("IG_"), Qt::CaseInsensitive)) {
        // Remember its identity: XInput only knows slot numbers, so this is
        // the one place the real VID/PID of an XInput pad is visible.
        m_xinputClass.insert(handle, id);
        m_xinputEndpoints.insert(handle, endpoint);
        return ignoreDevice(handle, endpoint,
                            QStringLiteral("XInput device — XInput backend handles it"));
    }

    DeviceState st;
    st.layout    = layout;
    st.vendorId  = info.vendorId;
    st.productId = info.productId;
    st.path      = path.value;
    auto inserted = m_devices.insert(handle, st);
    qInfo() << "Gamepad: tracking" << padName(layout)
            << "VID" << Qt::hex << st.vendorId << "PID" << st.productId
            << Qt::dec << "(" << m_devices.size() << "Sony/DS4 device(s) known)";
    InputDiagnostics::instance().noteDevice(
        id, InputDiagnostics::redactDevicePath(path.value),
        QStringLiteral("tracked (%1)").arg(QString::fromLatin1(padName(layout))));
    return &inserted.value();
}

// Remember a definitive "not our device" verdict for this handle. `reason`
// empty = not worth a log line. The log is deduplicated by device identity,
// not by handle: one pad exposes several collections and reconnects change
// every handle, so keying the log on handles would make it repeat forever.
DualSenseDevice::DeviceState* DualSenseDevice::ignoreDevice(void* handle, const QString& id,
                                                           const QString& reason)
{
    m_ignoredHandles.insert(handle, id);
    if (!reason.isEmpty() && !m_loggedIgnored.contains(id)) {
        m_loggedIgnored.insert(id);
        qInfo().noquote() << "Gamepad: ignoring HID device" << id
                          << QStringLiteral("(%1)").arg(reason);
    }
    InputDiagnostics::instance().noteDevice(
        id, {}, reason.isEmpty() ? QStringLiteral("ignored")
                                 : QStringLiteral("ignored (%1)").arg(reason));
    return nullptr;
}

// Windows reuses handle VALUES once a device is gone, so a cached verdict is
// only valid while the handle is live.
void DualSenseDevice::forgetClassification(void* handle)
{
    const QString rawHidIdentity = m_ignoredHandles.value(handle);
    dropRawHidState(handle);
    m_api->forgetDevice(handle);
    m_ignoredHandles.remove(handle);
    m_xinputClass.remove(handle);
    m_xinputEndpoints.remove(handle);
    m_rates.forget(handle);
    if (!rawHidIdentity.isEmpty()
        && !m_ignoredHandles.values().contains(rawHidIdentity))
        emit rawHidDeviceRemoved(rawHidIdentity);
}

// The device vanished (or its verdict is being re-evaluated): a raw-HID
// button physically held at that moment must not stay logically held, so a
// release is synthesized for every pressed usage before the state is dropped.
void DualSenseDevice::dropRawHidState(void* handle)
{
    ++m_rawHidStateEpoch;   // invalidates any delivery loop currently in flight
    m_rawHidEligible.remove(handle);
    const auto held = m_rawHidPressed.take(handle);
    if (held.isEmpty())
        return;
    const QString identity = m_ignoredHandles.value(handle);
    if (identity.isEmpty())
        return;
    auto& fallback = ModernInput::SelectiveRawHidFallback::instance();
    for (const quint32 usage : held) {
        const QString control = fallback.observeUsage(
            identity, quint16(usage >> 16), quint16(usage & 0xFFFF), false);
        if (!control.isEmpty())
            emit rawHidControl(identity, control, false);
    }
}

void DualSenseDevice::rawHidFallbackEvent(void* handle, void* hRawInputV)
{
    auto& fallback = ModernInput::SelectiveRawHidFallback::instance();
    // Idle guarantee: with nothing bound and no probe open this costs two
    // branches — no lookup, no insert, no allocation (tst_rawinputflood).
    if (!fallback.probeActive() && !fallback.hasAnyBindings())
        return;
    const int generation = fallback.generation();
    if (generation != m_rawHidGeneration) {
        // Binding set or probe state changed: eligibility verdicts are stale.
        // Pressed usages survive so a held button still releases.
        m_rawHidGeneration = generation;
        m_rawHidEligible.clear();
    }

    int eligible = m_rawHidEligible.value(handle, 0);
    if (eligible == 0) {
        const QString identity = m_ignoredHandles.value(handle);
        if (identity.isEmpty()
            || (!fallback.probeActive() && !fallback.hasBindingsFor(identity))) {
            eligible = -1;
        } else {
            const RawInputApi::DeviceInfo info = m_api->describeDevice(handle);
            if (!info.queried)
                return;   // transient failure — never cache as a verdict
            eligible = (info.isHid && isGamepadUsage(info.usagePage, info.usage)) ? 1 : -1;
        }
        m_rawHidEligible.insert(handle, eligible);
    }
    if (eligible < 0)
        return;

    RawInputApi::Payload payload;
    if (!m_api->readPayload(hRawInputV, payload))
        return;
    const QString identity = m_ignoredHandles.value(handle);
    struct VisitContext {
        DualSenseDevice* device = nullptr;
        void* handle = nullptr;
        const QString* identity = nullptr;
        quint64 stateEpoch = 0;
    } context{this, handle, &identity, m_rawHidStateEpoch};
    if (!m_api->visitButtonUsageReports(
            handle, payload, &context,
            [](void* opaque, const QList<quint32>& current) {
                auto* visit = static_cast<VisitContext*>(opaque);
                visit->device->routeRawHidUsageReport(
                    visit->handle, *visit->identity, visit->stateEpoch, current);
            }))
        return;   // descriptor unavailable/unparseable: no fallback for this device
}

// A rawHidControl handler may synchronously tear this device down (binding
// reload, provider detach, device removal → dropRawHidState), so no reference
// or iterator into m_rawHidPressed may survive an emit: edges are computed
// first, the stored state is fully committed, and only then are the edges
// delivered from a local list, re-checking the state epoch after every emit.
void DualSenseDevice::routeRawHidUsageReport(void* handle, const QString& identity,
                                             quint64 stateEpoch,
                                             const QList<quint32>& current)
{
    if (stateEpoch != m_rawHidStateEpoch)
        return;   // state dropped earlier in this same RAWINPUTHID batch

    // Preserve transitions inside one batch instead of reducing the batch to
    // its final state. Inline capacity keeps the active-path allocation
    // guarantee at 1/4/8 kHz (tst_rawinputflood).
    QSet<quint32>& previous = m_rawHidPressed[handle];
    QVarLengthArray<quint32, 8> pressed;
    QVarLengthArray<quint32, 8> released;
    for (const quint32 usage : current) {
        if (!previous.contains(usage))
            pressed.append(usage);
    }
    for (const quint32 usage : previous) {
        if (!current.contains(usage))
            released.append(usage);
    }
    if (pressed.isEmpty() && released.isEmpty())
        return;
    for (const quint32 usage : pressed)
        previous.insert(usage);
    for (const quint32 usage : released)
        previous.remove(usage);
    // `previous` must not be used past this point.

    auto& fallback = ModernInput::SelectiveRawHidFallback::instance();
    struct Edge {
        QString control;
        bool down = false;
    };
    QVarLengthArray<Edge, 8> edges;
    for (const quint32 usage : pressed) {
        const QString control = fallback.observeUsage(
            identity, quint16(usage >> 16), quint16(usage & 0xFFFF), true);
        if (!control.isEmpty())
            edges.append({control, true});
    }
    for (const quint32 usage : released) {
        const QString control = fallback.observeUsage(
            identity, quint16(usage >> 16), quint16(usage & 0xFFFF), false);
        if (!control.isEmpty())
            edges.append({control, false});
    }
    for (const Edge& edge : edges) {
        emit rawHidControl(identity, edge.control, edge.down);
        if (stateEpoch != m_rawHidStateEpoch)
            return;   // handler tore the device down; releases were synthesized
    }
}

QStringList DualSenseDevice::xinputClassIdentities() const
{
    QStringList identities;
    for (const QString& identity : m_xinputClass) {
        if (!identities.contains(identity))
            identities.append(identity);
    }
    return identities;
}

QStringList DualSenseDevice::xinputClassEndpoints() const
{
    QStringList endpoints;
    for (const QString& endpoint : m_xinputEndpoints) {
        if (!endpoint.isEmpty() && !endpoints.contains(endpoint))
            endpoints.append(endpoint);
    }
    return endpoints;
}

void DualSenseDevice::onDeviceChange(bool arrived, void* deviceHandle)
{
    // Any change to this handle voids what was cached about it — on arrival
    // too, because the value may now belong to a completely different device.
    // Tracked devices are left to removeDevice()/reconcile: dropping a live
    // pad's state on an arrival message would reset its active role.
    forgetClassification(deviceHandle);

    if (arrived)
        probeDevice(deviceHandle);
    else
        removeDevice(deviceHandle);

    // Windows re-enumerates the HID tree in bursts; collapse them into one
    // reconciliation pass + one topology hint for the fallback backends.
    m_reconcileTimer->start();
    m_topologyTimer->start();
}

void DualSenseDevice::removeDevice(void* handle)
{
    auto it = m_devices.find(handle);
    if (it == m_devices.end())
        return;

    const int layout = it->layout;
    const bool wasActive = (handle == m_activeHandle);
    m_devices.erase(it);
    m_rates.forget(handle);
    qInfo() << "Gamepad:" << padName(layout) << "removed"
            << "(" << m_devices.size() << "Sony/DS4 device(s) left)";

    if (wasActive) {
        m_activeHandle = nullptr;
        failoverOrScheduleDisconnect();
    }
}

// Full-list sync, debounced behind device-change bursts: tracks any supported
// device we somehow missed and prunes handles Windows no longer lists (a
// removal message can be lost during heavy re-enumeration, e.g. DSX
// recreating its virtual pad).
void DualSenseDevice::reconcileDevices()
{
    QSet<void*> present;
    QSet<QString> rawPathsLower;   // every HID interface Raw Input can see

    for (const RawInputApi::EnumeratedDevice& device : m_api->enumerateDevices()) {
        // Every type goes into `present`: the classification cache holds
        // non-HID handles too and must be pruned against the full list.
        present.insert(device.handle);
        if (device.type != RawInputApi::DeviceType::Hid)
            continue;
        const RawInputApi::DevicePath path = m_api->devicePath(device.handle);
        if (path.queried)
            rawPathsLower.insert(path.value.toLower());
        probeDevice(device.handle);
    }

    // Prune classifications for handles Windows no longer lists. This is the
    // safety net for handle reuse: a value that comes back later is
    // re-classified from scratch instead of inheriting the old verdict.
    QList<void*> staleIgnored;
    for (auto it = m_ignoredHandles.cbegin(); it != m_ignoredHandles.cend(); ++it) {
        if (!present.contains(it.key()))
            staleIgnored.push_back(it.key());
    }
    for (void* handle : staleIgnored)
        forgetClassification(handle);

    bool lostActive = false;
    for (auto it = m_devices.begin(); it != m_devices.end();) {
        if (present.contains(it.key())) {
            ++it;
            continue;
        }
        qInfo() << "Gamepad:" << padName(it->layout)
                << "pruned (no longer enumerated by Windows)";
        if (it.key() == m_activeHandle) {
            m_activeHandle = nullptr;
            lostActive = true;
        }
        m_rates.forget(it.key());
        it = m_devices.erase(it);
    }
    if (lostActive)
        failoverOrScheduleDisconnect();

    if (m_devices.isEmpty())
        qInfo() << "Gamepad: no Sony/DS4 Raw Input devices present";

    // Cross-check Windows PnP against what Raw Input just enumerated: a
    // supported pad present in PnP but absent here is being cloaked by a HID
    // filter driver (HidHide, installed with DSX/DS4Windows/reWASD) — the app
    // can't read it, but it CAN tell the user exactly what is wrong.
    auto cloak = m_api->scanHiddenPads(rawPathsLower);
    // Only alarm the user when the cloak explains an actual absence: with a
    // working pad tracked, hidden *additional* devices (e.g. DSX's parked
    // virtual pads) are expected and not worth a Settings warning.
    if (!m_devices.isEmpty() && !cloak.hiddenPads.isEmpty()) {
        qInfo() << "Gamepad: cloaked device(s) ignored while a pad is tracked:"
                << cloak.hiddenPads.join(QLatin1String(", "));
        cloak.hiddenPads.clear();
    }
    if (cloak.hiddenPads != m_lastHiddenPads) {
        if (!cloak.hiddenPads.isEmpty()) {
            qWarning() << "Gamepad:" << cloak.hiddenPads.join(QLatin1String(", "))
                       << "present in Windows PnP but INVISIBLE to Raw Input —"
                       << (cloak.hidHidePresent
                               ? "HidHide filter driver detected; whitelist GameHQ or disable hiding"
                               : "a HID filter driver is cloaking it");
        } else if (!m_lastHiddenPads.isEmpty()) {
            qInfo() << "Gamepad: previously hidden pad(s) now visible to Raw Input";
        }
        m_lastHiddenPads = cloak.hiddenPads;
        InputDiagnostics::instance().setCloakStatus(cloak.hiddenPads,
                                                    cloak.hidHidePresent);
        emit hiddenPadsChanged(cloak.hiddenPads, cloak.hidHidePresent);
    }
}

// Public nudge used after the HidHide whitelist fix: re-run the debounced
// full-list sync so a newly-visible pad is picked up without a replug where
// possible.
void DualSenseDevice::rescan()
{
    m_reconcileTimer->start();
}

// The active pad is gone. If another tracked pad has streamed reports
// recently, promote it right away (its held buttons become the new emitted
// state, releasing anything else). Otherwise arm the disconnect debounce and
// let a late report or reconnect cancel it.
void DualSenseDevice::failoverOrScheduleDisconnect()
{
    const qint64 now = m_clock.elapsed();
    void* best = nullptr;
    qint64 bestAge = 0;
    for (auto it = m_devices.cbegin(); it != m_devices.cend(); ++it) {
        if (!it->reported)
            continue;
        const qint64 age = now - it->lastReportMs;
        if (age <= kActiveSilenceMs * 3 && (!best || age < bestAge)) {
            best = it.key();
            bestAge = age;
        }
    }

    if (best) {
        const DeviceState& st = m_devices[best];
        m_activeHandle = best;
        qInfo() << "Gamepad: failing over to" << padName(st.layout);
        emitEdges(st.buttons);
        return;
    }

    if (m_connectedState && !m_disconnectTimer->isActive())
        m_disconnectTimer->start();
}

void DualSenseDevice::finishDisconnect()
{
    // A reconnect or failover between arm and fire wins over the debounce.
    if (m_activeHandle || !m_connectedState)
        return;

    qInfo() << "Gamepad: Sony/DS4 controller disconnected after debounce";
    emitEdges(0);
    m_connectedState = false;
    emit connected(false);
}

// The GUI thread runs this once per WM_INPUT, and a pad polling at 8000 Hz
// sends 8000 of them a second — per device. So the work is ordered by cost:
// header (fixed-size, stack, no allocation) → cached verdict (one hash lookup)
// → classification (twice per handle, ever) → payload. A device this backend
// does not drive never reaches the payload read at all.
void DualSenseDevice::onRawInput(void* hRawInputV)
{
    RawInputApi::Header header;
    if (!m_api->readHeader(hRawInputV, header) || !header.device)
        return;

    if (!m_sawInput) {
        m_sawInput = true;
        qInfo() << "Gamepad: first WM_INPUT received (a gamepad is sending reports)";
    }

    void* handle = header.device;
    if (header.type != RawInputApi::DeviceType::Hid) {
        noteEvent(handle, true);
        return;
    }
    if (m_ignoredHandles.contains(handle)) {
        noteEvent(handle, true);
        // Only the diagnostics probe and the selective Raw HID fallback ever
        // look past an ignored verdict. The probe is bounded by its window;
        // the fallback costs one generation check plus one hash lookup per
        // event unless this device carries a bound raw-HID control.
        if (m_probing)
            probeIgnoredEvent(handle, hRawInputV);
        rawHidFallbackEvent(handle, hRawInputV);
        return;
    }

    // Look up (or start tracking) this device — reports can beat the arrival
    // message, so an unknown handle is probed here too.
    DeviceState* st = probeDevice(handle);
    if (!st) {
        noteEvent(handle, true);
        return;
    }
    noteEvent(handle, false);

    RawInputApi::Payload payload;
    if (!m_api->readPayload(hRawInputV, payload))
        return;

    for (int i = 0; i < payload.reportCount; ++i)
        parseReport(handle, *st, payload.reports + i * payload.reportSize, payload.reportSize);
}

// Count the event and make sure the sampler is running. Deliberately the only
// bookkeeping on the ignored path: one hash lookup, no allocation, no log.
void DualSenseDevice::noteEvent(void* handle, bool ignored)
{
    m_rates.record(handle, ignored);
    if (!m_rateTimer->isActive()) {
        m_lastRateSampleMs = m_clock.elapsed();
        m_rateTimer->start();
    }
}

void DualSenseDevice::beginButtonProbe()
{
    m_probing = true;
    m_probeBudget.reset();
    m_probeEligible.clear();
    m_probePrev.clear();
}

// One call per probe-window event; also the single place probe state is torn
// down, so an expired window cannot leave payload reads enabled.
bool DualSenseDevice::probeWindowStillOpen()
{
    if (InputDiagnostics::instance().probeActive())
        return true;
    m_probing = false;
    m_probeEligible.clear();
    m_probePrev.clear();
    m_probeBudget.reset();
    return false;
}

void DualSenseDevice::probeIgnoredEvent(void* handle, void* hRawInput)
{
    if (!probeWindowStillOpen())
        return;

    // One eligibility query per handle per window: only devices that present a
    // Joystick/Gamepad/MultiAxis collection are worth a payload read — the
    // ignored set also contains keyboards, mice and vendor collections whose
    // reports must never be captured, even summarized.
    int eligible = m_probeEligible.value(handle, 0);
    if (eligible == 0) {
        const RawInputApi::DeviceInfo info = m_api->describeDevice(handle);
        eligible = (info.queried && info.isHid
                    && isGamepadUsage(info.usagePage, info.usage)) ? 1 : -1;
        m_probeEligible.insert(handle, eligible);
    }
    if (eligible < 0)
        return;
    // Full coverage up to 10 000 reports/s (so 4 and 8 kHz pads are read report
    // for report), evenly strided above that, hard-stopped at 30 000 reads for
    // the window. A short tap cannot land in a blind interval.
    if (!m_probeBudget.allow(m_clock.elapsed())) {
        InputDiagnostics::instance().noteProbeSampled();
        return;
    }

    RawInputApi::Payload payload;
    if (!m_api->readPayload(hRawInput, payload) || payload.reportSize <= 0)
        return;
    const QByteArray current(reinterpret_cast<const char*>(payload.reports),
                             payload.reportSize);
    QByteArray& previous = m_probePrev[handle];
    if (previous.isEmpty()) {
        previous = current;
        return;
    }
    if (current == previous)
        return;

    // Only the diff leaves the process: changed byte positions and the XOR of
    // their values, never the report itself.
    QStringList changes;
    const int n = qMin(current.size(), previous.size());
    for (int i = 0; i < n && changes.size() < 8; ++i) {
        const uchar diff = uchar(current[i]) ^ uchar(previous[i]);
        if (diff)
            changes << QStringLiteral("byte %1 ^%2")
                           .arg(i)
                           .arg(uint(diff), 2, 16, QLatin1Char('0'));
    }
    previous = current;
    if (!changes.isEmpty())
        InputDiagnostics::instance().noteProbeEvent(
            m_ignoredHandles.value(handle), QStringLiteral("Raw Input"),
            changes.join(QStringLiteral(", ")));
}

void DualSenseDevice::noteProbeButtonChange(const DeviceState& st, quint32 before,
                                            quint32 after)
{
    if (!probeWindowStillOpen())
        return;
    InputDiagnostics::instance().noteProbeEvent(
        deviceIdentity(st.vendorId, st.productId), QStringLiteral("Raw Input"),
        QStringLiteral("buttons %1 -> %2")
            .arg(before, 0, 16).arg(after, 0, 16));
}

void DualSenseDevice::logInputRates()
{
    const qint64 now = m_clock.elapsed();
    const qint64 window = qMax<qint64>(1, now - m_lastRateSampleMs);
    m_lastRateSampleMs = now;

    bool traffic = false;
    for (const InputRateMonitor::Sample& s : m_rates.sample(window)) {
        if (s.eventsPerSecond > 0)
            traffic = true;
        {
            // Keep the diagnostics registry current on every sample, not only
            // on the (rarer) loggable changes — the export should show the
            // last measured rate, not the last newsworthy one.
            const QString identity = s.ignored
                ? m_ignoredHandles.value(s.handle)
                : (m_devices.contains(s.handle)
                       ? deviceIdentity(m_devices.value(s.handle).vendorId,
                                        m_devices.value(s.handle).productId)
                       : QString());
            if (!identity.isEmpty())
                InputDiagnostics::instance().noteRate(identity, s.eventsPerSecond);
        }
        if (!s.worthLogging)
            continue;
        const QString what = deviceLabel(s.handle, s.ignored);
        if (s.eventsPerSecond == 0) {
            qInfo().noquote()
                << QStringLiteral("Gamepad: Raw Input stream from %1 stopped").arg(what);
            continue;
        }
        qInfo().noquote()
            << QStringLiteral("Gamepad: Raw Input %1 events/s from %2%3")
                   .arg(s.eventsPerSecond)
                   .arg(what, s.ignored
                            ? QStringLiteral(" (classified once, payload never read)")
                            : QString());
    }
    if (!traffic)
        m_rateTimer->stop();
}

QString DualSenseDevice::deviceLabel(void* handle, bool ignored) const
{
    if (ignored) {
        const QString id = m_ignoredHandles.value(handle);
        return id.isEmpty() ? QStringLiteral("unclassified device")
                            : QStringLiteral("ignored device %1").arg(id);
    }
    auto it = m_devices.constFind(handle);
    if (it == m_devices.cend())
        return QStringLiteral("device");
    return QStringLiteral("%1 %2").arg(QString::fromLatin1(padName(it->layout)),
                                       deviceIdentity(it->vendorId, it->productId));
}

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
                                  const unsigned char* d, int len)
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
            "Gamepad: input layout %1 report_id=0x%2 length=%3 variant=%4 axes=%5 buttons=%6")
            .arg(deviceIdentity(st.vendorId, st.productId))
            .arg(reportId, 2, 16, QLatin1Char('0'))
            .arg(len).arg(QLatin1String(layout.variant))
            .arg(layout.axes).arg(layout.buttons);
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

ControlId::DeviceProfile DualSenseDevice::profile() const
{
    if (!m_activeHandle || !m_devices.contains(m_activeHandle))
        return {};
    const DeviceState& st = m_devices.value(m_activeHandle);
    const QString model = QStringLiteral("%1:%2")
                              .arg(st.vendorId, 4, 16, QLatin1Char('0'))
                              .arg(st.productId, 4, 16, QLatin1Char('0'));
    const QString endpoint = ControllerIdentity::endpointFingerprint(st.path);
    return ControlId::DeviceProfile{
        QStringLiteral("Sony Raw Input"),
        endpoint,
        ControlId::ControllerFamily::PlayStation,
        QString::fromLatin1(padName(st.layout)),
        model,
        endpoint,
    };
}
