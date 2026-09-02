#include "localization/LocaleRegistry.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLatin1StringView>
#include <QRegularExpression>
#include <QSet>
#include <QVariantMap>
#include <utility>

namespace {

bool fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return false;
}

bool isString(const QJsonObject &object, QLatin1StringView key)
{
    return object.value(key).isString() && !object.value(key).toString().trimmed().isEmpty();
}

} // namespace

LocaleRegistry::LocaleRegistry(QObject *parent)
    : LocaleRegistry(developmentLocalesEnabled(), parent)
{
}

LocaleRegistry::LocaleRegistry(bool includeInternal, QObject *parent)
    : QObject(parent)
    , m_includeInternal(includeInternal)
{
}

bool LocaleRegistry::developmentLocalesEnabled()
{
#ifdef NDEBUG
    return false;
#else
    return true;
#endif
}

QString LocaleRegistry::keyFor(const QString &tag)
{
    QString key = tag.trimmed();
    key.replace(QLatin1Char('_'), QLatin1Char('-'));
    return key.toLower();
}

bool LocaleRegistry::load(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(error, QStringLiteral("Cannot read %1: %2").arg(path, file.errorString()));
    return loadData(file.readAll(), error);
}

bool LocaleRegistry::loadData(const QByteArray &data, QString *error)
{
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return fail(error, QStringLiteral("Manifest JSON is invalid: %1")
                               .arg(parseError.errorString()));
    }

    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("schema_version")).toInt(-1) != 1)
        return fail(error, QStringLiteral("Unsupported locale manifest schema"));
    if (!isString(root, QLatin1StringView("source_language"))
        || !isString(root, QLatin1StringView("default_locale"))
        || !root.value(QStringLiteral("locales")).isArray()
        || !root.value(QStringLiteral("aliases")).isObject()) {
        return fail(error, QStringLiteral("Locale manifest is missing required fields"));
    }

    const QRegularExpression tagPattern(
        QStringLiteral("^[a-z]{2,3}(?:-[A-Z][a-z]{3,4})?(?:-[A-Z]{2}|-[0-9]{3})?$"));
    const QSet<QString> validStates{QStringLiteral("enabled"), QStringLiteral("disabled"),
                                    QStringLiteral("reserve"), QStringLiteral("internal")};
    const QSet<QString> validDirections{QStringLiteral("ltr"), QStringLiteral("rtl")};

    QVector<Locale> locales;
    QHash<QString, qsizetype> indexes;
    QHash<QString, QString> derivedAliases;
    const QJsonArray localeValues = root.value(QStringLiteral("locales")).toArray();
    if (localeValues.isEmpty())
        return fail(error, QStringLiteral("Locale manifest contains no locales"));

    for (const QJsonValue &value : localeValues) {
        if (!value.isObject())
            return fail(error, QStringLiteral("Locale entry is not an object"));
        const QJsonObject object = value.toObject();
        if (!isString(object, QLatin1StringView("tag"))
            || !object.value(QStringLiteral("names")).isObject()
            || !object.value(QStringLiteral("aliases")).isArray()
            || !isString(object, QLatin1StringView("state"))
            || !isString(object, QLatin1StringView("direction"))
            || !isString(object, QLatin1StringView("qt_catalog"))) {
            return fail(error, QStringLiteral("Locale entry is missing required fields"));
        }

        Locale locale;
        locale.tag = object.value(QStringLiteral("tag")).toString();
        const QJsonObject names = object.value(QStringLiteral("names")).toObject();
        locale.nativeName = names.value(QStringLiteral("native")).toString().trimmed();
        locale.englishName = names.value(QStringLiteral("english")).toString().trimmed();
        locale.state = object.value(QStringLiteral("state")).toString();
        locale.direction = object.value(QStringLiteral("direction")).toString();
        locale.catalog = object.value(QStringLiteral("qt_catalog")).toString().trimmed();
        locale.tier = object.value(QStringLiteral("tier")).toInt(-1);
        const QJsonValue fallback = object.value(QStringLiteral("fallback"));
        if (!fallback.isNull() && !fallback.isString())
            return fail(error, QStringLiteral("Invalid fallback for %1").arg(locale.tag));
        locale.fallback = fallback.toString();

        if (!tagPattern.match(locale.tag).hasMatch() || locale.nativeName.isEmpty()
            || locale.englishName.isEmpty() || !validStates.contains(locale.state)
            || !validDirections.contains(locale.direction) || locale.tier < 0 || locale.tier > 2) {
            return fail(error, QStringLiteral("Invalid locale metadata for %1").arg(locale.tag));
        }

        const QString tagKey = keyFor(locale.tag);
        if (indexes.contains(tagKey))
            return fail(error, QStringLiteral("Duplicate locale tag %1").arg(locale.tag));
        indexes.insert(tagKey, locales.size());

        for (const QJsonValue &aliasValue : object.value(QStringLiteral("aliases")).toArray()) {
            if (!aliasValue.isString() || aliasValue.toString().trimmed().isEmpty())
                return fail(error, QStringLiteral("Invalid alias for %1").arg(locale.tag));
            const QString alias = aliasValue.toString();
            const QString aliasKey = keyFor(alias);
            if (derivedAliases.contains(aliasKey)
                && derivedAliases.value(aliasKey) != locale.tag) {
                return fail(error, QStringLiteral("Duplicate locale alias %1").arg(alias));
            }
            locale.aliases.append(alias);
            derivedAliases.insert(aliasKey, locale.tag);
        }
        locales.append(locale);
    }

    for (auto it = derivedAliases.cbegin(); it != derivedAliases.cend(); ++it) {
        const auto tagIt = indexes.constFind(it.key());
        if (tagIt != indexes.cend() && locales.at(*tagIt).tag != it.value())
            return fail(error, QStringLiteral("Alias conflicts with locale tag %1").arg(it.key()));
    }

    QHash<QString, QString> aliases;
    const QJsonObject aliasObject = root.value(QStringLiteral("aliases")).toObject();
    for (auto it = aliasObject.begin(); it != aliasObject.end(); ++it) {
        if (it.key().startsWith(QLatin1Char('$')))
            continue;
        if (!it.value().isString())
            return fail(error, QStringLiteral("Alias target is not a string: %1").arg(it.key()));
        const QString aliasKey = keyFor(it.key());
        const QString target = it.value().toString();
        if (!indexes.contains(keyFor(target)) || derivedAliases.value(aliasKey) != target)
            return fail(error, QStringLiteral("Alias index is inconsistent for %1").arg(it.key()));
        aliases.insert(aliasKey, target);
    }
    if (aliases.size() != derivedAliases.size())
        return fail(error, QStringLiteral("Alias index does not match locale entries"));

    const QString source = root.value(QStringLiteral("source_language")).toString();
    const QString defaultLocale = root.value(QStringLiteral("default_locale")).toString();
    if (!indexes.contains(keyFor(source)) || !indexes.contains(keyFor(defaultLocale)))
        return fail(error, QStringLiteral("Source or default locale is not registered"));

    for (const Locale &locale : locales) {
        if (!locale.fallback.isEmpty() && !indexes.contains(keyFor(locale.fallback)))
            return fail(error, QStringLiteral("Unknown fallback for %1").arg(locale.tag));
    }
    for (const Locale &locale : locales) {
        QSet<QString> visited;
        QString current = locale.tag;
        while (!current.isEmpty()) {
            const QString currentKey = keyFor(current);
            if (visited.contains(currentKey))
                return fail(error, QStringLiteral("Fallback cycle at %1").arg(locale.tag));
            visited.insert(currentKey);
            const Locale &entry = locales.at(indexes.value(currentKey));
            current = entry.fallback;
        }
    }

    const Locale &sourceEntry = locales.at(indexes.value(keyFor(source)));
    if (sourceEntry.state != QStringLiteral("enabled") || !sourceEntry.fallback.isEmpty())
        return fail(error, QStringLiteral("Source locale must be enabled with no fallback"));

    m_sourceLanguage = source;
    m_defaultLocale = defaultLocale;
    m_locales = std::move(locales);
    m_tagIndexes = std::move(indexes);
    m_aliases = std::move(aliases);
    if (error)
        error->clear();
    return true;
}

const LocaleRegistry::Locale *LocaleRegistry::find(const QString &tag) const
{
    const auto it = m_tagIndexes.constFind(keyFor(tag));
    return it == m_tagIndexes.cend() ? nullptr : &m_locales.at(*it);
}

QString LocaleRegistry::canonicalTag(const QString &requested) const
{
    QString candidate = keyFor(requested);
    if (candidate.isEmpty())
        return {};

    while (!candidate.isEmpty()) {
        const auto tagIt = m_tagIndexes.constFind(candidate);
        if (tagIt != m_tagIndexes.cend())
            return m_locales.at(*tagIt).tag;

        const auto aliasIt = m_aliases.constFind(candidate);
        if (aliasIt != m_aliases.cend())
            return *aliasIt;

        const qsizetype separator = candidate.lastIndexOf(QLatin1Char('-'));
        if (separator < 0)
            break;
        candidate.truncate(separator);
    }

    return {};
}

bool LocaleRegistry::isAvailable(const QString &tag) const
{
    const Locale *locale = find(tag);
    return locale && (locale->state == QStringLiteral("enabled")
                      || (m_includeInternal && locale->state == QStringLiteral("internal")));
}

QString LocaleRegistry::resolveKnown(const QString &canonical) const
{
    QString current = canonical;
    QSet<QString> visited;
    while (!current.isEmpty() && !visited.contains(keyFor(current))) {
        visited.insert(keyFor(current));
        if (isAvailable(current))
            return find(current)->tag;
        const Locale *locale = find(current);
        current = locale ? locale->fallback : QString();
    }
    return isAvailable(m_sourceLanguage) ? m_sourceLanguage : QString();
}

QString LocaleRegistry::resolveAvailable(const QString &requested) const
{
    const QString canonical = canonicalTag(requested);
    return canonical.isEmpty() ? resolveKnown(m_sourceLanguage) : resolveKnown(canonical);
}

QString LocaleRegistry::resolveSystem(const QStringList &uiLanguages) const
{
    for (const QString &candidate : uiLanguages) {
        const QString canonical = canonicalTag(candidate);
        if (canonical.isEmpty())
            continue;
        if (isAvailable(canonical))
            return canonical;

        const QString fallback = resolveKnown(canonical);
        if (!fallback.isEmpty() && fallback != m_sourceLanguage)
            return fallback;
    }
    return resolveKnown(m_defaultLocale);
}

QString LocaleRegistry::catalogName(const QString &tag) const
{
    const Locale *locale = find(tag);
    return locale ? locale->catalog : QString();
}

Qt::LayoutDirection LocaleRegistry::layoutDirection(const QString &tag) const
{
    const Locale *locale = find(tag);
    return locale && locale->direction == QStringLiteral("rtl")
        ? Qt::RightToLeft : Qt::LeftToRight;
}

QVariantList LocaleRegistry::availableLanguages() const
{
    QVariantList result;
    for (const Locale &locale : m_locales) {
        if (!isAvailable(locale.tag))
            continue;
        result.append(QVariantMap{
            {QStringLiteral("tag"), locale.tag},
            {QStringLiteral("nativeName"), locale.nativeName},
            {QStringLiteral("englishName"), locale.englishName},
            {QStringLiteral("direction"), locale.direction},
            {QStringLiteral("tier"), locale.tier},
        });
    }
    return result;
}
