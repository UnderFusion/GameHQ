#include "input/PhysicalControllerRegistry.h"

#include <QCryptographicHash>

#include <algorithm>

namespace ModernInput {

ControllerCapabilities LogicalController::capabilities() const
{
    ControllerCapabilities result;
    for (const auto& attachment : providers)
        result |= attachment.capabilities;
    return result;
}

bool LogicalController::hasProvider(ControllerProvider provider) const
{
    return std::any_of(providers.cbegin(), providers.cend(),
                       [provider](const auto& item) { return item.provider == provider; });
}

QString PhysicalControllerRegistry::attachmentKey(ControllerProvider provider,
                                                   const QString& providerDeviceId)
{
    return QStringLiteral("%1:%2").arg(static_cast<int>(provider)).arg(providerDeviceId);
}

QString PhysicalControllerRegistry::createLogicalId(const ProviderObservation& observation,
                                                     quint64 generation,
                                                     const QString& disambiguator)
{
    // A strong identity hashes deterministically from the identity namespace
    // alone: the same physical device must produce the same logical ID
    // regardless of provider arrival order, detection order, or session —
    // this ID is persisted in binding profiles and controller_layouts. Only
    // weak identities (nothing stable to hash) stay session-local through
    // the generation counter.
    //
    // The evidence is ranked exactly as findMatch() ranks it. If the ID were
    // derived from weaker evidence than the match, two endpoints the matcher
    // deliberately kept apart could hash to one ID and silently collapse.
    const bool strongIdentity = !observation.appLocalDeviceId.isEmpty()
        || !observation.endpointId.isEmpty() || !observation.containerId.isEmpty()
        || !observation.topologyRoot.isEmpty();
    const QString strongest = !observation.appLocalDeviceId.isEmpty()
        ? observation.appLocalDeviceId
        : (!observation.endpointId.isEmpty() ? observation.endpointId
           : (!observation.containerId.isEmpty() ? observation.containerId
              : (!observation.topologyRoot.isEmpty() ? observation.topologyRoot
                                                     : observation.providerDeviceId)));
    const QByteArray material = strongIdentity
        ? QStringLiteral("strong|%1%2").arg(strongest, disambiguator).toUtf8()
        : QStringLiteral("weak|%1|%2|%3")
              .arg(static_cast<int>(observation.provider))
              .arg(strongest).arg(generation).toUtf8();
    return QStringLiteral("controller-%1").arg(QString::fromLatin1(
        QCryptographicHash::hash(material, QCryptographicHash::Sha256).toHex().left(16)));
}

PhysicalControllerRegistry::Match
PhysicalControllerRegistry::findMatch(const ProviderObservation& observation) const
{
    // Evidence is ranked, strongest first. An exact per-device identity
    // (app-local ID, endpoint) always wins over shared-topology evidence
    // (container, root), because a container or root can legitimately hold
    // several endpoints. Every rank demands exactly one non-contradictory
    // candidate, so a match never depends on QHash iteration order and
    // ambiguity deliberately creates a new identity instead of guessing.
    const auto contradicts = [](const QString& left, const QString& right) {
        return !left.isEmpty() && !right.isEmpty() && left != right;
    };
    const auto uniqueCandidate = [this](auto&& accept) -> QString {
        QString match;
        for (auto it = m_controllers.cbegin(); it != m_controllers.cend(); ++it) {
            if (!accept(it.value()))
                continue;
            if (!match.isEmpty())
                return {};
            match = it.key();
        }
        return match;
    };

    if (!observation.appLocalDeviceId.isEmpty()) {
        const QString match = uniqueCandidate([&](const LogicalController& candidate) {
            return candidate.appLocalDeviceId == observation.appLocalDeviceId;
        });
        if (!match.isEmpty())
            return {match, MatchEvidence::AppLocalDeviceId};
    }

    if (!observation.endpointId.isEmpty()) {
        const QString match = uniqueCandidate([&](const LogicalController& candidate) {
            return candidate.endpointId == observation.endpointId
                && !candidate.hasProvider(observation.provider)
                && !contradicts(observation.appLocalDeviceId, candidate.appLocalDeviceId);
        });
        if (!match.isEmpty())
            return {match, MatchEvidence::EndpointId};
    }

    // A shared container may hold several endpoints (hub, receiver): a
    // container match must never merge two conflicting strong device IDs.
    if (!observation.containerId.isEmpty()) {
        const QString match = uniqueCandidate([&](const LogicalController& candidate) {
            return candidate.containerId == observation.containerId
                && !candidate.hasProvider(observation.provider)
                && !contradicts(observation.appLocalDeviceId, candidate.appLocalDeviceId)
                && !contradicts(observation.endpointId, candidate.endpointId);
        });
        if (!match.isEmpty())
            return {match, MatchEvidence::ContainerId};
    }

    if (!observation.topologyRoot.isEmpty()) {
        const QString match = uniqueCandidate([&](const LogicalController& candidate) {
            return candidate.topologyRoot == observation.topologyRoot
                && !candidate.hasProvider(observation.provider)
                && !contradicts(observation.appLocalDeviceId, candidate.appLocalDeviceId)
                && !contradicts(observation.endpointId, candidate.endpointId)
                && !contradicts(observation.containerId, candidate.containerId);
        });
        if (!match.isEmpty())
            return {match, MatchEvidence::TopologyRoot};
    }

    return {};
}

QString PhysicalControllerRegistry::observe(const ProviderObservation& observation,
                                            QString* rekeyedFrom)
{
    if (rekeyedFrom)
        rekeyedFrom->clear();
    const QString key = attachmentKey(observation.provider, observation.providerDeviceId);
    if (const auto existing = m_attachmentToLogical.constFind(key);
        existing != m_attachmentToLogical.cend()) {
        // Re-observation of a live attachment (e.g. CapabilityChanged) must
        // refresh its capabilities and fill in missing metadata, not no-op.
        QString currentId = *existing;
        if (const auto it = m_controllers.find(currentId); it != m_controllers.end()) {
            for (auto& attachment : it->providers) {
                if (attachment.provider == observation.provider
                    && attachment.providerDeviceId == observation.providerDeviceId) {
                    attachment.capabilities = observation.capabilities;
                    attachment.controls.unite(observation.controls);
                }
            }
            if (it->displayName.isEmpty())
                it->displayName = observation.displayName;
            if (it->appLocalDeviceId.isEmpty())
                it->appLocalDeviceId = observation.appLocalDeviceId;
            if (it->containerId.isEmpty())
                it->containerId = observation.containerId;
            if (it->topologyRoot.isEmpty())
                it->topologyRoot = observation.topologyRoot;
            if (it->endpointId.isEmpty())
                it->endpointId = observation.endpointId;
            if (it->modelFingerprint.isEmpty())
                it->modelFingerprint = observation.modelFingerprint;
        }
        const bool strongObservation = !observation.appLocalDeviceId.isEmpty()
            || !observation.containerId.isEmpty() || !observation.topologyRoot.isEmpty()
            || !observation.endpointId.isEmpty();
        const auto current = m_controllers.constFind(currentId);
        if (strongObservation && current != m_controllers.cend()
            && current->confidence == IdentityConfidence::Weak) {
            const QString upgradedId = createLogicalId(observation, 0);
            if (upgradedId != currentId && !m_controllers.contains(upgradedId)) {
                LogicalController moved = m_controllers.take(currentId);
                moved.logicalId = upgradedId;
                moved.confidence = IdentityConfidence::Strong;
                m_controllers.insert(upgradedId, moved);
                for (auto attachment = m_attachmentToLogical.begin();
                     attachment != m_attachmentToLogical.end(); ++attachment) {
                    if (attachment.value() == currentId)
                        attachment.value() = upgradedId;
                }
                if (rekeyedFrom)
                    *rekeyedFrom = currentId;
                currentId = upgradedId;
            }
        }
        return currentId;
    }

    const Match match = findMatch(observation);
    QString logicalId = match.logicalId;
    MatchEvidence evidence = match.evidence;
    IdentityConfidence confidence = IdentityConfidence::Strong;
    // A strong observation merging into a controller that was created from a
    // weak identity (a legacy provider observed first) upgrades the logical
    // ID to the deterministic strong hash. Persisted per-controller state
    // (binding profiles, extra-button layouts) is keyed on that ID, so the
    // upgrade is what reconnects a device to its saved profile regardless of
    // which provider observed it first this session.
    if (!logicalId.isEmpty()
        && (!observation.appLocalDeviceId.isEmpty() || !observation.containerId.isEmpty()
            || !observation.topologyRoot.isEmpty() || !observation.endpointId.isEmpty())) {
        const auto existing = m_controllers.constFind(logicalId);
        const bool appLocalUpgrade = !observation.appLocalDeviceId.isEmpty()
            && existing != m_controllers.cend() && existing->appLocalDeviceId.isEmpty();
        const bool weakUpgrade = existing != m_controllers.cend()
            && existing->confidence == IdentityConfidence::Weak;
        if (existing != m_controllers.cend() && (appLocalUpgrade || weakUpgrade)) {
            const QString upgradedId = createLogicalId(observation, 0);
            if (upgradedId != logicalId && !m_controllers.contains(upgradedId)) {
                const QString previousId = logicalId;
                LogicalController moved = m_controllers.take(logicalId);
                moved.logicalId = upgradedId;
                m_controllers.insert(upgradedId, moved);
                for (auto it = m_attachmentToLogical.begin();
                     it != m_attachmentToLogical.end(); ++it) {
                    if (it.value() == logicalId)
                        it.value() = upgradedId;
                }
                logicalId = upgradedId;
                if (rekeyedFrom)
                    *rekeyedFrom = previousId;
            }
        }
    }
    if (logicalId.isEmpty()) {
        logicalId = createLogicalId(observation, ++m_generation);
        // Shared evidence (a receiver root, a hub container) can hash two
        // deliberately separate endpoints onto one ID. findMatch() already
        // refused to merge them, so the new identity must not merge them
        // through the back door either: disambiguate on the provider
        // endpoint, which stays stable for this device across sessions.
        if (m_controllers.contains(logicalId)) {
            logicalId = createLogicalId(
                observation, m_generation,
                QStringLiteral("|%1").arg(
                    attachmentKey(observation.provider, observation.providerDeviceId)));
        }
        evidence = MatchEvidence::NewIdentity;
        confidence = (!observation.appLocalDeviceId.isEmpty()
                      || !observation.containerId.isEmpty()
                      || !observation.topologyRoot.isEmpty()
                      || !observation.endpointId.isEmpty())
            ? IdentityConfidence::Strong : IdentityConfidence::Weak;
        LogicalController controller;
        controller.logicalId = logicalId;
        controller.displayName = observation.displayName;
        controller.confidence = confidence;
        controller.appLocalDeviceId = observation.appLocalDeviceId;
        controller.containerId = observation.containerId;
        controller.topologyRoot = observation.topologyRoot;
        controller.endpointId = observation.endpointId;
        controller.modelFingerprint = observation.modelFingerprint;
        controller.vendorId = observation.vendorId;
        controller.productId = observation.productId;
        m_controllers.insert(logicalId, controller);
    }

    auto& controller = m_controllers[logicalId];
    if (controller.displayName.isEmpty())
        controller.displayName = observation.displayName;
    if (controller.appLocalDeviceId.isEmpty())
        controller.appLocalDeviceId = observation.appLocalDeviceId;
    if (controller.containerId.isEmpty())
        controller.containerId = observation.containerId;
    if (controller.topologyRoot.isEmpty())
        controller.topologyRoot = observation.topologyRoot;
    if (controller.endpointId.isEmpty())
        controller.endpointId = observation.endpointId;
    if (controller.modelFingerprint.isEmpty())
        controller.modelFingerprint = observation.modelFingerprint;
    controller.confidence = std::max(controller.confidence, confidence);
    controller.providers.push_back({observation.provider, observation.providerDeviceId,
                                    observation.capabilities, observation.controls,
                                    evidence});
    m_attachmentToLogical.insert(key, logicalId);
    return logicalId;
}

bool PhysicalControllerRegistry::removeProvider(ControllerProvider provider,
                                                const QString& providerDeviceId)
{
    const QString key = attachmentKey(provider, providerDeviceId);
    const QString logicalId = m_attachmentToLogical.take(key);
    if (logicalId.isEmpty())
        return false;
    auto it = m_controllers.find(logicalId);
    if (it == m_controllers.end())
        return false;
    auto& providers = it->providers;
    providers.erase(std::remove_if(providers.begin(), providers.end(),
                                   [&](const auto& item) {
                                       return item.provider == provider
                                           && item.providerDeviceId == providerDeviceId;
                                   }), providers.end());
    // Retain the logical identity while disconnected. A later observation
    // carrying the same strong ID restores this controller's profile; weak
    // observations still create a separate logical controller.
    return true;
}

const LogicalController* PhysicalControllerRegistry::controller(const QString& logicalId) const
{
    const auto it = m_controllers.constFind(logicalId);
    return it == m_controllers.cend() ? nullptr : &it.value();
}

QVector<LogicalController> PhysicalControllerRegistry::controllers() const
{
    return m_controllers.values();
}

QString PhysicalControllerRegistry::logicalIdFor(ControllerProvider provider,
                                                 const QString& providerDeviceId) const
{
    return m_attachmentToLogical.value(attachmentKey(provider, providerDeviceId));
}

ControllerProvider PhysicalControllerRegistry::preferredProvider(
    const QString& logicalId, ControllerCapability capability,
    const QString& controlId) const
{
    const auto* logical = controller(logicalId);
    if (!logical)
        return ControllerProvider::WinMM;

    const bool hasExplicitOwner = !controlId.isEmpty()
        && std::any_of(logical->providers.cbegin(), logical->providers.cend(),
                       [&](const auto& item) {
                           return item.capabilities.testFlag(capability)
                               && item.controls.contains(controlId);
                       });
    const auto supports = [&](ControllerProvider provider) {
        return std::any_of(logical->providers.cbegin(), logical->providers.cend(),
                           [&](const auto& item) {
                               return item.provider == provider
                                   && item.capabilities.testFlag(capability)
                                   && (!hasExplicitOwner || item.controls.contains(controlId));
                           });
    };
    const QVector<ControllerProvider> order = capability == ControllerCapability::StandardControls
        ? QVector<ControllerProvider>{ControllerProvider::SonyRaw, ControllerProvider::GameInput,
                                      ControllerProvider::XInput, ControllerProvider::WinMM}
        : QVector<ControllerProvider>{ControllerProvider::GameInput, ControllerProvider::SonyRaw,
                                      ControllerProvider::RawHid, ControllerProvider::XInput,
                                      ControllerProvider::WinMM};
    for (ControllerProvider provider : order) {
        if (supports(provider))
            return provider;
    }
    return ControllerProvider::WinMM;
}

MatchEvidence PhysicalControllerRegistry::matchEvidence(
    ControllerProvider provider, const QString& providerDeviceId) const
{
    const auto* logical = controller(logicalIdFor(provider, providerDeviceId));
    if (!logical)
        return MatchEvidence::NewIdentity;
    for (const auto& attachment : logical->providers) {
        if (attachment.provider == provider
            && attachment.providerDeviceId == providerDeviceId)
            return attachment.evidence;
    }
    return MatchEvidence::NewIdentity;
}

bool PhysicalControllerRegistry::addProviderControl(
    ControllerProvider provider, const QString& providerDeviceId,
    const QString& controlId)
{
    if (controlId.isEmpty())
        return false;
    const QString logicalId = logicalIdFor(provider, providerDeviceId);
    auto logical = m_controllers.find(logicalId);
    if (logical == m_controllers.end())
        return false;
    for (auto& attachment : logical->providers) {
        if (attachment.provider == provider
            && attachment.providerDeviceId == providerDeviceId) {
            attachment.controls.insert(controlId);
            return true;
        }
    }
    return false;
}

} // namespace ModernInput
