#pragma once

#include "share/ShareProvider.h"

#include <QObject>
#include <QPointer>
#include <QVector>

namespace share
{

// Owns nothing but the list: providers are parented by whoever created them
// (App for built-ins, the external bridge for out-of-process ones). Order is
// registration order, which is also the order the Share dialog shows.
class ProviderRegistry : public QObject
{
    Q_OBJECT
public:
    explicit ProviderRegistry(QObject* parent = nullptr);

    enum class AddError { None, NullProvider, InvalidId, DuplicateId };
    AddError add(Provider* provider);
    bool remove(const QString& providerId);

    Provider* find(const QString& providerId) const;
    QVector<Provider*> providers() const;
    // Providers whose capabilities accept this request's media kind. Includes
    // unavailable ones so the UI can explain why they are disabled.
    QVector<Provider*> providersFor(const Request& request) const;

signals:
    void providersChanged();

private:
    QVector<QPointer<Provider>> m_providers;
};

} // namespace share
