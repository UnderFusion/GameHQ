#include "localization/StartupLocalization.h"

StartupLocalization::StartupLocalization()
    : m_languageManager(&m_registry)
{
}

bool StartupLocalization::initialize(QString *error)
{
    return initialize(QStringLiteral(":/i18n/locales.json"), {}, error);
}

bool StartupLocalization::initialize(const QString &manifestPath,
                                     const QStringList &systemUiLanguages,
                                     QString *error)
{
    if (!m_registry.load(manifestPath, error))
        return false;
    return m_languageManager.initialize(QStringLiteral("system"), systemUiLanguages, error);
}
