#include "input/SonyHidReaderPolicy.h"
#include "input/DualSenseDevice.h"
#include "input/DualSenseDeviceInternal.h"

#include "input/ControllerArbitration.h"
#include "input/ControllerIdentity.h"
#include "input/InputDiagnostics.h"
#include "input/SelectiveRawHidFallback.h"
#include "input/SonyHidReader.h"
#include "input/SonyReportLayout.h"
#include "input/StickNav.h"

#include <QDebug>
#include <QThreadPool>
#include <QTimer>
#include <QVarLengthArray>

#include <utility>

#include <windows.h>
#include <dbt.h>

using namespace DualSenseInternal;

DualSenseDevice::DualSenseDevice(QObject* parent)
    : DualSenseDevice(RawInputApi::createSystem(), parent)
{
    const auto hidPolicy = SonyHidReaderPolicy::resolve(qgetenv("GAMEHQ_SONY_HID_READER"));
    qInfo().noquote() << QStringLiteral("Sony HID reader: %1 (%2)")
        .arg(hidPolicy.enabled ? QStringLiteral("enabled") : QStringLiteral("disabled"),
             QString::fromLatin1(hidPolicy.reason));
    // An explicit "0" turns the reader off everywhere. Otherwise it is always
    // installed for pads HidHide cloaks from Raw Input: those have no other
    // source, and a whitelisted GameHQ can still open them directly, as DSX
    // does. The policy decides only whether Raw-Input-visible pads use it.
    if (qgetenv("GAMEHQ_SONY_HID_READER") == "0")
        return;
    setHidReaderFactory([this](const QString& path, bool ds4, quint64 readerId,
                               QString* error) -> std::unique_ptr<SonyHidReaderHandle> {
        SonyHidReader::Callbacks callbacks;
        // Worker thread -> GUI thread. Posted events for a destroyed device
        // are discarded by Qt; the reader itself is joined before that.
        callbacks.report = [this, readerId](const QByteArray& report) {
            QMetaObject::invokeMethod(this, [this, readerId, report] {
                onHidReport(readerId, report);
            }, Qt::QueuedConnection);
        };
        callbacks.failed = [this, readerId](const QString& reason) {
            QMetaObject::invokeMethod(this, [this, readerId, reason] {
                onHidReaderFailed(readerId, reason);
            }, Qt::QueuedConnection);
        };
        return SonyHidReader::open(path,
                                   ds4 ? SonyReportLayout::Family::Ds4
                                       : SonyReportLayout::Family::DualSense,
                                   std::move(callbacks), error);
    }, hidPolicy.enabled);
    qInfo() << (hidPolicy.enabled
                    ? "Gamepad: direct HID reader enabled (read-only, Raw Input remains the fallback)"
                    : "Gamepad: direct HID reader limited to pads hidden from Raw Input (read-only)");
}

DualSenseDevice::DualSenseDevice(RawInputApi* api, QObject* parent)
    : Gamepad(parent)
    , m_api(api)
    , m_disconnectTimer(new QTimer(this))
    , m_reconcileTimer(new QTimer(this))
    , m_topologyTimer(new QTimer(this))
    , m_rateTimer(new QTimer(this))
    , m_pnpScanPool(std::make_unique<QThreadPool>())
{
    m_clock.start();
    m_pnpScanPool->setMaxThreadCount(1);

    m_disconnectTimer->setSingleShot(true);
    m_disconnectTimer->setInterval(kDisconnectDebounceMs);
    connect(m_disconnectTimer, &QTimer::timeout, this, &DualSenseDevice::finishDisconnect);

    m_reconcileTimer->setSingleShot(true);
    m_reconcileTimer->setInterval(kTopologyDebounceMs);
    connect(m_reconcileTimer, &QTimer::timeout, this, &DualSenseDevice::reconcileDevices);

    m_topologyTimer->setSingleShot(true);
    m_topologyTimer->setInterval(kTopologyDebounceMs);
    connect(m_topologyTimer, &QTimer::timeout, this, &DualSenseDevice::deviceTopologyChanged);
    connect(m_topologyTimer, &QTimer::timeout, this, [this] { logDeliveryReceipt("topology"); });

    // Runs only while devices are actually reporting; the sample that finds
    // everything quiet stops it again.
    m_rateTimer->setInterval(kRateSampleMs);
    connect(m_rateTimer, &QTimer::timeout, this, &DualSenseDevice::logInputRates);
}

DualSenseDevice::~DualSenseDevice()
{
    // A PnP scan in flight still uses m_api; its queued result is dropped
    // with this object's posted events.
    m_pnpScanPool->waitForDone();

    // Join every reader worker before anything they could post to goes away.
    m_hidReaders.clear();

    // Unregister so Windows stops routing WM_INPUT to a dead window.
    registerRawInput(true);
    if (m_devNotify)
        UnregisterDeviceNotification(static_cast<HDEVNOTIFY>(m_devNotify));

    if (m_hwnd)
        DestroyWindow(static_cast<HWND>(m_hwnd));
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
    startHidReader(handle, inserted.value());
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
    if (!m_api->readPayload(hRawInputV, payload)) {
        ++m_payloadFailures;
        return;
    }
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

void DualSenseDevice::onPnpInterfaceChange()
{
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
    stopHidReader(handle, it.value(), "endpoint removed");
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
        if (it->hidOnly || present.contains(it.key())) {   // HID-only: syncHiddenInterfaces
            ++it;
            continue;
        }
        qInfo() << "Gamepad:" << padName(it->layout)
                << "pruned (no longer enumerated by Windows)";
        if (it.key() == m_activeHandle) {
            m_activeHandle = nullptr;
            lostActive = true;
        }
        stopHidReader(it.key(), it.value(), "endpoint pruned");
        m_rates.forget(it.key());
        it = m_devices.erase(it);
    }
    // A reader that failed to open or died is retried once per topology pass
    // (never per report), e.g. after another tool released exclusive access.
    for (auto it = m_devices.begin(); it != m_devices.end(); ++it) {
        if (!it->hidReaderId)
            startHidReader(it.key(), it.value());
    }
    if (lostActive)
        failoverOrScheduleDisconnect();

    // Cross-check Windows PnP against what Raw Input just enumerated: a
    // supported pad present in PnP but absent here is being cloaked by a HID
    // filter driver (HidHide, installed with DSX/DS4Windows/reWASD) — the app
    // can't read it, but it CAN tell the user exactly what is wrong.
    requestPnpScan(rawPathsLower);
}

void DualSenseDevice::requestPnpScan(const QSet<QString>& rawPathsLower)
{
    if (m_pnpScanRunning) {
        // The running scan's snapshot is already outdated; rescan with this
        // one as soon as it returns.
        m_pnpScanQueued = true;
        m_queuedRawPaths = rawPathsLower;
        return;
    }
    m_pnpScanRunning = true;
    RawInputApi* api = m_api.get();
    const bool wantInterfaces = bool(m_hidReaderFactory);
    m_pnpScanPool->start([this, api, rawPathsLower, wantInterfaces] {
        PnpScan scan;
        scan.rawPathsLower = rawPathsLower;
        scan.interfacesScanned = wantInterfaces;
        if (wantInterfaces)
            scan.hidden = api->hiddenPadInterfaces(rawPathsLower);
        scan.cloak = api->scanHiddenPads(rawPathsLower);
        QMetaObject::invokeMethod(this, [this, scan] { applyPnpScan(scan); },
                                  Qt::QueuedConnection);
    });
}

void DualSenseDevice::applyPnpScan(const PnpScan& scan)
{
    m_pnpScanRunning = false;
    if (m_pnpScanQueued) {
        // A newer topology pass superseded this snapshot: never apply a
        // result older than the Raw Input view it would be compared with.
        m_pnpScanQueued = false;
        requestPnpScan(std::exchange(m_queuedRawPaths, {}));
        return;
    }

    if (scan.interfacesScanned && m_hidReaderFactory
        && syncHiddenInterfaces(scan.hidden, scan.rawPathsLower))
        failoverOrScheduleDisconnect();

    if (m_devices.isEmpty())
        qInfo() << "Gamepad: no Sony/DS4 Raw Input devices present";

    auto cloak = scan.cloak;
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
            qInfo() << "Gamepad: previously hidden pad(s) now readable";
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
            if (!s.ignored) {
                logDeliveryReceipt("stream-silent");
                onRawInputStreamSilent(s.handle);
                // One follow-up shows whether WM_INPUT resumed, kept arriving
                // but failing, or stayed absent. Never more than one pending.
                if (!m_silenceFollowupPending) {
                    m_silenceFollowupPending = true;
                    QTimer::singleShot(kSilenceFollowupMs, this, [this] {
                        m_silenceFollowupPending = false;
                        logDeliveryReceipt("silence-followup");
                    });
                }
            }
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

void DualSenseDevice::logDeliveryReceipt(const char* reason)
{
    const qint64 now = m_clock.elapsed();
    const HWND own = static_cast<HWND>(m_hwnd);

    // Effective registrations of THIS process (the API only reports the
    // caller's own); a gamepad-class entry targeting another window, or one
    // missing INPUTSINK, would explain background silence.
    QStringList regs;
    UINT count = 0;
    GetRegisteredRawInputDevices(nullptr, &count, sizeof(RAWINPUTDEVICE));
    if (count > 0 && count < 64) {
        RAWINPUTDEVICE list[64]{};
        const UINT got = GetRegisteredRawInputDevices(list, &count, sizeof(RAWINPUTDEVICE));
        for (UINT i = 0; i < got && got != UINT(-1); ++i) {
            const RAWINPUTDEVICE& r = list[i];
            const char* target = !r.hwndTarget ? "focus"
                               : r.hwndTarget == own ? "own"
                               : "other";
            regs << QString::asprintf("%02x:%02x/0x%x/%s", r.usUsagePage, r.usUsage,
                                      unsigned(r.dwFlags), target);
        }
    }

    DWORD fgPid = 0;
    if (HWND fg = GetForegroundWindow())
        GetWindowThreadProcessId(fg, &fgPid);
    const bool windowOk = own && IsWindow(own);
    const bool sameThread = windowOk
        && GetWindowThreadProcessId(own, nullptr) == GetCurrentThreadId();

    qInfo().noquote() << QStringLiteral(
        "Gamepad: raw-input receipt reason=%1 window=%2 thread=%3 foreground=%4 "
        "wm_input_fg=%5 wm_input_sink=%6 header_fail=%7 payload_fail=%8 "
        "tracked=%9 span_ms=%10 registrations=[%11]")
        .arg(QLatin1String(reason))
        .arg(windowOk ? QStringLiteral("ok") : QStringLiteral("invalid"))
        .arg(sameThread ? QStringLiteral("same") : QStringLiteral("other"))
        .arg(fgPid == GetCurrentProcessId() ? QStringLiteral("self")
                                            : (fgPid ? QStringLiteral("other")
                                                     : QStringLiteral("none")))
        .arg(m_wmInputForeground).arg(m_wmInputSink)
        .arg(m_headerFailures).arg(m_payloadFailures)
        .arg(m_devices.size())
        .arg(now - m_lastReceiptMs)
        .arg(regs.join(QLatin1Char(' ')));

    m_wmInputForeground = m_wmInputSink = m_headerFailures = m_payloadFailures = 0;
    m_lastReceiptMs = now;
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
