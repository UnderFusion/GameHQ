// DualSenseDevice: per-report source arbitration between Raw Input and the
// direct HID reader (WM_INPUT handling, hidden-interface sync, reader
// lifecycle and failover). Split out of DualSenseDevice.cpp; the class itself
// is declared in DualSenseDevice.h.
#include "input/DualSenseDevice.h"
#include "input/DualSenseDeviceInternal.h"

#include "input/ControllerIdentity.h"
#include "input/InputDiagnostics.h"
#include "input/SelectiveRawHidFallback.h"
#include "input/SonyHidReader.h"
#include "input/SonyReportLayout.h"

#include <QDebug>
#include <QVarLengthArray>

#include <utility>

#include <windows.h>

using namespace DualSenseInternal;

// The GUI thread runs this once per WM_INPUT, and a pad polling at 8000 Hz
// sends 8000 of them a second — per device. So the work is ordered by cost:
// header (fixed-size, stack, no allocation) → cached verdict (one hash lookup)
// → classification (twice per handle, ever) → payload. A device this backend
// does not drive never reaches the payload read at all.
void DualSenseDevice::onRawInput(void* hRawInputV)
{
    RawInputApi::Header header;
    if (!m_api->readHeader(hRawInputV, header) || !header.device) {
        ++m_headerFailures;
        return;
    }

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

    // Raw Input stays the preferred source: a reader opened only to cover a
    // silent stream gives the endpoint back once Raw Input is steady again.
    if (st->hidRescue && st->hidReaderId) {
        const qint64 now = m_clock.elapsed();
        if (!st->rawResumeMs || now - st->lastRawMs > kRescueRawGapMs)
            st->rawResumeMs = now;
        st->lastRawMs = now;
        if (now - st->rawResumeMs >= kRescueHandbackMs)
            endHidRescue(*st);
    }

    // Explicit per-endpoint source arbitration: while this endpoint's direct
    // HID reader is healthy it is the only source, and the Raw Input copy of
    // the same report is dropped before its payload is read, so no edge can
    // be produced twice. This picks a source; it does not de-duplicate by time.
    if (st->hidPreferred) {
        const qint64 now = m_clock.elapsed();
        if (now - st->lastHidMs <= kHidFreshMs) {
            st->rawWhileHidQuietMs = 0;
            return;
        }
        if (!st->rawWhileHidQuietMs)
            st->rawWhileHidQuietMs = now;
        if (now - st->rawWhileHidQuietMs < kHidFailoverMs)
            return;
        // This Raw Input report carries the pad's current state, so the
        // buttons still down stay down instead of a release+press pair.
        demoteHidSource(handle, *st, "reader silent while Raw Input delivers", false);
    }

    RawInputApi::Payload payload;
    if (!m_api->readPayload(hRawInputV, payload)) {
        ++m_payloadFailures;
        return;
    }

    for (int i = 0; i < payload.reportCount; ++i)
        parseReport(handle, *st, payload.reports + i * payload.reportSize, payload.reportSize);
}

bool DualSenseDevice::syncHiddenInterfaces(
    const QList<RawInputApi::HiddenInterface>& hiddenInterfaces,
    const QSet<QString>& rawPathsLower)
{
    if (!m_hidReaderFactory)
        return false;
    QHash<QString, RawInputApi::HiddenInterface> hidden;   // lower-case path -> interface
    for (const auto& i : hiddenInterfaces)
        hidden.insert(i.path.toLower(), i);

    // Prune HID-only endpoints that left PnP or became visible to Raw Input
    // (the Raw Input entry then takes the path over).
    bool lostActive = false;
    for (auto it = m_devices.begin(); it != m_devices.end();) {
        const QString key = it->path.toLower();
        if (!it->hidOnly || (hidden.contains(key) && !rawPathsLower.contains(key))) {
            ++it;
            continue;
        }
        qInfo() << "Gamepad:" << padName(it->layout) << "(direct HID) no longer hidden-only";
        if (it.key() == m_activeHandle) {
            m_activeHandle = nullptr;
            lostActive = true;
        }
        stopHidReader(it.key(), it.value(), "endpoint pruned");
        m_rates.forget(it.key());
        it = m_devices.erase(it);
    }

    for (auto h = hidden.cbegin(); h != hidden.cend(); ++h) {
        bool tracked = false;
        for (auto it = m_devices.cbegin(); it != m_devices.cend() && !tracked; ++it)
            tracked = it->path.compare(h->path, Qt::CaseInsensitive) == 0;
        const int layout = supportedReportLayout(h->vendorId, h->productId);
        if (tracked || layout == LayoutUnknown)
            continue;
        void* key = hidOnlyKey(m_nextHidOnlyKey++);
        DeviceState st;
        st.layout = layout;
        st.vendorId = h->vendorId;
        st.productId = h->productId;
        st.path = h->path;
        st.hidOnly = true;
        auto inserted = m_devices.insert(key, st);
        startHidReader(key, inserted.value());
        if (!inserted->hidReaderId) {   // not whitelisted, or not a gamepad collection
            m_devices.erase(inserted);
            continue;
        }
        qInfo() << "Gamepad: tracking" << padName(layout) << "hidden from Raw Input via direct HID"
                << "VID" << Qt::hex << h->vendorId << "PID" << h->productId << Qt::dec;
        InputDiagnostics::instance().noteDevice(
            deviceIdentity(h->vendorId, h->productId),
            InputDiagnostics::redactDevicePath(h->path),
            QStringLiteral("tracked via direct HID (hidden from Raw Input)"));
    }
    return lostActive;
}

void DualSenseDevice::setHidReaderFactory(HidReaderFactory factory, bool forVisibleEndpoints)
{
    m_hidReaderFactory = std::move(factory);
    m_hidReaderForVisible = forVisibleEndpoints;
    if (!m_hidReaderFactory)
        return;
    for (auto it = m_devices.begin(); it != m_devices.end(); ++it) {
        if (!it->hidReaderId)
            startHidReader(it.key(), it.value());
    }
}

void DualSenseDevice::onRawInputStreamSilent(void* handle)
{
    const auto it = m_devices.find(handle);
    if (it == m_devices.end() || it->hidOnly || it->hidReaderId || !m_hidReaderFactory)
        return;
    qInfo().noquote() << QStringLiteral(
        "Gamepad: Raw Input went silent for %1 while it is still present, "
        "trying the read-only direct HID reader")
        .arg(deviceIdentity(it->vendorId, it->productId));
    startHidReader(handle, it.value(), /*rescue=*/true);
}

void DualSenseDevice::startHidReader(void* handle, DeviceState& st, bool rescue)
{
    Q_UNUSED(handle)
    if (!m_hidReaderFactory || st.hidReaderId || st.path.isEmpty())
        return;
    if (!st.hidOnly && !m_hidReaderForVisible && !rescue)
        return;
    // One reader per endpoint path. Two live Raw Input handles for one path
    // only overlap briefly during re-enumeration; the second one waits.
    for (auto it = m_devices.cbegin(); it != m_devices.cend(); ++it) {
        if (it->hidReaderId && it->path.compare(st.path, Qt::CaseInsensitive) == 0)
            return;
    }
    const quint64 id = m_nextHidReaderId++;
    QString error;
    std::unique_ptr<SonyHidReaderHandle> reader =
        m_hidReaderFactory(st.path, st.layout == LayoutDs4, id, &error);
    const QString identity = deviceIdentity(st.vendorId, st.productId);
    const QString pathKey = st.path.toLower();
    if (!reader) {
        if (!m_hidOpenFailuresLogged.contains(pathKey)) {
            m_hidOpenFailuresLogged.insert(pathKey);
            qInfo().noquote() << QStringLiteral(
                "Gamepad: direct HID reader unavailable for %1 %2 (%3), Raw Input stays the source")
                .arg(QString::fromLatin1(padName(st.layout)), identity, error);
        }
        return;
    }
    m_hidOpenFailuresLogged.remove(pathKey);
    m_hidReaders.emplace(id, std::move(reader));
    st.hidReaderId = id;
    st.hidRescue = rescue && !st.hidOnly && !m_hidReaderForVisible;
    st.rawResumeMs = 0;
    st.hidPreferred = false;
    st.lastHidMs = 0;
    st.rawWhileHidQuietMs = 0;
    qInfo().noquote() << QStringLiteral("Gamepad: direct HID reader opened for %1 %2 (read-only)")
                             .arg(QString::fromLatin1(padName(st.layout)), identity);
}

void DualSenseDevice::stopHidReader(void* handle, DeviceState& st, const char* why)
{
    if (!st.hidReaderId)
        return;
    const quint64 id = st.hidReaderId;
    demoteHidSource(handle, st, why);
    st.hidReaderId = 0;
    st.hidRescue = false;
    // Destruction cancels the pending read and joins the worker; queued
    // deliveries still carrying `id` find no owner and are dropped.
    m_hidReaders.erase(id);
    qInfo().noquote() << QStringLiteral("Gamepad: direct HID reader closed for %1 (%2)")
                             .arg(deviceIdentity(st.vendorId, st.productId),
                                  QLatin1String(why));
}

void DualSenseDevice::endHidRescue(DeviceState& st)
{
    const quint64 id = st.hidReaderId;
    st.hidReaderId = 0;
    st.hidPreferred = false;
    st.hidRescue = false;
    st.rawResumeMs = 0;
    st.rawWhileHidQuietMs = 0;
    // Same DeviceState, same decoder: the next Raw Input report is diffed
    // against what the reader last delivered. Late reader deliveries carry
    // `id` and are dropped.
    m_hidReaders.erase(id);
    qInfo().noquote() << QStringLiteral(
        "Gamepad: Raw Input resumed for %1, closing the rescue HID reader")
        .arg(deviceIdentity(st.vendorId, st.productId));
}

void DualSenseDevice::demoteHidSource(void* handle, DeviceState& st, const char* why,
                                      bool releaseHeld)
{
    if (!st.hidPreferred)
        return;
    st.hidPreferred = false;
    st.rawWhileHidQuietMs = 0;
    qInfo().noquote() << QStringLiteral(
        "Gamepad: direct HID reader no longer the source for %1 (%2), Raw Input takes over")
        .arg(deviceIdentity(st.vendorId, st.productId), QLatin1String(why));
    if (!releaseHeld)
        return;   // the caller parses the new source's report right away
    // No report from the new source is at hand: release everything this
    // endpoint held; the next report presses again whatever is still down.
    st.buttons = 0;
    st.stick = 0;
    if (handle == m_activeHandle)
        emitEdges(0);
}

void DualSenseDevice::onHidReport(quint64 readerId, const QByteArray& report)
{
    if (!readerId || report.isEmpty())
        return;
    for (auto it = m_devices.begin(); it != m_devices.end(); ++it) {
        if (it->hidReaderId != readerId)
            continue;
        DeviceState& st = it.value();
        st.lastHidMs = m_clock.elapsed();
        st.rawWhileHidQuietMs = 0;
        if (!st.hidPreferred) {
            st.hidPreferred = true;
            qInfo().noquote() << QStringLiteral(
                "Gamepad: direct HID reader is now the source for %1 (Raw Input payloads skipped)")
                .arg(deviceIdentity(st.vendorId, st.productId));
        }
        parseReport(it.key(), st, reinterpret_cast<const unsigned char*>(report.constData()),
                    int(report.size()), "Sony direct HID");
        return;
    }
    // Stale delivery: the reader was replaced or its endpoint removed.
}

void DualSenseDevice::onHidReaderFailed(quint64 readerId, const QString& reason)
{
    for (auto it = m_devices.begin(); it != m_devices.end(); ++it) {
        if (it->hidReaderId != readerId)
            continue;
        qInfo().noquote() << QStringLiteral("Gamepad: direct HID reader failed for %1: %2")
                                 .arg(deviceIdentity(it->vendorId, it->productId), reason);
        if (it->hidOnly)
            removeDevice(it.key());   // no other source: the endpoint is gone
        else
            stopHidReader(it.key(), it.value(), "reader failed");
        return;
    }
}
