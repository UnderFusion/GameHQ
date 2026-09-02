#pragma once

#include "localization/LanguageManager.h"
#include "localization/LocaleRegistry.h"

#include <QStringList>

class StartupLocalization final
{
public:
    StartupLocalization();

    bool initialize(QString *error = nullptr);
    bool initialize(const QString &manifestPath, const QStringList &systemUiLanguages,
                    QString *error = nullptr);

private:
    LocaleRegistry m_registry;
    LanguageManager m_languageManager;
};
