#include "app/ReleaseNotes.h"
#include "localization/LocaleRegistry.h"

#include <QCryptographicHash>
#include <QDate>
#include <QDebug>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QRegularExpression>
#include <QSet>
#include <QVariantMap>
#include <optional>

namespace
{
constexpr int kMaxSections = 8;
constexpr int kMaxItemsPerSection = 20;
constexpr int kMaxHistoricalReleases = 12;
constexpr int kMaxTitleLength = 80;
constexpr int kMaxItemLength = 1200;
constexpr int kMaxMarkdownLength = 64 * 1024;
constexpr int kMaxMarkdownBlocks = 128;
constexpr int kMaxMarkdownBlockLength = 2000;
constexpr int kBundleSchemaVersion = 2;
constexpr int kExpectedProductionLocales = 16;

struct BundleEntry
{
    QString locale;
    QString filename;
    qint64 size = 0;
    QByteArray sha256;
};

struct DocumentEntry
{
    QString version;
    QString date;
    QString sourceIntegrity;
};

struct BundleIndex
{
    QString sourceLocale;
    QString currentVersion;
    QList<DocumentEntry> documents;
    QList<BundleEntry> bundles;
};

void setError(QString* error, const QString& value)
{
    if (error)
        *error = value;
}

bool exactKeys(const QJsonObject &object, std::initializer_list<QString> keys)
{
    if (object.size() != static_cast<qsizetype>(keys.size()))
        return false;
    for (const QString &key : keys) {
        if (!object.contains(key))
            return false;
    }
    return true;
}

bool validVersion(const QString &value)
{
    static const QRegularExpression pattern(QStringLiteral(R"(^\d+\.\d+\.\d+$)"));
    return pattern.match(value).hasMatch();
}

bool validIsoDate(const QString &value)
{
    static const QRegularExpression pattern(QStringLiteral(R"(^\d{4}-\d{2}-\d{2}$)"));
    return pattern.match(value).hasMatch() && QDate::fromString(value, Qt::ISODate).isValid();
}

bool validSourceIntegrity(const QString &value)
{
    static const QRegularExpression pattern(QStringLiteral(R"(^sha256:[0-9a-f]{64}$)"));
    return pattern.match(value).hasMatch();
}

std::optional<BundleIndex> parseBundleIndex(const QByteArray &json,
                                            const LocaleRegistry &registry,
                                            QString *error)
{
    QJsonParseError parseError{};
    const QJsonDocument parsed = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !parsed.isObject()) {
        setError(error, QStringLiteral("Release-note bundle index is invalid JSON."));
        return std::nullopt;
    }
    const QJsonObject root = parsed.object();
    if (!exactKeys(root, {QStringLiteral("schema_version"),
                          QStringLiteral("source_locale"),
                          QStringLiteral("current_version"),
                          QStringLiteral("history_limit"),
                          QStringLiteral("documents"),
                          QStringLiteral("bundles")})
        || root.value(QStringLiteral("schema_version")).toInt(-1) != kBundleSchemaVersion
        || root.value(QStringLiteral("source_locale")).toString() != registry.sourceLanguage()
        || !validVersion(root.value(QStringLiteral("current_version")).toString())
        || root.value(QStringLiteral("history_limit")).toInt(-1) != kMaxHistoricalReleases
        || !root.value(QStringLiteral("documents")).isArray()
        || !root.value(QStringLiteral("bundles")).isArray()) {
        setError(error, QStringLiteral("Release-note bundle index metadata is invalid."));
        return std::nullopt;
    }

    BundleIndex result;
    result.sourceLocale = root.value(QStringLiteral("source_locale")).toString();
    result.currentVersion = root.value(QStringLiteral("current_version")).toString();
    QSet<QString> versions;
    for (const QJsonValue &value : root.value(QStringLiteral("documents")).toArray()) {
        if (!value.isObject()) {
            setError(error, QStringLiteral("Release-note document index is malformed."));
            return std::nullopt;
        }
        const QJsonObject object = value.toObject();
        const QString version = object.value(QStringLiteral("version")).toString();
        const QString date = object.value(QStringLiteral("date")).toString();
        const QString integrity = object.value(QStringLiteral("source_integrity")).toString();
        if (!exactKeys(object, {QStringLiteral("version"), QStringLiteral("date"),
                                QStringLiteral("source_integrity")})
            || !validVersion(version) || !validIsoDate(date)
            || !validSourceIntegrity(integrity) || versions.contains(version)) {
            setError(error, QStringLiteral("Release-note document index is malformed."));
            return std::nullopt;
        }
        versions.insert(version);
        result.documents.append({version, date, integrity});
    }
    if (result.documents.isEmpty()
        || result.documents.size() > kMaxHistoricalReleases + 1
        || result.documents.first().version != result.currentVersion) {
        setError(error, QStringLiteral("Release-note document order or history limit is invalid."));
        return std::nullopt;
    }

    QSet<QString> locales;
    for (const QJsonValue &value : root.value(QStringLiteral("bundles")).toArray()) {
        if (!value.isObject()) {
            setError(error, QStringLiteral("Release-note bundle record is malformed."));
            return std::nullopt;
        }
        const QJsonObject object = value.toObject();
        const QString locale = object.value(QStringLiteral("locale")).toString();
        const QString filename = object.value(QStringLiteral("filename")).toString();
        const QString hash = object.value(QStringLiteral("sha256")).toString();
        const qint64 size = object.value(QStringLiteral("size")).toInteger(-1);
        const QString canonical = registry.canonicalTag(locale);
        if (!exactKeys(object, {QStringLiteral("locale"), QStringLiteral("filename"),
                                QStringLiteral("size"), QStringLiteral("sha256")})
            || canonical != locale || !registry.isAvailable(locale)
            || locale == QStringLiteral("en-XA") || locale == QStringLiteral("ar-XB")
            || locales.contains(locale)
            || filename != QStringLiteral("release-notes.%1.json").arg(locale)
            || size <= 0 || size > 1024 * 1024
            || !QRegularExpression(QStringLiteral(R"(^[0-9a-f]{64}$)"))
                    .match(hash).hasMatch()) {
            setError(error, QStringLiteral("Release-note bundle record is malformed."));
            return std::nullopt;
        }
        locales.insert(locale);
        result.bundles.append({locale, filename, size, hash.toLatin1()});
    }
    if (result.bundles.size() != kExpectedProductionLocales
        || !locales.contains(result.sourceLocale)) {
        setError(error, QStringLiteral("Release-note index must cover sixteen production locales."));
        return std::nullopt;
    }
    return result;
}

const BundleEntry *findBundle(const BundleIndex &index, const QString &locale)
{
    for (const BundleEntry &entry : index.bundles) {
        if (entry.locale == locale)
            return &entry;
    }
    return nullptr;
}

bool validateBundleMetadata(const QJsonObject &root, const QString &locale,
                            const BundleIndex &index, const ReleaseNotes &notes,
                            QString *error)
{
    const QJsonObject metadata = root.value(QStringLiteral("_meta")).toObject();
    if (!exactKeys(metadata, {QStringLiteral("schema_version"),
                              QStringLiteral("requested_locale"),
                              QStringLiteral("source_locale"),
                              QStringLiteral("documents")})
        || metadata.value(QStringLiteral("schema_version")).toInt(-1) != kBundleSchemaVersion
        || metadata.value(QStringLiteral("requested_locale")).toString() != locale
        || metadata.value(QStringLiteral("source_locale")).toString() != index.sourceLocale
        || !metadata.value(QStringLiteral("documents")).isArray()
        || notes.version() != index.currentVersion) {
        setError(error, QStringLiteral("Release-note bundle identity metadata is invalid."));
        return false;
    }

    QList<QJsonObject> releases{root};
    const QJsonValue historyValue = root.value(QStringLiteral("history"));
    if (!historyValue.isArray()) {
        setError(error, QStringLiteral("Release-note bundle history is missing."));
        return false;
    }
    for (const QJsonValue &value : historyValue.toArray()) {
        if (!value.isObject()) {
            setError(error, QStringLiteral("Release-note bundle history is malformed."));
            return false;
        }
        releases.append(value.toObject());
    }
    const QJsonArray documentMetadata = metadata.value(QStringLiteral("documents")).toArray();
    if (releases.size() != index.documents.size()
        || documentMetadata.size() != index.documents.size()
        || notes.releases().size() != index.documents.size()) {
        setError(error, QStringLiteral("Release-note bundle structure does not match its index."));
        return false;
    }
    for (qsizetype i = 0; i < index.documents.size(); ++i) {
        const DocumentEntry &expected = index.documents.at(i);
        const QJsonObject release = releases.at(i);
        const QJsonObject document = documentMetadata.at(i).toObject();
        const QString resolvedLocale = document.value(QStringLiteral("resolved_locale")).toString();
        const bool fallback = document.value(QStringLiteral("fallback")).toBool();
        const QJsonValue fallbackReason = document.value(QStringLiteral("fallback_reason"));
        if (!exactKeys(document, {QStringLiteral("version"),
                                  QStringLiteral("source_integrity"),
                                  QStringLiteral("resolved_locale"),
                                  QStringLiteral("fallback"),
                                  QStringLiteral("fallback_reason")})
            || release.value(QStringLiteral("version")).toString() != expected.version
            || release.value(QStringLiteral("date")).toString() != expected.date
            || document.value(QStringLiteral("version")).toString() != expected.version
            || document.value(QStringLiteral("source_integrity")).toString()
                != expected.sourceIntegrity
            || (resolvedLocale != locale && resolvedLocale != index.sourceLocale)
            || fallback != (resolvedLocale != locale)
            || (fallback && !fallbackReason.isString())
            || (!fallback && !fallbackReason.isNull())) {
            setError(error, QStringLiteral("Release-note bundle document integrity is invalid."));
            return false;
        }
    }
    return true;
}

bool parseSections(const QJsonValue& sectionsValue, QVariantList* parsedSections,
                   QString* error)
{
    if (!sectionsValue.isArray()) {
        setError(error, QStringLiteral("Release notes sections must be an array."));
        return false;
    }
    const QJsonArray sections = sectionsValue.toArray();
    if (sections.isEmpty() || sections.size() > kMaxSections) {
        setError(error, QStringLiteral("Release notes contain an invalid section count."));
        return false;
    }

    QVariantList result;
    for (const QJsonValue& sectionValue : sections) {
        if (!sectionValue.isObject()) {
            setError(error, QStringLiteral("Every release-notes section must be an object."));
            return false;
        }
        const QJsonObject section = sectionValue.toObject();
        const QString title = section.value(QStringLiteral("title")).toString().trimmed();
        const QJsonValue itemsValue = section.value(QStringLiteral("items"));
        if (title.isEmpty() || title.size() > kMaxTitleLength || !itemsValue.isArray()) {
            setError(error, QStringLiteral("A release-notes section is malformed."));
            return false;
        }

        const QJsonArray items = itemsValue.toArray();
        if (items.isEmpty() || items.size() > kMaxItemsPerSection) {
            setError(error, QStringLiteral("A release-notes section has an invalid item count."));
            return false;
        }
        QStringList parsedItems;
        for (const QJsonValue& itemValue : items) {
            if (!itemValue.isString()) {
                setError(error, QStringLiteral("Every release-note item must be plain text."));
                return false;
            }
            const QString item = itemValue.toString().trimmed();
            if (item.isEmpty() || item.size() > kMaxItemLength) {
                setError(error, QStringLiteral("A release-note item has an invalid length."));
                return false;
            }
            parsedItems.append(item);
        }

        result.append(QVariantMap{
            { QStringLiteral("title"), title },
            { QStringLiteral("items"), parsedItems },
        });
    }

    *parsedSections = result;
    return true;
}

bool parseRelease(const QJsonObject& object, const QLocale& locale,
                  QVariantMap* parsedRelease, QString* error)
{
    const QString version = object.value(QStringLiteral("version")).toString().trimmed();
    static const QRegularExpression versionPattern(QStringLiteral(R"(^\d+\.\d+\.\d+$)"));
    if (!versionPattern.match(version).hasMatch()) {
        setError(error, QStringLiteral("Release notes contain an invalid version."));
        return false;
    }

    QVariantList sections;
    if (!parseSections(object.value(QStringLiteral("sections")), &sections, error))
        return false;

    QString displayDate;
    const QString dateText = object.value(QStringLiteral("date")).toString().trimmed();
    if (!dateText.isEmpty()) {
        static const QRegularExpression datePattern(QStringLiteral(R"(^\d{4}-\d{2}-\d{2}$)"));
        const QDate date = QDate::fromString(dateText, Qt::ISODate);
        if (!datePattern.match(dateText).hasMatch() || !date.isValid()) {
            setError(error, QStringLiteral("Release notes contain an invalid date."));
            return false;
        }
        displayDate = locale.toString(date, QLocale::ShortFormat);
    }

    *parsedRelease = {
        { QStringLiteral("version"), version },
        { QStringLiteral("date"), displayDate },
        { QStringLiteral("sections"), sections },
    };
    return true;
}

QString plainInlineMarkdown(QString text)
{
    static const QRegularExpression image(
        QStringLiteral(R"(!\[([^\]]*)\]\([^)]+\))"));
    static const QRegularExpression link(
        QStringLiteral(R"(\[([^\]]+)\]\([^)]+\))"));
    static const QRegularExpression strongAsterisk(
        QStringLiteral(R"(\*\*([^*]+)\*\*)"));
    static const QRegularExpression strongUnderscore(
        QStringLiteral(R"(__([^_]+)__)"));
    static const QRegularExpression code(
        QStringLiteral(R"(`([^`]+)`)"));

    text.replace(image, QStringLiteral("\\1"));
    text.replace(link, QStringLiteral("\\1"));
    text.replace(strongAsterisk, QStringLiteral("\\1"));
    text.replace(strongUnderscore, QStringLiteral("\\1"));
    text.replace(code, QStringLiteral("\\1"));
    return text.trimmed().left(kMaxMarkdownBlockLength);
}

QVariantMap markdownBlock(const QString& kind, const QString& rawText)
{
    const QString text = plainInlineMarkdown(rawText);
    QString lead;
    QString body = text;

    if (kind == QStringLiteral("bullet")) {
        static const QRegularExpression strongLead(
            QStringLiteral(R"(^(?:\*\*|__)(.+?)(?:\*\*|__)\s*(.*)$)"));
        const QRegularExpressionMatch match = strongLead.match(rawText.trimmed());
        if (match.hasMatch()) {
            lead = plainInlineMarkdown(match.captured(1));
            body = plainInlineMarkdown(match.captured(2));
        }
    }

    return {
        { QStringLiteral("kind"), kind },
        { QStringLiteral("text"), text },
        { QStringLiteral("lead"), lead },
        { QStringLiteral("body"), body },
    };
}
}

ReleaseNotes ReleaseNotes::fromJson(const QByteArray& json, QString* error)
{
    return fromJson(json, QLocale(), error);
}

ReleaseNotes ReleaseNotes::fromJson(const QByteArray& json, const QLocale& locale,
                                    QString* error)
{
    ReleaseNotes result;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        setError(error, QStringLiteral("Release notes are not a valid JSON object."));
        return result;
    }

    const QJsonObject root = document.object();
    QVariantMap currentRelease;
    if (!parseRelease(root, locale, &currentRelease, error))
        return result;

    QVariantList releases{currentRelease};
    QSet<QString> versions{currentRelease.value(QStringLiteral("version")).toString()};
    const QJsonValue historyValue = root.value(QStringLiteral("history"));
    if (!historyValue.isUndefined()) {
        if (!historyValue.isArray()
            || historyValue.toArray().size() > kMaxHistoricalReleases) {
            setError(error, QStringLiteral("Release notes contain an invalid history."));
            return {};
        }
        for (const QJsonValue& historicalValue : historyValue.toArray()) {
            if (!historicalValue.isObject()) {
                setError(error, QStringLiteral("Every historical release must be an object."));
                return {};
            }
            QVariantMap historicalRelease;
            if (!parseRelease(historicalValue.toObject(), locale, &historicalRelease, error))
                return {};
            const QString historicalVersion =
                historicalRelease.value(QStringLiteral("version")).toString();
            if (versions.contains(historicalVersion)) {
                setError(error, QStringLiteral("Release-note versions must be unique."));
                return {};
            }
            versions.insert(historicalVersion);
            releases.append(historicalRelease);
        }
    }

    result.m_version = currentRelease.value(QStringLiteral("version")).toString();
    result.m_locale = locale.bcp47Name();
    result.m_sections = currentRelease.value(QStringLiteral("sections")).toList();
    result.m_releases = releases;
    setError(error, {});
    return result;
}

ReleaseNotes ReleaseNotes::loadBundled()
{
    QFile file(QStringLiteral(":/release-notes/release-notes.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning("Could not open bundled release notes");
        return {};
    }

    QString error;
    ReleaseNotes notes = fromJson(file.readAll(), &error);
    if (!notes.isValid())
        qWarning("Could not parse bundled release notes: %s", qPrintable(error));
    return notes;
}

ReleaseNotes ReleaseNotes::loadBundled(const QString &requestedLocale,
                                       const LocaleRegistry &registry,
                                       QString *error)
{
    QFile indexFile(QStringLiteral(":/release-notes/generated/release-notes.index.json"));
    if (!indexFile.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("Could not open the release-note bundle index."));
        return {};
    }
    const BundleReader reader = [](const QString &filename) {
        QFile file(QStringLiteral(":/release-notes/generated/%1").arg(filename));
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
    };
    return loadVerifiedBundle(indexFile.readAll(), requestedLocale, registry, reader, error);
}

ReleaseNotes ReleaseNotes::loadVerifiedBundle(const QByteArray &indexJson,
                                              const QString &requestedLocale,
                                              const LocaleRegistry &registry,
                                              const BundleReader &reader,
                                              QString *error)
{
    QString indexError;
    const std::optional<BundleIndex> index = parseBundleIndex(indexJson, registry, &indexError);
    if (!index) {
        setError(error, indexError);
        return {};
    }
    QString canonical = registry.resolveAvailable(requestedLocale);
    if (canonical.isEmpty())
        canonical = index->sourceLocale;

    const auto load = [&](const QString &locale, QString *loadError) -> ReleaseNotes {
        ReleaseNotes result;
        const BundleEntry *entry = findBundle(*index, locale);
        if (!entry) {
            setError(loadError, QStringLiteral("Release-note bundle is not indexed for %1.")
                                    .arg(locale));
            return result;
        }
        const QByteArray data = reader(entry->filename);
        if (data.size() != entry->size) {
            setError(loadError, QStringLiteral("Release-note bundle size mismatch for %1.")
                                    .arg(locale));
            return result;
        }
        const QByteArray actualHash = QCryptographicHash::hash(
            data, QCryptographicHash::Sha256).toHex();
        if (actualHash != entry->sha256) {
            setError(loadError, QStringLiteral("Release-note bundle hash mismatch for %1.")
                                    .arg(locale));
            return result;
        }

        QJsonParseError parseError{};
        const QJsonDocument parsed = QJsonDocument::fromJson(data, &parseError);
        if (parseError.error != QJsonParseError::NoError || !parsed.isObject()) {
            setError(loadError, QStringLiteral("Release-note bundle JSON is invalid for %1.")
                                    .arg(locale));
            return result;
        }
        result = fromJson(data, QLocale(locale), loadError);
        if (!result.isValid()
            || !validateBundleMetadata(parsed.object(), locale, *index, result, loadError)) {
            return {};
        }
        result.m_locale = locale;
        setError(loadError, {});
        return result;
    };

    QString localizedError;
    ReleaseNotes localized = load(canonical, &localizedError);
    if (localized.isValid()) {
        setError(error, {});
        return localized;
    }
    if (canonical == index->sourceLocale) {
        setError(error, localizedError);
        return {};
    }

    QString fallbackError;
    ReleaseNotes fallback = load(index->sourceLocale, &fallbackError);
    if (!fallback.isValid()) {
        setError(error, QStringLiteral("%1 English fallback also failed: %2")
                            .arg(localizedError, fallbackError));
        return {};
    }
    setError(error, QStringLiteral("%1 Loaded complete en-US fallback.")
                        .arg(localizedError));
    return fallback;
}

QVariantList ReleaseNotes::blocksFromMarkdown(const QString& markdown)
{
    if (markdown.isEmpty())
        return {};

    const QString bounded = markdown.left(kMaxMarkdownLength);
    const QStringList lines = bounded.split(QRegularExpression(QStringLiteral(R"(\r?\n)")));
    static const QRegularExpression heading(
        QStringLiteral(R"(^\s*#{1,6}\s+(.+?)\s*#*\s*$)"));
    static const QRegularExpression bullet(
        QStringLiteral(R"(^\s*(?:[-*+]|\d+[.)])\s+(.+)$)"));

    QVariantList blocks;
    QStringList paragraph;
    auto appendBlock = [&blocks](const QVariantMap& block) {
        if (blocks.size() < kMaxMarkdownBlocks
            && !block.value(QStringLiteral("text")).toString().isEmpty()) {
            blocks.append(block);
        }
    };
    auto flushParagraph = [&]() {
        if (paragraph.isEmpty())
            return;
        appendBlock(markdownBlock(QStringLiteral("paragraph"),
                                  paragraph.join(QLatin1Char(' '))));
        paragraph.clear();
    };

    for (const QString& rawLine : lines) {
        if (blocks.size() >= kMaxMarkdownBlocks)
            break;

        const QString trimmed = rawLine.trimmed();
        if (trimmed.isEmpty()) {
            flushParagraph();
            continue;
        }

        const QRegularExpressionMatch headingMatch = heading.match(rawLine);
        if (headingMatch.hasMatch()) {
            flushParagraph();
            appendBlock(markdownBlock(QStringLiteral("heading"),
                                      headingMatch.captured(1)));
            continue;
        }

        const QRegularExpressionMatch bulletMatch = bullet.match(rawLine);
        if (bulletMatch.hasMatch()) {
            flushParagraph();
            appendBlock(markdownBlock(QStringLiteral("bullet"),
                                      bulletMatch.captured(1)));
            continue;
        }

        paragraph.append(trimmed);
    }
    flushParagraph();
    return blocks;
}
