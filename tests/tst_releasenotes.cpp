#include "app/ReleaseNotes.h"
#include "localization/LocaleRegistry.h"

#include <QCryptographicHash>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTest>

namespace
{
struct BundleFixture
{
    LocaleRegistry registry{false};
    QByteArray index;
    QHash<QString, QByteArray> bundles;

    bool load(QString *error)
    {
        if (!registry.load(QStringLiteral(GAMEHQ_LOCALE_MANIFEST_FILE), error))
            return false;
        QFile indexFile(QStringLiteral(GAMEHQ_RELEASE_NOTES_GENERATED_DIR)
                        + QStringLiteral("/release-notes.index.json"));
        if (!indexFile.open(QIODevice::ReadOnly)) {
            *error = indexFile.errorString();
            return false;
        }
        index = indexFile.readAll();
        const QJsonArray entries = QJsonDocument::fromJson(index).object()
                                       .value(QStringLiteral("bundles")).toArray();
        for (const QJsonValue &value : entries) {
            const QString filename = value.toObject().value(QStringLiteral("filename")).toString();
            QFile file(QStringLiteral(GAMEHQ_RELEASE_NOTES_GENERATED_DIR)
                       + QLatin1Char('/') + filename);
            if (!file.open(QIODevice::ReadOnly)) {
                *error = file.errorString();
                return false;
            }
            bundles.insert(filename, file.readAll());
        }
        return true;
    }

    ReleaseNotes notes(const QString &locale, QString *error = nullptr) const
    {
        return ReleaseNotes::loadVerifiedBundle(
            index, locale, registry,
            [this](const QString &filename) { return bundles.value(filename); }, error);
    }

    void replaceBundle(const QString &locale, const QJsonObject &bundle)
    {
        replaceBundleBytes(locale, QJsonDocument(bundle).toJson(QJsonDocument::Indented));
    }

    void replaceBundleBytes(const QString &locale, const QByteArray &bytes)
    {
        const QString filename = QStringLiteral("release-notes.%1.json").arg(locale);
        bundles[filename] = bytes;
        QJsonObject indexRoot = QJsonDocument::fromJson(index).object();
        QJsonArray entries = indexRoot.value(QStringLiteral("bundles")).toArray();
        for (qsizetype i = 0; i < entries.size(); ++i) {
            QJsonObject entry = entries.at(i).toObject();
            if (entry.value(QStringLiteral("locale")).toString() != locale)
                continue;
            entry.insert(QStringLiteral("size"), bytes.size());
            entry.insert(QStringLiteral("sha256"), QString::fromLatin1(
                QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()));
            entries.replace(i, entry);
        }
        indexRoot.insert(QStringLiteral("bundles"), entries);
        index = QJsonDocument(indexRoot).toJson(QJsonDocument::Indented);
    }
};

QStringList availableTags(const LocaleRegistry &registry)
{
    QStringList result;
    for (const QVariant &entry : registry.availableLanguages())
        result.append(entry.toMap().value(QStringLiteral("tag")).toString());
    return result;
}
}

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
    void loadsEveryCanonicalLocaleBundle();
    void resolvesAliasesAndReloadsWithoutStaleLocaleState();
    void fallsBackWhollyForMissingOrInvalidLocalizedBundles();
    void rejectsWrongBundleIdentitySizeHashAndStructure();
    void localeDatesAndOfflineHistorySurviveSwitches();
    void exposesTheFrozenReleasedHistoryWithoutTheLegacySource();
    void structuresGitHubMarkdownWithoutActiveContent();
    void rejectsDateLessDraftDocumentsFromTheIndex();
    void rejectsMalformedIndexRecords();
    void fallsBackForCorruptLocalizedBytesAndFailsClosedForCorruptEnglish();
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

void ReleaseNotesTest::loadsEveryCanonicalLocaleBundle()
{
    BundleFixture fixture;
    QString error;
    QVERIFY2(fixture.load(&error), qPrintable(error));
    const QStringList tags = availableTags(fixture.registry);
    QCOMPARE(tags.size(), 16);
    for (const QString &tag : tags) {
        const ReleaseNotes notes = fixture.notes(tag, &error);
        QVERIFY2(notes.isValid(), qPrintable(tag + QStringLiteral(": ") + error));
        QCOMPARE(notes.locale(), tag);
        QCOMPARE(notes.version(), QStringLiteral("0.7.6"));
        QCOMPARE(notes.releases().size(), 4);
    }
}

void ReleaseNotesTest::resolvesAliasesAndReloadsWithoutStaleLocaleState()
{
    BundleFixture fixture;
    QString error;
    QVERIFY2(fixture.load(&error), qPrintable(error));
    const QList<QPair<QString, QString>> switches{
        {QStringLiteral("en-US"), QStringLiteral("en-US")},
        {QStringLiteral("pl"), QStringLiteral("pl-PL")},
        {QStringLiteral("zh-TW"), QStringLiteral("zh-Hant")},
        {QStringLiteral("th"), QStringLiteral("th-TH")},
        {fixture.registry.resolveSystem({QStringLiteral("de-DE")}), QStringLiteral("de-DE")},
    };
    for (const auto &[requested, expected] : switches) {
        const ReleaseNotes notes = fixture.notes(requested, &error);
        QVERIFY2(notes.isValid(), qPrintable(error));
        QCOMPARE(notes.locale(), expected);
    }
    QCOMPARE(fixture.notes(QStringLiteral("es-MX")).locale(), QStringLiteral("es-419"));
    QCOMPARE(fixture.notes(QStringLiteral("pt")).locale(), QStringLiteral("pt-BR"));
}

void ReleaseNotesTest::fallsBackWhollyForMissingOrInvalidLocalizedBundles()
{
    BundleFixture fixture;
    QString error;
    QVERIFY2(fixture.load(&error), qPrintable(error));
    const QVariantList english = fixture.notes(QStringLiteral("en-US")).releases();

    fixture.bundles.remove(QStringLiteral("release-notes.pl-PL.json"));
    ReleaseNotes fallback = fixture.notes(QStringLiteral("pl-PL"), &error);
    QVERIFY2(fallback.isValid(), qPrintable(error));
    QCOMPARE(fallback.locale(), QStringLiteral("en-US"));
    QCOMPARE(fallback.releases(), english);

    fallback = fixture.notes(QStringLiteral("not-a-locale"), &error);
    QVERIFY2(fallback.isValid(), qPrintable(error));
    QCOMPARE(fallback.locale(), QStringLiteral("en-US"));
    QCOMPARE(fallback.releases(), english);
}

void ReleaseNotesTest::rejectsWrongBundleIdentitySizeHashAndStructure()
{
    BundleFixture original;
    QString error;
    QVERIFY2(original.load(&error), qPrintable(error));
    const QVariantList english = original.notes(QStringLiteral("en-US")).releases();
    const QString filename = QStringLiteral("release-notes.pl-PL.json");

    BundleFixture wrongSize;
    QVERIFY2(wrongSize.load(&error), qPrintable(error));
    QJsonObject sizeIndex = QJsonDocument::fromJson(wrongSize.index).object();
    QJsonArray sizeEntries = sizeIndex.value(QStringLiteral("bundles")).toArray();
    for (qsizetype i = 0; i < sizeEntries.size(); ++i) {
        QJsonObject entry = sizeEntries.at(i).toObject();
        if (entry.value(QStringLiteral("locale")).toString() == QStringLiteral("pl-PL")) {
            entry.insert(QStringLiteral("size"), entry.value(QStringLiteral("size")).toInt() + 1);
            sizeEntries.replace(i, entry);
        }
    }
    sizeIndex.insert(QStringLiteral("bundles"), sizeEntries);
    wrongSize.index = QJsonDocument(sizeIndex).toJson();
    QCOMPARE(wrongSize.notes(QStringLiteral("pl-PL"), &error).releases(), english);

    BundleFixture wrongHash;
    QVERIFY2(wrongHash.load(&error), qPrintable(error));
    QJsonObject hashIndex = QJsonDocument::fromJson(wrongHash.index).object();
    QJsonArray hashEntries = hashIndex.value(QStringLiteral("bundles")).toArray();
    for (qsizetype i = 0; i < hashEntries.size(); ++i) {
        QJsonObject entry = hashEntries.at(i).toObject();
        if (entry.value(QStringLiteral("locale")).toString() == QStringLiteral("pl-PL")) {
            entry.insert(QStringLiteral("sha256"), QString(64, QLatin1Char('0')));
            hashEntries.replace(i, entry);
        }
    }
    hashIndex.insert(QStringLiteral("bundles"), hashEntries);
    wrongHash.index = QJsonDocument(hashIndex).toJson();
    QCOMPARE(wrongHash.notes(QStringLiteral("pl-PL"), &error).releases(), english);

    const auto verifyMutatedBundleFallsBack = [&](auto mutate) {
        BundleFixture fixture;
        QVERIFY2(fixture.load(&error), qPrintable(error));
        QJsonObject bundle = QJsonDocument::fromJson(fixture.bundles.value(filename)).object();
        mutate(bundle);
        fixture.replaceBundle(QStringLiteral("pl-PL"), bundle);
        const ReleaseNotes fallback = fixture.notes(QStringLiteral("pl-PL"), &error);
        QVERIFY2(fallback.isValid(), qPrintable(error));
        QCOMPARE(fallback.locale(), QStringLiteral("en-US"));
        QCOMPARE(fallback.releases(), english);
    };

    verifyMutatedBundleFallsBack([](QJsonObject &bundle) {
        QJsonObject metadata = bundle.value(QStringLiteral("_meta")).toObject();
        metadata.insert(QStringLiteral("schema_version"), 1);
        bundle.insert(QStringLiteral("_meta"), metadata);
    });
    verifyMutatedBundleFallsBack([](QJsonObject &bundle) {
        bundle.insert(QStringLiteral("version"), QStringLiteral("9.9.9"));
    });
    verifyMutatedBundleFallsBack([](QJsonObject &bundle) {
        QJsonObject metadata = bundle.value(QStringLiteral("_meta")).toObject();
        metadata.insert(QStringLiteral("requested_locale"), QStringLiteral("de-DE"));
        bundle.insert(QStringLiteral("_meta"), metadata);
    });
    verifyMutatedBundleFallsBack([](QJsonObject &bundle) {
        QJsonArray sections = bundle.value(QStringLiteral("sections")).toArray();
        QJsonObject section = sections.first().toObject();
        section.remove(QStringLiteral("items"));
        sections.replace(0, section);
        bundle.insert(QStringLiteral("sections"), sections);
    });
    verifyMutatedBundleFallsBack([](QJsonObject &bundle) {
        QJsonArray sections = bundle.value(QStringLiteral("sections")).toArray();
        QJsonObject section = sections.first().toObject();
        QJsonArray items = section.value(QStringLiteral("items")).toArray();
        items.replace(0, QStringLiteral("CZĘŚCIOWE POLSKIE POLE"));
        section.insert(QStringLiteral("items"), items);
        sections.replace(0, section);
        bundle.insert(QStringLiteral("sections"), sections);
        QJsonObject metadata = bundle.value(QStringLiteral("_meta")).toObject();
        QJsonArray documents = metadata.value(QStringLiteral("documents")).toArray();
        QJsonObject document = documents.first().toObject();
        document.insert(QStringLiteral("source_integrity"), QStringLiteral("sha256:")
                        + QString(64, QLatin1Char('0')));
        documents.replace(0, document);
        metadata.insert(QStringLiteral("documents"), documents);
        bundle.insert(QStringLiteral("_meta"), metadata);
    });
}

void ReleaseNotesTest::localeDatesAndOfflineHistorySurviveSwitches()
{
    BundleFixture fixture;
    QString error;
    QVERIFY2(fixture.load(&error), qPrintable(error));
    QSet<QString> dates;
    for (const QString &tag : {QStringLiteral("en-US"), QStringLiteral("pl-PL"),
                               QStringLiteral("zh-Hant"), QStringLiteral("th-TH")}) {
        const ReleaseNotes notes = fixture.notes(tag, &error);
        QVERIFY2(notes.isValid(), qPrintable(error));
        QCOMPARE(notes.releases().size(), 4);
        dates.insert(notes.releases().first().toMap().value(QStringLiteral("date")).toString());
    }
    QVERIFY(dates.size() >= 3);
}

void ReleaseNotesTest::exposesTheFrozenReleasedHistoryWithoutTheLegacySource()
{
    // The retired assets/release-notes.json is gone. Every historical release it
    // used to carry must still reach the runtime through the versioned source's
    // generated bundles, with its version, date, section order and item text
    // unchanged.
    BundleFixture fixture;
    QString error;
    QVERIFY2(fixture.load(&error), qPrintable(error));
    QVERIFY(!QFile::exists(QStringLiteral(":/release-notes/release-notes.json")));

    const ReleaseNotes notes = fixture.notes(QStringLiteral("en-US"), &error);
    QVERIFY2(notes.isValid(), qPrintable(error));
    const QVariantList releases = notes.releases();
    QCOMPARE(releases.size(), 4);

    const QStringList expected{QStringLiteral("0.7.6"), QStringLiteral("0.7.5"),
                               QStringLiteral("0.7.4"), QStringLiteral("0.7.3")};
    for (int index = 0; index < expected.size(); ++index) {
        const QVariantMap release = releases.at(index).toMap();
        QCOMPARE(release.value(QStringLiteral("version")).toString(), expected.at(index));
        QVERIFY2(!release.value(QStringLiteral("date")).toString().isEmpty(),
                 qPrintable(expected.at(index)));
        const QVariantList sections = release.value(QStringLiteral("sections")).toList();
        QVERIFY2(!sections.isEmpty(), qPrintable(expected.at(index)));
        for (const QVariant &value : sections) {
            const QVariantMap section = value.toMap();
            QVERIFY(!section.value(QStringLiteral("title")).toString().isEmpty());
            QVERIFY(!section.value(QStringLiteral("items")).toList().isEmpty());
        }
    }

    const QVariantList current = releases.first().toMap()
                                     .value(QStringLiteral("sections")).toList();
    QCOMPARE(current.size(), 2);
    QCOMPARE(current.at(0).toMap().value(QStringLiteral("title")).toString(),
             QStringLiteral("Fixed"));
    QCOMPARE(current.at(0).toMap().value(QStringLiteral("items")).toList().size(), 3);
    QCOMPARE(current.at(1).toMap().value(QStringLiteral("title")).toString(),
             QStringLiteral("Diagnostics / Reliability"));
    QCOMPARE(current.at(1).toMap().value(QStringLiteral("items")).toList().size(), 1);
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

void ReleaseNotesTest::rejectsDateLessDraftDocumentsFromTheIndex()
{
    BundleFixture fixture;
    QString error;
    QVERIFY2(fixture.load(&error), qPrintable(error));

    QJsonObject root = QJsonDocument::fromJson(fixture.index).object();
    QJsonArray documents = root.value(QStringLiteral("documents")).toArray();
    QJsonObject current = documents.first().toObject();
    current.insert(QStringLiteral("date"), QJsonValue::Null);
    documents.replace(0, current);
    root.insert(QStringLiteral("documents"), documents);
    fixture.index = QJsonDocument(root).toJson();

    QVERIFY(!fixture.notes(QStringLiteral("en-US"), &error).isValid());
    QVERIFY2(error.contains(QStringLiteral("document index is malformed")), qPrintable(error));
}

void ReleaseNotesTest::rejectsMalformedIndexRecords()
{
    const auto verifyRejected = [](auto mutate) {
        BundleFixture fixture;
        QString error;
        QVERIFY2(fixture.load(&error), qPrintable(error));
        QJsonObject root = QJsonDocument::fromJson(fixture.index).object();
        mutate(root);
        fixture.index = QJsonDocument(root).toJson();
        QVERIFY(!fixture.notes(QStringLiteral("pl-PL"), &error).isValid());
        QVERIFY2(!error.isEmpty(), "Malformed index must provide a diagnostic.");
    };

    verifyRejected([](QJsonObject &root) {
        root.insert(QStringLiteral("schema_version"), 1);
    });
    verifyRejected([](QJsonObject &root) {
        root.insert(QStringLiteral("current_version"), QStringLiteral("0.7.7"));
    });
    verifyRejected([](QJsonObject &root) {
        QJsonArray documents = root.value(QStringLiteral("documents")).toArray();
        QJsonObject duplicate = documents.at(1).toObject();
        duplicate.insert(QStringLiteral("version"),
                         documents.first().toObject().value(QStringLiteral("version")));
        documents.replace(1, duplicate);
        root.insert(QStringLiteral("documents"), documents);
    });
    verifyRejected([](QJsonObject &root) {
        QJsonArray bundles = root.value(QStringLiteral("bundles")).toArray();
        bundles.removeLast();
        root.insert(QStringLiteral("bundles"), bundles);
    });
    verifyRejected([](QJsonObject &root) {
        QJsonArray bundles = root.value(QStringLiteral("bundles")).toArray();
        QJsonObject pseudo = bundles.first().toObject();
        pseudo.insert(QStringLiteral("locale"), QStringLiteral("en-XA"));
        pseudo.insert(QStringLiteral("filename"), QStringLiteral("release-notes.en-XA.json"));
        bundles.replace(0, pseudo);
        root.insert(QStringLiteral("bundles"), bundles);
    });
}

void ReleaseNotesTest::fallsBackForCorruptLocalizedBytesAndFailsClosedForCorruptEnglish()
{
    BundleFixture fixture;
    QString error;
    QVERIFY2(fixture.load(&error), qPrintable(error));
    const QVariantList english = fixture.notes(QStringLiteral("en-US")).releases();
    const QByteArray malformed{"{\"sections\":[\"unterminated\""};

    fixture.replaceBundleBytes(QStringLiteral("pl-PL"), malformed);
    const ReleaseNotes fallback = fixture.notes(QStringLiteral("pl-PL"), &error);
    QVERIFY2(fallback.isValid(), qPrintable(error));
    QCOMPARE(fallback.locale(), QStringLiteral("en-US"));
    QCOMPARE(fallback.releases(), english);
    QVERIFY2(error.contains(QStringLiteral("complete en-US fallback")), qPrintable(error));

    fixture.replaceBundleBytes(QStringLiteral("en-US"), malformed);
    QVERIFY(!fixture.notes(QStringLiteral("pl-PL"), &error).isValid());
    QVERIFY2(error.contains(QStringLiteral("English fallback also failed")), qPrintable(error));
}

QTEST_APPLESS_MAIN(ReleaseNotesTest)
#include "tst_releasenotes.moc"
