#include "share/ShareProviderRegistry.h"

#include <QDebug>

namespace share
{

ProviderRegistry::ProviderRegistry(QObject* parent)
    : QObject(parent)
{
}

ProviderRegistry::AddError ProviderRegistry::add(Provider* provider)
{
    if (!provider)
        return AddError::NullProvider;
    const QString id = provider->id();
    if (!isValidProviderId(id)) {
        qWarning() << "Share: refusing provider with invalid id" << id;
        return AddError::InvalidId;
    }
    if (find(id)) {
        qWarning() << "Share: refusing duplicate provider id" << id;
        return AddError::DuplicateId;
    }
    m_providers.append(provider);
    connect(provider, &QObject::destroyed, this, [this, id] { remove(id); });
    connect(provider, &Provider::stateChanged, this, &ProviderRegistry::providersChanged);
    qInfo() << "Share: provider registered" << id
            << "caps=" << capabilityNames(provider->capabilities()).join(QLatin1Char(','));
    emit providersChanged();
    return AddError::None;
}

bool ProviderRegistry::remove(const QString& providerId)
{
    // A destroyed provider leaves a null QPointer; purge those on every call.
    bool changed = m_providers.removeIf([](const QPointer<Provider>& p) { return p.isNull(); }) > 0;
    for (int i = 0; i < m_providers.size(); ++i) {
        if (m_providers.at(i)->id() != providerId)
            continue;
        disconnect(m_providers.at(i).data(), nullptr, this, nullptr);
        m_providers.removeAt(i);
        changed = true;
        break;
    }
    if (changed)
        emit providersChanged();
    return changed;
}

Provider* ProviderRegistry::find(const QString& providerId) const
{
    for (const QPointer<Provider>& p : m_providers) {
        if (!p.isNull() && p->id() == providerId)
            return p.data();
    }
    return nullptr;
}

QVector<Provider*> ProviderRegistry::providers() const
{
    QVector<Provider*> out;
    for (const QPointer<Provider>& p : m_providers) {
        if (!p.isNull())
            out.append(p.data());
    }
    return out;
}

QVector<Provider*> ProviderRegistry::providersFor(const Request& request) const
{
    QVector<Provider*> out;
    if (!request.isValid())
        return out;
    for (Provider* p : providers()) {
        if (p->capabilities().testFlag(request.requiredCapability()))
            out.append(p);
    }
    return out;
}

} // namespace share
