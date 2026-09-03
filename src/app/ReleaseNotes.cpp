#include "app/ReleaseNotes.h"

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

void setError(QString* error, const QString& value)
{
    if (error)
        *error = value;
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
