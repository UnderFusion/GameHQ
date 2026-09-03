#include <QtTest>

#include "localization/LanguageManager.h"
#include "localization/NativeText.h"
#include "localization/LocaleRegistry.h"
#include "localization/StartupLocalization.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>

namespace {

constexpr auto kLocalizedId = "gamehq.test.manager.localized.message";
constexpr auto kFallbackId = "gamehq.test.manager.fallback.message";

QByteArray fixtureManifest()
{
    QFile file(QStringLiteral(":/i18n/locales-test.json"));
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QStringList availableTags(const LocaleRegistry &registry)
{
    QStringList tags;
    for (const QVariant &value : registry.availableLanguages())
        tags.append(value.toMap().value(QStringLiteral("tag")).toString());
    return tags;
}

} // namespace

class LanguageManagerTest : public QObject
{
    Q_OBJECT

private slots:
    void productionManifestIsValid();
    void promotedLaunchLocalesSwitchWithEnglishFallback();
    void aliasesAndFallbacksResolveDeterministically();
    void malformedAndInconsistentManifestsAreRejected();
    void systemAndExplicitLanguagesLoadCatalogs();
    void targetMissFallsBackToEnglish();
    void missingAndCorruptTargetCatalogsFallBackToEnglish();
    void missingSourceCatalogIsFatal();
    void startupBootstrapTranslatesBeforeAppInitialization();
    void localeSensitiveFormattingAndPluralsFollowLiveSwitches();
};

void LanguageManagerTest::productionManifestIsValid()
{
    LocaleRegistry registry(false);
    QString error;
    QVERIFY2(registry.load(QStringLiteral(":/i18n/locales.json"), &error), qPrintable(error));
    QCOMPARE(registry.sourceLanguage(), QStringLiteral("en-US"));
    const QStringList tags = availableTags(registry);
    QCOMPARE(tags.size(), 16);
    QVERIFY(tags.contains(QStringLiteral("en-US")));
    QVERIFY(tags.contains(QStringLiteral("pl-PL")));
    QVERIFY(tags.contains(QStringLiteral("th-TH")));
    QVERIFY(tags.contains(QStringLiteral("es-419")));
    QVERIFY(tags.contains(QStringLiteral("uk-UA")));
    QVERIFY(tags.contains(QStringLiteral("it-IT")));
    QCOMPARE(registry.canonicalTag(QStringLiteral("es-MX")), QStringLiteral("es-419"));
    QCOMPARE(registry.canonicalTag(QStringLiteral("es-CO")), QStringLiteral("es-419"));
    QVERIFY(!tags.contains(QStringLiteral("cs-CZ")));
    QVERIFY(!tags.contains(QStringLiteral("en-XA")));
    QVERIFY(!tags.contains(QStringLiteral("ar-XB")));
    QVERIFY(registry.catalogName(QStringLiteral("en-XA")).isEmpty());
    QVERIFY(registry.catalogName(QStringLiteral("ar-XB")).isEmpty());
    QCOMPARE(registry.resolveAvailable(QStringLiteral("en-XA")), QStringLiteral("en-US"));
    QCOMPARE(registry.resolveAvailable(QStringLiteral("ar-XB")), QStringLiteral("en-US"));
}

void LanguageManagerTest::promotedLaunchLocalesSwitchWithEnglishFallback()
{
    LocaleRegistry registry(false);
    QString error;
    QVERIFY2(registry.load(QStringLiteral(":/i18n/locales.json"), &error), qPrintable(error));
    LanguageManager manager(&registry);
    QVERIFY2(manager.initialize(QStringLiteral("en-US"), {}, &error), qPrintable(error));

    for (const QString& tag : {QStringLiteral("th-TH"), QStringLiteral("es-419"),
                               QStringLiteral("uk-UA"), QStringLiteral("it-IT")}) {
        manager.setRequestedLanguage(tag);
        QCOMPARE(manager.requestedLanguage(), tag);
        QCOMPARE(manager.effectiveLanguage(), tag);
        QCOMPARE(QLocale().name().replace(QLatin1Char('_'), QLatin1Char('-')), tag);
        QCOMPARE(qtTrId("gamehq.action.save"), QStringLiteral("Save"));
    }
}

void LanguageManagerTest::aliasesAndFallbacksResolveDeterministically()
{
    LocaleRegistry registry(false);
    QString error;
    QVERIFY2(registry.loadData(fixtureManifest(), &error), qPrintable(error));
    QCOMPARE(registry.canonicalTag(QStringLiteral("pl_PL")), QStringLiteral("pl-PL"));
    QCOMPARE(registry.canonicalTag(QStringLiteral("pl-CA")), QStringLiteral("pl-PL"));
    QCOMPARE(registry.canonicalTag(QStringLiteral("zh_CN")), QStringLiteral("zh-Hans"));
    QCOMPARE(registry.canonicalTag(QStringLiteral("zh-HK")), QStringLiteral("zh-Hant"));
    QCOMPARE(registry.canonicalTag(QStringLiteral("zh-Hant-TW")), QStringLiteral("zh-Hant"));
    QCOMPARE(registry.canonicalTag(QStringLiteral("zh-Hans-CN")), QStringLiteral("zh-Hans"));
    QCOMPARE(registry.resolveAvailable(QStringLiteral("es-MX")), QStringLiteral("en-US"));
    QCOMPARE(registry.resolveAvailable(QStringLiteral("xx-YY")), QStringLiteral("en-US"));
}

void LanguageManagerTest::malformedAndInconsistentManifestsAreRejected()
{
    LocaleRegistry registry(false);
    QString error;
    QVERIFY(!registry.loadData(QByteArrayLiteral("{not-json"), &error));
    QVERIFY(!error.isEmpty());

    QJsonDocument document = QJsonDocument::fromJson(fixtureManifest());
    QJsonObject root = document.object();
    QJsonObject aliases = root.value(QStringLiteral("aliases")).toObject();
    aliases.insert(QStringLiteral("pl"), QStringLiteral("en-US"));
    root.insert(QStringLiteral("aliases"), aliases);
    QVERIFY(!registry.loadData(QJsonDocument(root).toJson(), &error));
    QVERIFY(error.contains(QStringLiteral("inconsistent")));
}

void LanguageManagerTest::systemAndExplicitLanguagesLoadCatalogs()
{
    LocaleRegistry registry(false);
    QString error;
    QVERIFY2(registry.loadData(fixtureManifest(), &error), qPrintable(error));
    LanguageManager manager(&registry);
    QSignalSpy languageSpy(&manager, &LanguageManager::languageChanged);
    QSignalSpy revisionSpy(&manager, &LanguageManager::translationRevisionChanged);

    QVERIFY2(manager.initialize(QStringLiteral("system"),
                                {QStringLiteral("es-MX"), QStringLiteral("pl-PL")}, &error),
             qPrintable(error));
    QCOMPARE(manager.requestedLanguage(), QStringLiteral("system"));
    QCOMPARE(manager.effectiveLanguage(), QStringLiteral("pl-PL"));
    QCOMPARE(manager.localeName(), QStringLiteral("pl-PL"));
    QCOMPARE(manager.layoutDirection(), Qt::LeftToRight);
    QCOMPARE(qtTrId(kLocalizedId), QStringLiteral("Menedżer po polsku"));
    QVERIFY(!qtTrId(kLocalizedId).startsWith(QStringLiteral("gamehq.")));
    QCOMPARE(languageSpy.size(), 1);
    QCOMPARE(revisionSpy.size(), 1);

    manager.setRequestedLanguage(QStringLiteral("en"));
    QCOMPARE(manager.requestedLanguage(), QStringLiteral("en-US"));
    QCOMPARE(manager.effectiveLanguage(), QStringLiteral("en-US"));
    QCOMPARE(qtTrId(kLocalizedId), QStringLiteral("Manager English"));
    QCOMPARE(manager.translationRevision(), 2);
}

void LanguageManagerTest::targetMissFallsBackToEnglish()
{
    LocaleRegistry registry(false);
    QString error;
    QVERIFY2(registry.loadData(fixtureManifest(), &error), qPrintable(error));
    LanguageManager manager(&registry);
    QVERIFY2(manager.initialize(QStringLiteral("pl-PL"), {}, &error), qPrintable(error));
    QCOMPARE(qtTrId(kFallbackId), QStringLiteral("Manager fallback"));
    QVERIFY(!qtTrId(kFallbackId).startsWith(QStringLiteral("gamehq.")));
}

void LanguageManagerTest::missingAndCorruptTargetCatalogsFallBackToEnglish()
{
    for (const QString &requested : {QStringLiteral("zh-Hant"), QStringLiteral("fr-FR")}) {
        LocaleRegistry registry(false);
        QString error;
        QVERIFY2(registry.loadData(fixtureManifest(), &error), qPrintable(error));
        LanguageManager manager(&registry);
        QSignalSpy failureSpy(&manager, &LanguageManager::catalogLoadFailed);
        QVERIFY2(manager.initialize(requested, {}, &error), qPrintable(error));
        QCOMPARE(manager.requestedLanguage(), requested);
        QCOMPARE(manager.effectiveLanguage(), QStringLiteral("en-US"));
        QCOMPARE(qtTrId(kLocalizedId), QStringLiteral("Manager English"));
        QCOMPARE(failureSpy.size(), 1);
    }
}

void LanguageManagerTest::missingSourceCatalogIsFatal()
{
    QJsonDocument document = QJsonDocument::fromJson(fixtureManifest());
    QJsonObject root = document.object();
    QJsonArray locales = root.value(QStringLiteral("locales")).toArray();
    QJsonObject source = locales.at(0).toObject();
    source.insert(QStringLiteral("qt_catalog"), QStringLiteral("missing_source"));
    locales.replace(0, source);
    root.insert(QStringLiteral("locales"), locales);

    LocaleRegistry registry(false);
    QString error;
    QVERIFY2(registry.loadData(QJsonDocument(root).toJson(), &error), qPrintable(error));
    LanguageManager manager(&registry);
    QVERIFY(!manager.initialize(QStringLiteral("en-US"), {}, &error));
    QVERIFY(error.contains(QStringLiteral("Source catalog")));
    QCOMPARE(manager.translationRevision(), 0);
}

void LanguageManagerTest::startupBootstrapTranslatesBeforeAppInitialization()
{
    constexpr auto titleId = "gamehq.startup.portable_import.failed_title";
    constexpr auto detailId = "gamehq.notification.replay_failed.reason";

    QCOMPARE(NativeText::get(titleId, "Portable import failed"),
             QStringLiteral("Portable import failed"));

    {
        StartupLocalization startup;
        QString error;
        QVERIFY2(startup.initialize(QStringLiteral(":/i18n/locales-test.json"),
                                    {QStringLiteral("pl-PL")}, &error),
                 qPrintable(error));
        QCOMPARE(NativeText::get(titleId, "Portable import failed"),
                 QStringLiteral("Import profilu przenośnego nie powiódł się"));
        QCOMPARE(NativeText::get(detailId, "Reason: %1").arg(QStringLiteral("DXGI 0x887A")),
                 QStringLiteral("Powód: DXGI 0x887A"));
    }

    StartupLocalization unavailable;
    QString error;
    QVERIFY(!unavailable.initialize(QStringLiteral(":/i18n/missing-locales.json"), {}, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(NativeText::get(titleId, "Portable import failed"),
             QStringLiteral("Portable import failed"));
}

void LanguageManagerTest::localeSensitiveFormattingAndPluralsFollowLiveSwitches()
{
    LocaleRegistry registry(false);
    QString error;
    QVERIFY2(registry.loadData(fixtureManifest(), &error), qPrintable(error));
    LanguageManager manager(&registry);
    QVERIFY2(manager.initialize(QStringLiteral("en-US"), {}, &error), qPrintable(error));

    const QDateTime sample(QDate(2026, 9, 3), QTime(14, 5), QTimeZone::LocalTime);
    const auto verify = [&](const QString& tag) {
        const QLocale expected(tag);
        QCOMPARE(manager.effectiveLanguage(), tag);
        QCOMPARE(manager.formatInteger(1234567), expected.toString(1234567LL));
        QCOMPARE(manager.formatDecimal(1234.5, 1), expected.toString(1234.5, 'f', 1));
        QCOMPARE(manager.formatDate(sample),
                 expected.toString(sample.toLocalTime().date(), QLocale::ShortFormat));
        QCOMPARE(manager.formatDateTime(sample),
                 expected.toString(sample.toLocalTime(), QLocale::ShortFormat));
        const QString zeroDigit = expected.zeroDigit();
        QCOMPARE(manager.formatDuration(754000),
                 QStringLiteral("%1:%2").arg(expected.toString(12),
                                              expected.toString(34).rightJustified(
                                                  2, zeroDigit.isEmpty() ? u'0'
                                                                         : zeroDigit.front())));
        const QStringList items{QStringLiteral("Alpha"), QStringLiteral("Beta"),
                                QStringLiteral("Gamma")};
        QCOMPARE(manager.formatList(items), expected.createSeparatedList(items));
        QCOMPARE(qtTrId("gamehq.format.size.megabytes")
                     .arg(manager.formatDecimal(1.5, 1)),
                 QStringLiteral("%1 MB").arg(expected.toString(1.5, 'f', 1)));
        QVERIFY(!qtTrId("gamehq.duration.minutes", 1).contains(QStringLiteral("(s)")));
        QVERIFY(!qtTrId("gamehq.duration.minutes", 5).contains(QStringLiteral("(s)")));
    };

    verify(QStringLiteral("en-US"));
    for (const QString& tag : {QStringLiteral("pl-PL"), QStringLiteral("zh-Hans"),
                               QStringLiteral("de-DE"), QStringLiteral("en-US")}) {
        manager.setRequestedLanguage(tag);
        verify(tag);
    }
}

QTEST_GUILESS_MAIN(LanguageManagerTest)
#include "tst_languagemanager.moc"
