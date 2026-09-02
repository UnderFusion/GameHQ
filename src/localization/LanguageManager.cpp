#include "localization/LanguageManager.h"
#include "localization/LocaleRegistry.h"

#include <QCoreApplication>
#include <QLocale>
#include <QTranslator>

LanguageManager::LanguageManager(LocaleRegistry *registry, QObject *parent)
    : QObject(parent)
    , m_registry(registry)
{
    Q_ASSERT(m_registry);
}

LanguageManager::~LanguageManager()
{
    uninstallTranslators();
}

QString LanguageManager::catalogPath(const QString &tag) const
{
    const QString catalog = m_registry->catalogName(tag);
    return catalog.isEmpty() ? QString() : QStringLiteral(":/i18n/%1.qm").arg(catalog);
}

void LanguageManager::uninstallTranslators()
{
    if (m_activeTranslator)
        QCoreApplication::removeTranslator(m_activeTranslator.get());
    if (m_sourceTranslator)
        QCoreApplication::removeTranslator(m_sourceTranslator.get());
    m_activeTranslator.reset();
    m_sourceTranslator.reset();
}

bool LanguageManager::initialize(const QString &requestedLanguage,
                                 const QStringList &systemUiLanguages, QString *error)
{
    m_systemUiLanguages = systemUiLanguages.isEmpty()
        ? QLocale::system().uiLanguages() : systemUiLanguages;
    return apply(requestedLanguage, m_systemUiLanguages, error);
}

void LanguageManager::setRequestedLanguage(const QString &requestedLanguage)
{
    QString error;
    if (!apply(requestedLanguage, m_systemUiLanguages, &error))
        emit catalogLoadFailed(requestedLanguage, error);
}

bool LanguageManager::apply(const QString &requestedLanguage,
                            const QStringList &systemUiLanguages, QString *error)
{
    const QString trimmed = requestedLanguage.trimmed();
    const bool useSystem = trimmed.isEmpty()
        || trimmed.compare(QStringLiteral("system"), Qt::CaseInsensitive) == 0;
    const QString canonical = useSystem ? QString() : m_registry->canonicalTag(trimmed);
    const QString normalizedRequested = useSystem ? QStringLiteral("system")
        : (canonical.isEmpty() ? trimmed : canonical);
    QString effective = useSystem ? m_registry->resolveSystem(systemUiLanguages)
                                  : m_registry->resolveAvailable(trimmed);
    const QString sourceLanguage = m_registry->sourceLanguage();
    if (effective.isEmpty() || sourceLanguage.isEmpty()) {
        if (error)
            *error = QStringLiteral("Locale registry has no usable source language");
        return false;
    }

    auto sourceTranslator = std::make_unique<QTranslator>();
    const QString sourcePath = catalogPath(sourceLanguage);
    if (sourcePath.isEmpty() || !sourceTranslator->load(sourcePath)) {
        if (error)
            *error = QStringLiteral("Source catalog is missing or corrupt: %1").arg(sourcePath);
        return false;
    }

    std::unique_ptr<QTranslator> activeTranslator;
    if (effective != sourceLanguage) {
        activeTranslator = std::make_unique<QTranslator>();
        const QString activePath = catalogPath(effective);
        if (activePath.isEmpty() || !activeTranslator->load(activePath)) {
            emit catalogLoadFailed(effective,
                                   QStringLiteral("Catalog is missing or corrupt: %1")
                                       .arg(activePath));
            activeTranslator.reset();
            effective = sourceLanguage;
        }
    }

    const bool requestedChanged = normalizedRequested != m_requestedLanguage;
    const bool effectiveChanged = effective != m_effectiveLanguage;
    uninstallTranslators();
    m_sourceTranslator = std::move(sourceTranslator);
    m_activeTranslator = std::move(activeTranslator);
    QCoreApplication::installTranslator(m_sourceTranslator.get());
    if (m_activeTranslator)
        QCoreApplication::installTranslator(m_activeTranslator.get());
    QLocale::setDefault(QLocale(effective));

    m_requestedLanguage = normalizedRequested;
    m_effectiveLanguage = effective;
    ++m_translationRevision;
    if (error)
        error->clear();
    if (requestedChanged)
        emit requestedLanguageChanged();
    if (effectiveChanged)
        emit languageChanged();
    emit translationRevisionChanged();
    return true;
}

QVariantList LanguageManager::availableLanguages() const
{
    return m_registry->availableLanguages();
}

Qt::LayoutDirection LanguageManager::layoutDirection() const
{
    return m_registry->layoutDirection(m_effectiveLanguage);
}
