#pragma once

#include "input/CapabilityEventRouter.h"
#include "input/PhysicalControllerRegistry.h"

#include <QHash>
#include <QSet>
#include <QString>

namespace ModernInput {

// One registry + one capability router shared by every real input provider
// (Sony Raw Input, GameInput, XInput, WinMM, selective Raw HID). This is the
// t25 integration seam: providers report physical identity and lifecycle
// here, and every system-capable edge (Capture/Guide) and raw-HID edge is
// routed through the shared CapabilityEventRouter so one physical button can
// never fire twice through two provider pipelines.
//
// Correlation model: APP_LOCAL_DEVICE_ID, PnP container/root, or endpoint
// evidence may merge providers. VID/PID remains a weak model hint and never
// merges two attachments by itself, so identical pads stay independent.
//
// Threading: main (Qt GUI) thread only, like the registry and router it owns.
// GameInput callbacks never reach this object directly — they cross the
// mutex-protected GameInputEventQueue first.
class ProviderIntegration
{
public:
    ProviderIntegration() : m_capabilityRouter(&m_registry) {}

    PhysicalControllerRegistry& registry() { return m_registry; }
    const PhysicalControllerRegistry& registry() const { return m_registry; }
    CapabilityEventRouter& capabilityRouter() { return m_capabilityRouter; }

    // A legacy backend became active. `fingerprint` is a weak model hint;
    // endpoint/container/root carry the physical correlation evidence.
    QString observeLegacy(ControllerProvider provider, const QString& providerDeviceId,
                          const QString& fingerprint, const QString& displayName,
                          ControllerCapabilities capabilities,
                          QStringList* safeReleases = nullptr,
                          const QString& endpointId = {},
                          const QString& containerId = {},
                          const QString& deviceRoot = {});
    QStringList removeLegacy(ControllerProvider provider,
                             const QString& providerDeviceId);

    // True while ANY legacy provider attachment (Sony Raw, XInput, WinMM) is
    // live. GameInput routing uses this as its safety gate: an edge whose
    // logical controller has no correlated legacy attachment stays shadowed
    // while this is true, because an uncorrelated legacy pad could mirror the
    // same physical control through the legacy dispatch path.
    bool legacyProviderConnected() const { return !m_legacyKeys.isEmpty(); }
    bool hasLegacyAttachment(const QString& logicalId) const;

    // Route a Capture/Guide edge from a legacy provider through the shared
    // dedup. Fails OPEN (accepted, no dedup) if the attachment was never
    // observed: the proven legacy path must never lose input to bookkeeping.
    CapabilityRouteResult routeLegacySystemEdge(ControllerProvider provider,
                                                const QString& providerDeviceId,
                                                const QString& controlId, bool pressed,
                                                quint64 timestamp);
    bool allowsLegacyViewFallback(ControllerProvider provider,
                                  const QString& providerDeviceId,
                                  bool hasExplicitViewBinding,
                                  bool fallbackEnabled = true) const;

    // Route a selective-Raw-HID edge. The identity is a stable anonymized
    // endpoint, allowing strong correlation without exposing a device path.
    CapabilityRouteResult routeRawHidEdge(const QString& deviceIdentity,
                                          const QString& controlId, bool pressed,
                                          quint64 timestamp);
    QStringList removeRawHid(const QString& deviceIdentity);

    static ControllerCapability systemCapabilityFor(const QString& controlId);

private:
    static QString legacyKey(ControllerProvider provider, const QString& providerDeviceId);

    PhysicalControllerRegistry m_registry;
    CapabilityEventRouter m_capabilityRouter;
    QSet<QString> m_legacyKeys;
    QSet<QString> m_rawHidObserved;
};

} // namespace ModernInput
