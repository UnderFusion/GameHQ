#pragma once

#include <QFlags>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QSet>
#include <QVector>

namespace ModernInput {

enum class ControllerProvider {
    SonyRaw,
    GameInput,
    XInput,
    WinMM,
    RawHid
};

enum class ControllerCapability : quint32 {
    None = 0,
    StandardControls = 1u << 0,
    SystemShare = 1u << 1,
    Guide = 1u << 2,
    ExtraControls = 1u << 3
};
Q_DECLARE_FLAGS(ControllerCapabilities, ControllerCapability)

enum class IdentityConfidence {
    Weak,
    // Reserved for a future genuine cross-provider topology key. No provider
    // currently produces evidence that earns this level (see cpo-c03).
    Correlated,
    Strong
};

// Which piece of evidence attached a provider to a logical controller.
// Registry-internal provenance only: it is never part of the sanitized
// diagnostics bundle.
enum class MatchEvidence {
    NewIdentity,
    AppLocalDeviceId,
    EndpointId,
    ContainerId,
    TopologyRoot
};

struct ProviderObservation {
    ControllerProvider provider = ControllerProvider::WinMM;
    QString providerDeviceId;
    QString appLocalDeviceId;
    QString containerId;
    QString topologyRoot;
    QString endpointId;
    QString modelFingerprint;
    QString displayName;
    quint16 vendorId = 0;
    quint16 productId = 0;
    ControllerCapabilities capabilities;
    QSet<QString> controls;
};

struct ProviderAttachment {
    ControllerProvider provider = ControllerProvider::WinMM;
    QString providerDeviceId;
    ControllerCapabilities capabilities;
    QSet<QString> controls;
    MatchEvidence evidence = MatchEvidence::NewIdentity;
};

struct LogicalController {
    QString logicalId;
    QString displayName;
    IdentityConfidence confidence = IdentityConfidence::Weak;
    QString appLocalDeviceId;
    QString containerId;
    QString topologyRoot;
    QString endpointId;
    QString modelFingerprint;
    quint16 vendorId = 0;
    quint16 productId = 0;
    QVector<ProviderAttachment> providers;

    ControllerCapabilities capabilities() const;
    bool hasProvider(ControllerProvider provider) const;
    bool connected() const { return !providers.isEmpty(); }
};

class PhysicalControllerRegistry
{
public:
    QString observe(const ProviderObservation& observation,
                    QString* rekeyedFrom = nullptr);
    bool removeProvider(ControllerProvider provider, const QString& providerDeviceId);
    const LogicalController* controller(const QString& logicalId) const;
    QVector<LogicalController> controllers() const;
    QString logicalIdFor(ControllerProvider provider, const QString& providerDeviceId) const;
    ControllerProvider preferredProvider(const QString& logicalId,
                                         ControllerCapability capability,
                                         const QString& controlId = {}) const;
    bool addProviderControl(ControllerProvider provider,
                            const QString& providerDeviceId,
                            const QString& controlId);
    MatchEvidence matchEvidence(ControllerProvider provider,
                                const QString& providerDeviceId) const;

private:
    struct Match {
        QString logicalId;
        MatchEvidence evidence = MatchEvidence::NewIdentity;
    };

    Match findMatch(const ProviderObservation& observation) const;
    static QString attachmentKey(ControllerProvider provider, const QString& providerDeviceId);
    static QString createLogicalId(const ProviderObservation& observation,
                                   quint64 generation,
                                   const QString& disambiguator = {});

    QHash<QString, LogicalController> m_controllers;
    QHash<QString, QString> m_attachmentToLogical;
    quint64 m_generation = 0;
};

} // namespace ModernInput

Q_DECLARE_OPERATORS_FOR_FLAGS(ModernInput::ControllerCapabilities)
