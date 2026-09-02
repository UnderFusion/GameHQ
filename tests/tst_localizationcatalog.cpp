#include <QtTest>

#include <QCoreApplication>
#include <QDirIterator>
#include <QFile>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QTranslator>
#include <QXmlStreamReader>

namespace {

constexpr auto kLocalizedId = "gamehq.test.catalog.localized.message";
constexpr auto kFallbackId = "gamehq.test.catalog.fallback.message";

struct CatalogEntry {
    QString source;
    QString translation;
    bool unfinished = false;
};

struct LookupResult {
    bool englishLoaded = false;
    bool targetLoaded = false;
    QString text;
};

QHash<QString, CatalogEntry> readTsCatalog(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = file.errorString();
        return {};
    }

    QXmlStreamReader xml(&file);
    QHash<QString, CatalogEntry> entries;
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement() || xml.name() != u"message")
            continue;

        const QString id = xml.attributes().value(u"id").toString();
        CatalogEntry entry;
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isEndElement() && xml.name() == u"message")
                break;
            if (!xml.isStartElement())
                continue;
            if (xml.name() == u"source") {
                entry.source = xml.readElementText();
            } else if (xml.name() == u"translation") {
                entry.unfinished = xml.attributes().value(u"type") == u"unfinished";
                entry.translation = xml.readElementText();
            }
        }
        if (!id.isEmpty())
            entries.insert(id, entry);
    }

    if (xml.hasError())
        *error = xml.errorString();
    return entries;
}

QSet<QString> activeProductionIds()
{
    const QString sourceRoot = QStringLiteral(GAMEHQ_SOURCE_DIR "/src");
    const QStringList filters{QStringLiteral("*.cpp"), QStringLiteral("*.h"),
                              QStringLiteral("*.qml"), QStringLiteral("*.js")};
    const QRegularExpression pattern(
        QStringLiteral("(?:qsTrId|qtTrId|QT_TRID_NOOP)\\s*\\(\\s*\"([^\"]+)\""));

    QSet<QString> ids;
    QDirIterator files(sourceRoot, filters, QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        QFile file(files.next());
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const QString source = QString::fromUtf8(file.readAll());
        auto matches = pattern.globalMatch(source);
        while (matches.hasNext())
            ids.insert(matches.next().captured(1));
    }
    return ids;
}

LookupResult lookupFixture(const char *id)
{
    QTranslator english;
    QTranslator target;
    LookupResult result;
    result.englishLoaded = english.load(QStringLiteral(":/i18n/gamehq_test_en_US.qm"));
    result.targetLoaded = target.load(QStringLiteral(":/i18n/gamehq_test_pl_PL.qm"));

    if (result.englishLoaded)
        QCoreApplication::installTranslator(&english);
    if (result.targetLoaded)
        QCoreApplication::installTranslator(&target);
    result.text = qtTrId(id);
    if (result.targetLoaded)
        QCoreApplication::removeTranslator(&target);
    if (result.englishLoaded)
        QCoreApplication::removeTranslator(&english);
    return result;
}

} // namespace

class LocalizationCatalogTest : public QObject
{
    Q_OBJECT

private slots:
    void productionEnglishCatalogCoversActiveIds();
    void nonEnglishCatalogLoads();
    void targetMissFallsBackToEnglish();
    void publicIdsNeverReachTheRenderedText();
};

void LocalizationCatalogTest::productionEnglishCatalogCoversActiveIds()
{
    QString error;
    const auto catalog = readTsCatalog(
        QStringLiteral(GAMEHQ_SOURCE_DIR "/i18n/app/gamehq_en_US.ts"), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    const auto activeIds = activeProductionIds();
    for (const QString &id : activeIds) {
        QVERIFY2(id.startsWith(QStringLiteral("gamehq.")), qPrintable(id));
        QVERIFY2(catalog.contains(id), qPrintable(QStringLiteral("Missing en-US ID: ") + id));
        const auto &entry = catalog[id];
        QVERIFY2(!entry.unfinished, qPrintable(QStringLiteral("Unfinished en-US ID: ") + id));
        QVERIFY2(!entry.source.trimmed().isEmpty(), qPrintable(QStringLiteral("Missing source: ") + id));
        QVERIFY2(!entry.translation.trimmed().isEmpty(),
                 qPrintable(QStringLiteral("Missing en-US translation: ") + id));
        QVERIFY2(!entry.translation.startsWith(QStringLiteral("gamehq.")), qPrintable(id));
    }
}

void LocalizationCatalogTest::nonEnglishCatalogLoads()
{
    const LookupResult result = lookupFixture(kLocalizedId);
    QVERIFY(result.englishLoaded);
    QVERIFY(result.targetLoaded);
    QCOMPARE(result.text, QStringLiteral("Załadowano po polsku"));
}

void LocalizationCatalogTest::targetMissFallsBackToEnglish()
{
    const LookupResult result = lookupFixture(kFallbackId);
    QVERIFY(result.englishLoaded);
    QVERIFY(result.targetLoaded);
    QCOMPARE(result.text, QStringLiteral("English fallback"));
}

void LocalizationCatalogTest::publicIdsNeverReachTheRenderedText()
{
    for (const char *id : {kLocalizedId, kFallbackId}) {
        const LookupResult result = lookupFixture(id);
        QVERIFY(result.englishLoaded);
        QVERIFY(result.targetLoaded);
        QVERIFY2(!result.text.startsWith(QStringLiteral("gamehq.")), result.text.toUtf8().constData());
    }
}

QTEST_GUILESS_MAIN(LocalizationCatalogTest)
#include "tst_localizationcatalog.moc"
