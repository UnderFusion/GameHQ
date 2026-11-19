#include "input/ProviderIntegration.h"

#include "input/ControlId.h"

namespace ModernInput {

namespace {
bool parseFingerprint(const QString& fingerprint, quint16& vendorId, quint16& productId)
{
    const int colon = fingerprint.indexOf(QLatin1Char(':'));
    if (colon <= 0)
        return false;
    bool vendorOk = false;
    bool productOk = false;
    const uint vendor = fingerprint.left(colon).toUInt(&vendorOk, 16);
    const uint product = fingerprint.mid(colon + 1).toUInt(&productOk, 16);
    if (!vendorOk || !productOk)
        return false;
    vendorId = quint16(vendor);
    productId = quint16(product);
    return true;
}

QSet<QString> standardControls()
{
    return {
        ControlId::FaceSouth, ControlId::FaceEast, ControlId::FaceNorth,
        ControlId::FaceWest, ControlId::ShoulderLeft, ControlId::ShoulderRight,
        ControlId::TriggerLeft, ControlId::TriggerRight, ControlId::ThumbLeft,
        ControlId::ThumbRight, ControlId::DpadUp, ControlId::DpadDown,
        ControlId::DpadLeft, ControlId::DpadRight, ControlId::Menu,
        ControlId::ViewBack,
    };
}
} // namespace

QString ProviderIntegration::legacyKey(ControllerProvider provider,
                                       const QString& providerDeviceId)
{
    return QStringLiteral("%1:%2").arg(int(provider)).arg(providerDeviceId);
}

QString ProviderIntegration::observeLegacy(ControllerProvider provider,
                                           const QString& providerDeviceId,
                                           const QString& fingerprint,
                                           const QString& displayName,
                                           ControllerCapabilities capabilities,
                                           QStringList* safeReleases,
                                           const QString& endpointId,
                                           const QString& containerId,
                                           const QString& deviceRoot)
{
    if (safeReleases)
        safeReleases->clear();
    if (providerDeviceId.isEmpty())
        return {};
    ProviderObservation observation;
    observation.provider = provider;
    observation.providerDeviceId = providerDeviceId;
    observation.displayName = displayName;
    observation.capabilities = capabilities;
    if (capabilities.testFlag(ControllerCapability::StandardControls))
        observation.controls.unite(standardControls());
    if (capabilities.testFlag(ControllerCapability::SystemShare))
        observation.controls.insert(ControlId::Capture);
    if (capabilities.testFlag(ControllerCapability::Guide))
        observation.controls.insert(ControlId::Guide);
    // VID/PID remains a weak model fingerprint only. Cross-provider merging
    // requires the explicit endpoint, container, or device-root evidence
    // supplied by the backend.
    observation.modelFingerprint = fingerprint.toLower();
    observation.endpointId = endpointId;
    observation.containerId = containerId;
    observation.topologyRoot = deviceRoot;
    parseFingerprint(fingerprint, observation.vendorId, observation.productId);
    QString rekeyedFrom;
    const QString logicalId = m_registry.observe(observation, &rekeyedFrom);
    if (!rekeyedFrom.isEmpty()) {
        const QStringList releases = m_capabilityRouter.resetLogicalController(rekeyedFrom);
        m_capabilityRouter.resetLogicalController(logicalId);
        if (safeReleases)
            *safeReleases = releases;
    }
    m_legacyKeys.insert(legacyKey(provider, providerDeviceId));
    return logicalId;
}

QStringList ProviderIntegration::removeLegacy(ControllerProvider provider,
                                              const QString& providerDeviceId)
{
    if (!m_legacyKeys.remove(legacyKey(provider, providerDeviceId)))
        return {};
    const QString logicalId = m_registry.logicalIdFor(provider, providerDeviceId);
    const QStringList releases = m_capabilityRouter.resetLogicalController(logicalId);
    m_registry.removeProvider(provider, providerDeviceId);
    return releases;
}

bool ProviderIntegration::hasLegacyAttachment(const QString& logicalId) const
{
    const auto* logical = m_registry.controller(logicalId);
    if (!logical)
        return false;
    return logical->hasProvider(ControllerProvider::SonyRaw)
        || logical->hasProvider(ControllerProvider::XInput)
        || logical->hasProvider(ControllerProvider::WinMM);
}

ControllerCapability ProviderIntegration::systemCapabilityFor(const QString& controlId)
{
    return controlId == ControlId::Capture ? ControllerCapability::SystemShare
                                           : ControllerCapability::Guide;
}

CapabilityRouteResult ProviderIntegration::routeLegacySystemEdge(
    ControllerProvider provider, const QString& providerDeviceId,
    const QString& controlId, bool pressed, quint64 timestamp)
{
    const QString logicalId = m_registry.logicalIdFor(provider, providerDeviceId);
    if (logicalId.isEmpty()) {
        // Never observed (edge raced the connection bookkeeping): fail open.
        // Losing a proven legacy press to dedup bookkeeping would be worse
        // than the double-fire this path exists to prevent.
        CapabilityRouteResult result;
        result.accepted = true;
        return result;
    }
    return m_capabilityRouter.route({logicalId, provider, systemCapabilityFor(controlId),
                                     controlId, pressed, timestamp});
}

bool ProviderIntegration::allowsLegacyViewFallback(
    ControllerProvider provider, const QString& providerDeviceId,
    bool hasExplicitViewBinding, bool fallbackEnabled) const
{
    if (!fallbackEnabled || provider != ControllerProvider::XInput || hasExplicitViewBinding
        || !providerDeviceId.startsWith(QLatin1String("xinput.slot")))
        return false;
    const QString logicalId = m_registry.logicalIdFor(provider, providerDeviceId);
    const auto* logical = m_registry.controller(logicalId);
    return logical && !logical->capabilities().testFlag(ControllerCapability::SystemShare);
}

CapabilityRouteResult ProviderIntegration::routeRawHidEdge(const QString& deviceIdentity,
                                                           const QString& controlId,
                                                           bool pressed, quint64 timestamp)
{
    CapabilityRouteResult resetResult;
    if (!m_rawHidObserved.contains(deviceIdentity)) {
        ProviderObservation observation;
        observation.provider = ControllerProvider::RawHid;
        observation.providerDeviceId = deviceIdentity;
        observation.endpointId = deviceIdentity.toLower();
        observation.displayName = QStringLiteral("Raw HID controller");
        observation.capabilities = ControllerCapability::ExtraControls;
        observation.controls.insert(controlId);
        parseFingerprint(deviceIdentity, observation.vendorId, observation.productId);
        QString rekeyedFrom;
        const QString logicalId = m_registry.observe(observation, &rekeyedFrom);
        if (!rekeyedFrom.isEmpty()) {
            resetResult.safeReleases =
                m_capabilityRouter.resetLogicalController(rekeyedFrom);
            m_capabilityRouter.resetLogicalController(logicalId);
        }
        m_rawHidObserved.insert(deviceIdentity);
    }
    m_registry.addProviderControl(ControllerProvider::RawHid,
                                  deviceIdentity, controlId);
    const QString logicalId = m_registry.logicalIdFor(ControllerProvider::RawHid,
                                                      deviceIdentity);
    CapabilityRouteResult routed = m_capabilityRouter.route(
        {logicalId, ControllerProvider::RawHid, ControllerCapability::ExtraControls,
         controlId, pressed, timestamp});
    routed.safeReleases = resetResult.safeReleases + routed.safeReleases;
    return routed;
}

QStringList ProviderIntegration::removeRawHid(const QString& deviceIdentity)
{
    if (!m_rawHidObserved.remove(deviceIdentity))
        return {};
    const QString logicalId = m_registry.logicalIdFor(ControllerProvider::RawHid,
                                                      deviceIdentity);
    const QStringList releases = m_capabilityRouter.resetLogicalController(logicalId);
    m_registry.removeProvider(ControllerProvider::RawHid, deviceIdentity);
    return releases;
}

} // namespace ModernInput
