#include "localization/LanguageManager.h"
#include "localization/LocaleRegistry.h"

#include <QCoreApplication>
#include <QLocale>
#include <QTranslator>
#include <algorithm>
#include <utility>

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

void LanguageManager::setQmlRetranslateCallback(std::function<void()> callback)
{
    m_qmlRetranslate = std::move(callback);
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

    const bool initializing = !m_sourceTranslator;
    std::unique_ptr<QTranslator> sourceTranslator;
    if (initializing) {
        sourceTranslator = std::make_unique<QTranslator>();
        const QString sourcePath = catalogPath(sourceLanguage);
        if (sourcePath.isEmpty() || !sourceTranslator->load(sourcePath)) {
            if (error) {
                *error = QStringLiteral("Source catalog is missing or corrupt: %1")
                             .arg(sourcePath);
            }
            return false;
        }
    }

    std::unique_ptr<QTranslator> replacementTranslator;
    if (effective != sourceLanguage) {
        replacementTranslator = std::make_unique<QTranslator>();
        const QString activePath = catalogPath(effective);
        if (activePath.isEmpty() || !replacementTranslator->load(activePath)) {
            const QString reason = QStringLiteral("Catalog is missing or corrupt: %1")
                                       .arg(activePath);
            if (!initializing) {
                if (error)
                    *error = reason;
                return false;
            }
            emit catalogLoadFailed(effective, reason);
            replacementTranslator.reset();
            effective = sourceLanguage;
        }
    }

    const bool requestedChanged = normalizedRequested != m_requestedLanguage;
    const bool effectiveChanged = effective != m_effectiveLanguage;

    if (initializing) {
        if (!QCoreApplication::installTranslator(sourceTranslator.get())) {
            if (error)
                *error = QStringLiteral("Source catalog could not be installed");
            return false;
        }
        if (replacementTranslator
            && !QCoreApplication::installTranslator(replacementTranslator.get())) {
            QCoreApplication::removeTranslator(sourceTranslator.get());
            if (error)
                *error = QStringLiteral("Requested catalog could not be installed");
            return false;
        }
        m_sourceTranslator = std::move(sourceTranslator);
    } else if (replacementTranslator
               && !QCoreApplication::installTranslator(replacementTranslator.get())) {
        if (error)
            *error = QStringLiteral("Requested catalog could not be installed");
        return false;
    }

    // The replacement is installed above the previous target first. Removing
    // the old translator therefore never exposes a source/target mixture, and
    // a failed load or install leaves the complete previous stack untouched.
    if (m_activeTranslator)
        QCoreApplication::removeTranslator(m_activeTranslator.get());
    m_activeTranslator = std::move(replacementTranslator);
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
    emit retranslationRequested();
    if (m_qmlRetranslate)
        m_qmlRetranslate();
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

QString LanguageManager::formatInteger(qint64 value) const
{
    return QLocale().toString(value);
}

QString LanguageManager::formatDecimal(double value, int precision) const
{
    return QLocale().toString(value, 'f', std::max(0, precision));
}

QString LanguageManager::formatDate(const QDateTime &value) const
{
    return value.isValid() ? QLocale().toString(value.toLocalTime().date(), QLocale::ShortFormat)
                           : QString();
}

QString LanguageManager::formatDateTime(const QDateTime &value) const
{
    return value.isValid() ? QLocale().toString(value.toLocalTime(), QLocale::ShortFormat)
                           : QString();
}

QString LanguageManager::formatDuration(qint64 milliseconds) const
{
    const qint64 totalSeconds = std::max<qint64>(0, milliseconds) / 1000;
    const qint64 minutes = totalSeconds / 60;
    const qint64 seconds = totalSeconds % 60;
    const QLocale locale;
    const QString zeroDigit = locale.zeroDigit();
    const QString secondsText = locale.toString(seconds).rightJustified(
        2, zeroDigit.isEmpty() ? u'0' : zeroDigit.front());
    return QStringLiteral("%1:%2").arg(locale.toString(minutes), secondsText);
}

QString LanguageManager::formatList(const QStringList &values) const
{
    return QLocale().createSeparatedList(values);
}
