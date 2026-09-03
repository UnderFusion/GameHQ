#include "app/ReleaseNotes.h"

#include <QSet>
#include <QTest>

class ReleaseNotesTest : public QObject
{
    Q_OBJECT

private slots:
    void parsesStructuredPlainText();
    void parsesHistoricalReleases();
    void formatsDatesForTheEffectiveLocale();
    void rejectsNonStringItems();
    void rejectsInvalidVersion();
    void rejectsDuplicateReleaseVersions();
    void rejectsOversizedDocuments();
    void structuresGitHubMarkdownWithoutActiveContent();
};

void ReleaseNotesTest::parsesStructuredPlainText()
{
    const ReleaseNotes notes = ReleaseNotes::fromJson(R"({
        "version":"1.2.3",
        "sections":[{"title":"Added","items":["First item","Second item"]}]
    })", QLocale(QStringLiteral("en-US")));

    QVERIFY(notes.isValid());
    QCOMPARE(notes.version(), QStringLiteral("1.2.3"));
    QCOMPARE(notes.sections().size(), 1);
    QCOMPARE(notes.releases().size(), 1);
    const QVariantMap section = notes.sections().first().toMap();
    QCOMPARE(section.value(QStringLiteral("title")).toString(), QStringLiteral("Added"));
    QCOMPARE(section.value(QStringLiteral("items")).toStringList().size(), 2);
}

void ReleaseNotesTest::parsesHistoricalReleases()
{
    const ReleaseNotes notes = ReleaseNotes::fromJson(R"({
        "version":"1.2.3",
        "date":"2026-08-07",
        "sections":[{"title":"Fixed","items":["Current item"]}],
        "history":[{
            "version":"1.2.2",
            "date":"2026-08-05",
            "sections":[{"title":"Added","items":["Earlier item"]}]
        }]
    })", QLocale(QStringLiteral("en-US")));

    QVERIFY(notes.isValid());
    QCOMPARE(notes.releases().size(), 2);
    const QVariantMap earlier = notes.releases().at(1).toMap();
    QCOMPARE(earlier.value(QStringLiteral("version")).toString(), QStringLiteral("1.2.2"));
    QCOMPARE(earlier.value(QStringLiteral("date")).toString(),
             QLocale(QStringLiteral("en-US")).toString(QDate(2026, 8, 5),
                                                        QLocale::ShortFormat));
    QCOMPARE(earlier.value(QStringLiteral("sections")).toList().size(), 1);
}

void ReleaseNotesTest::formatsDatesForTheEffectiveLocale()
{
    const QByteArray json = R"({
        "version":"1.2.3",
        "date":"2026-09-03",
        "sections":[{"title":"Fixed","items":["Item"]}]
    })";
    QStringList renderedDates;
    for (const QString& tag : {QStringLiteral("en-US"), QStringLiteral("pl-PL"),
                               QStringLiteral("zh-Hans"), QStringLiteral("de-DE")}) {
        const QLocale locale(tag);
        const ReleaseNotes notes = ReleaseNotes::fromJson(json, locale);
        QVERIFY(notes.isValid());
        const QString rendered = notes.releases().first().toMap()
                                     .value(QStringLiteral("date")).toString();
        QCOMPARE(rendered, locale.toString(QDate(2026, 9, 3), QLocale::ShortFormat));
        renderedDates.append(rendered);
    }
    const QSet<QString> distinctDates(renderedDates.cbegin(), renderedDates.cend());
    QVERIFY(distinctDates.size() >= 3);
}

void ReleaseNotesTest::rejectsNonStringItems()
{
    const ReleaseNotes notes = ReleaseNotes::fromJson(R"({
        "version":"1.2.3",
        "sections":[{"title":"Added","items":[{"html":"<img src='file:///x'>"}]}]
    })");
    QVERIFY(!notes.isValid());
}

void ReleaseNotesTest::rejectsInvalidVersion()
{
    const ReleaseNotes notes = ReleaseNotes::fromJson(R"({
        "version":"v1.2.3-beta",
        "sections":[{"title":"Added","items":["Item"]}]
    })");
    QVERIFY(!notes.isValid());
}

void ReleaseNotesTest::rejectsDuplicateReleaseVersions()
{
    const ReleaseNotes notes = ReleaseNotes::fromJson(R"({
        "version":"1.2.3",
        "sections":[{"title":"Fixed","items":["Current item"]}],
        "history":[{
            "version":"1.2.3",
            "sections":[{"title":"Fixed","items":["Duplicate item"]}]
        }]
    })");
    QVERIFY(!notes.isValid());
}

void ReleaseNotesTest::rejectsOversizedDocuments()
{
    QByteArray json = R"({"version":"1.2.3","sections":[{"title":"Added","items":[)";
    for (int i = 0; i < 21; ++i) {
        if (i > 0)
            json += ',';
        json += R"("Item")";
    }
    json += R"(]}]})";
    QVERIFY(!ReleaseNotes::fromJson(json).isValid());
}

void ReleaseNotesTest::structuresGitHubMarkdownWithoutActiveContent()
{
    const QVariantList blocks = ReleaseNotes::blocksFromMarkdown(R"(
GameHQ maintenance update.

## Highlights

- **Automatic updater:** Installs safely.
- [Release page](https://example.com) and ![tracking image](https://example.com/x.png)
)");

    QCOMPARE(blocks.size(), 4);
    QCOMPARE(blocks.at(0).toMap().value(QStringLiteral("kind")).toString(),
             QStringLiteral("paragraph"));
    QCOMPARE(blocks.at(1).toMap().value(QStringLiteral("text")).toString(),
             QStringLiteral("Highlights"));

    const QVariantMap emphasized = blocks.at(2).toMap();
    QCOMPARE(emphasized.value(QStringLiteral("kind")).toString(), QStringLiteral("bullet"));
    QCOMPARE(emphasized.value(QStringLiteral("lead")).toString(),
             QStringLiteral("Automatic updater:"));
    QCOMPARE(emphasized.value(QStringLiteral("body")).toString(),
             QStringLiteral("Installs safely."));

    const QVariantMap sanitized = blocks.at(3).toMap();
    QCOMPARE(sanitized.value(QStringLiteral("text")).toString(),
             QStringLiteral("Release page and tracking image"));
    QVERIFY(!sanitized.value(QStringLiteral("text")).toString().contains(QStringLiteral("https")));
}

QTEST_APPLESS_MAIN(ReleaseNotesTest)
#include "tst_releasenotes.moc"
