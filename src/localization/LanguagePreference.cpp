#include "localization/LanguagePreference.h"

#include "config/ConfigKeys.h"
#include "config/ConfigManager.h"
#include "localization/LanguageManager.h"
#include "localization/LocaleRegistry.h"

#include <QDebug>
#include <QScopedValueRollback>

namespace
{
constexpr auto kRegistryPath = "HKEY_CURRENT_USER\\Software\\underfusion\\GameHQ";
constexpr auto kBootstrapValue = "BootstrapLanguage";
constexpr auto kSystemLanguage = "system";
}

RegistryLanguageBootstrapStore::RegistryLanguageBootstrapStore(const QString &registryPath)
    : m_settings(registryPath.isEmpty() ? QString::fromLatin1(kRegistryPath) : registryPath,
                 QSettings::NativeFormat)
{
}

bool RegistryLanguageBootstrapStore::hasValue() const
{
    return m_settings.contains(QString::fromLatin1(kBootstrapValue));
}

QString RegistryLanguageBootstrapStore::value() const
{
    return m_settings.value(QString::fromLatin1(kBootstrapValue)).toString();
}

bool RegistryLanguageBootstrapStore::removeValue()
{
    m_settings.remove(QString::fromLatin1(kBootstrapValue));
    m_settings.sync();
    return m_settings.status() == QSettings::NoError && !hasValue();
}

LanguagePreference::LanguagePreference(ConfigManager *config, LocaleRegistry *registry,
                                       QObject *parent)
    : QObject(parent)
    , m_config(config)
    , m_registry(registry)
{
    Q_ASSERT(m_config);
    Q_ASSERT(m_registry);
}

void LanguagePreference::appendWarning(QString *warning, const QString &message)
{
    if (!warning)
        return;
    if (!warning->isEmpty())
        warning->append(QStringLiteral("; "));
    warning->append(message);
}

QString LanguagePreference::normalized(const QString &requested) const
{
    const QString trimmed = requested.trimmed();
    if (trimmed.isEmpty()
        || trimmed.compare(QString::fromLatin1(kSystemLanguage), Qt::CaseInsensitive) == 0) {
        return QString::fromLatin1(kSystemLanguage);
    }

    const QString canonical = m_registry->canonicalTag(trimmed);
    return !canonical.isEmpty() && m_registry->isAvailable(canonical)
        ? canonical : QString::fromLatin1(kSystemLanguage);
}

QString LanguagePreference::initialLanguage(LanguageBootstrapStore &bootstrap, bool portable,
                                             QString *warning)
{
    if (warning)
        warning->clear();

    if (m_config->hasExplicitValue(QString(ConfigKeys::UiLanguage))) {
        const QString stored = m_config->value(QString(ConfigKeys::UiLanguage)).toString();
        const QString requested = normalized(stored);
        if (requested != stored) {
            m_config->setValue(QString(ConfigKeys::UiLanguage), requested);
            if (!m_config->save())
                appendWarning(warning, QStringLiteral("could not repair the stored language"));
        }
        if (!portable && bootstrap.hasValue() && !bootstrap.removeValue()) {
            appendWarning(warning,
                          QStringLiteral("could not discard a stale installer language"));
        }
        return requested;
    }

    if (portable || !bootstrap.hasValue())
        return QString::fromLatin1(kSystemLanguage);

    const QString requested = normalized(bootstrap.value());
    const bool valid = requested != QString::fromLatin1(kSystemLanguage);
    if (!valid) {
        if (!bootstrap.removeValue()) {
            appendWarning(warning,
                          QStringLiteral("invalid installer language could not be discarded"));
        }
        return QString::fromLatin1(kSystemLanguage);
    }

    m_config->setValue(QString(ConfigKeys::UiLanguage), requested);
    if (!m_config->save()) {
        m_config->resetValue(QString(ConfigKeys::UiLanguage));
        appendWarning(warning, QStringLiteral("installer language could not be persisted"));
        return QString::fromLatin1(kSystemLanguage);
    }
    if (!bootstrap.removeValue()) {
        appendWarning(warning,
                      QStringLiteral("installer language was saved but could not be cleared"));
    }
    return requested;
}

void LanguagePreference::bind(LanguageManager *manager)
{
    Q_ASSERT(manager);
    Q_ASSERT(!m_manager);
    m_manager = manager;

    connect(m_manager, &LanguageManager::requestedLanguageChanged, this, [this] {
        if (m_applyingConfig)
            return;
        const QString requested = normalized(m_manager->requestedLanguage());
        m_config->setValue(QString(ConfigKeys::UiLanguage), requested);
        if (!m_config->save())
            qWarning() << "Localization preference could not be saved";
    });
    connect(m_config, &ConfigManager::valueChanged, this,
            [this](const QString &key, const QVariant &value) {
        if (key != ConfigKeys::UiLanguage || m_applyingConfig)
            return;
        QScopedValueRollback applying(m_applyingConfig, true);
        const QString requested = normalized(value.toString());
        if (requested != value.toString())
            m_config->setValue(QString(ConfigKeys::UiLanguage), requested);
        m_manager->setRequestedLanguage(requested);
    });
}
