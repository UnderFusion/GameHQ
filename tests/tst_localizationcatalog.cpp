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
                entry.translation = xml.readElementText(
                    QXmlStreamReader::IncludeChildElements);
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

QStringList p4OneSourceFiles()
{
    const QString qmlRoot = QStringLiteral(GAMEHQ_SOURCE_DIR "/src/ui/qml/");
    QStringList files{
        qmlRoot + QStringLiteral("Main.qml"),
        qmlRoot + QStringLiteral("SettingsView.qml"),
        qmlRoot + QStringLiteral("ToastWindow.qml"),
        qmlRoot + QStringLiteral("settings/GeneralSettingsPage.qml"),
        qmlRoot + QStringLiteral("settings/AboutSettingsPage.qml"),
        qmlRoot + QStringLiteral("components/AboutWhatsNewDialog.qml"),
        qmlRoot + QStringLiteral("components/DesktopSidebar.qml"),
        qmlRoot + QStringLiteral("components/DialogCloseButton.qml"),
        qmlRoot + QStringLiteral("components/SettingsDisclosure.qml"),
        qmlRoot + QStringLiteral("components/SettingsPathRow.qml"),
        qmlRoot + QStringLiteral("components/UpdateBanner.qml"),
    };
    QDirIterator themes(qmlRoot + QStringLiteral("themes"),
                        {QStringLiteral("*.qml")}, QDir::Files);
    while (themes.hasNext())
        files.append(themes.next());
    return files;
}

QStringList productionQmlFiles()
{
    const QString qmlRoot = QStringLiteral(GAMEHQ_SOURCE_DIR "/src/ui/qml");
    QStringList files;
    QDirIterator iterator(qmlRoot, {QStringLiteral("*.qml")}, QDir::Files,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext())
        files.append(iterator.next());
    files.sort();
    return files;
}

QStringList p4ThreeSourceFiles()
{
    const QString root = QStringLiteral(GAMEHQ_SOURCE_DIR "/src/");
    return {
        root + QStringLiteral("main.cpp"),
        root + QStringLiteral("app/App.cpp"),
        root + QStringLiteral("ui/AppController.cpp"),
        root + QStringLiteral("config/CaptureLocations.cpp"),
        root + QStringLiteral("config/PortableProfileImporter.cpp"),
        root + QStringLiteral("updates/UpdateService.cpp"),
        root + QStringLiteral("updates/UpdatePreflight.cpp"),
        root + QStringLiteral("updates/UpdateDownloader.cpp"),
        root + QStringLiteral("updates/UpdateInstaller.cpp"),
    };
}

QStringList p4FourSourceFiles()
{
    const QString root = QStringLiteral(GAMEHQ_SOURCE_DIR "/src/");
    return {
        root + QStringLiteral("input/ActionCatalog.cpp"),
        root + QStringLiteral("input/BindingEditorModel.cpp"),
        root + QStringLiteral("input/BindingPattern.cpp"),
        root + QStringLiteral("tray/TrayIcon.cpp"),
        root + QStringLiteral("ui/AppController.cpp"),
    };
}

QStringList translatedCatalogs()
{
    return {
        QStringLiteral("gamehq_en_US.ts"), QStringLiteral("gamehq_zh_Hans.ts"),
        QStringLiteral("gamehq_ru_RU.ts"), QStringLiteral("gamehq_es_ES.ts"),
        QStringLiteral("gamehq_pt_BR.ts"), QStringLiteral("gamehq_de_DE.ts"),
        QStringLiteral("gamehq_ja_JP.ts"), QStringLiteral("gamehq_fr_FR.ts"),
        QStringLiteral("gamehq_pl_PL.ts"), QStringLiteral("gamehq_ko_KR.ts"),
        QStringLiteral("gamehq_zh_Hant.ts"), QStringLiteral("gamehq_tr_TR.ts"),
    };
}

QStringList promotedLaunchCatalogs()
{
    return {
        QStringLiteral("gamehq_th_TH.ts"), QStringLiteral("gamehq_es_419.ts"),
        QStringLiteral("gamehq_uk_UA.ts"), QStringLiteral("gamehq_it_IT.ts"),
    };
}

QSet<QString> translationIdsIn(const QStringList &paths)
{
    const QRegularExpression pattern(
        QStringLiteral("(?:qsTrId|qtTrId|QT_TRID_NOOP)\\s*\\(\\s*\"([^\"]+)\""));
    QSet<QString> ids;
    for (const QString &path : paths) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            continue;
        auto matches = pattern.globalMatch(QString::fromUtf8(file.readAll()));
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
    void languageSelectorUsesRegistryAndAtomicManagerPath();
    void promotedLaunchCatalogsAreSynchronizedAndFallBackToEnglish();
    void migratedP4OneIdsCoverEveryLaunchLocale();
    void migratedP4OneFilesHaveNoHardcodedUserText();
    void migratedProductionQmlIdsCoverEveryLaunchLocale();
    void productionQmlHasNoHardcodedUserText();
    void migratedP4ThreeCppIdsCoverEveryLaunchLocale();
    void migratedP4ThreeCppHasNoHardcodedErrorAssignments();
    void migratedP4FourNativeIdsCoverEveryLaunchLocale();
    void p4FourCachedPresentationIsIdDriven();
    void p4FiveFormattingIdsCoverEveryLaunchLocale();
    void p4FiveFormattingUsesLocaleAwareSafeBoundaries();
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

void LocalizationCatalogTest::languageSelectorUsesRegistryAndAtomicManagerPath()
{
    QFile general(QStringLiteral(GAMEHQ_SOURCE_DIR
                                 "/src/ui/qml/settings/GeneralSettingsPage.qml"));
    QVERIFY(general.open(QIODevice::ReadOnly));
    const QString generalSource = QString::fromUtf8(general.readAll());
    QVERIFY(generalSource.contains(QStringLiteral("objectName: \"languageSelector\"")));
    QVERIFY(generalSource.contains(QStringLiteral("languageManager.availableLanguages")));
    QVERIFY(generalSource.contains(QStringLiteral("locale.nativeName")));
    QVERIFY(generalSource.contains(
        QStringLiteral("languageManager.requestedLanguage = value")));
    QVERIFY(generalSource.contains(QStringLiteral("languageCombo.refresh()")));

    QFile combo(QStringLiteral(GAMEHQ_SOURCE_DIR
                               "/src/ui/qml/components/SettingsCombo.qml"));
    QVERIFY(combo.open(QIODevice::ReadOnly));
    const QString comboSource = QString::fromUtf8(combo.readAll());
    QVERIFY(comboSource.contains(QStringLiteral("signal valueCommitted(var value)")));
    QVERIFY(comboSource.contains(QStringLiteral("onOptionsChanged: refresh()")));
    QVERIFY(comboSource.contains(QStringLiteral("onDefaultValueChanged: refresh()")));
}

void LocalizationCatalogTest::promotedLaunchCatalogsAreSynchronizedAndFallBackToEnglish()
{
    QString error;
    const auto english = readTsCatalog(
        QStringLiteral(GAMEHQ_SOURCE_DIR "/i18n/app/gamehq_en_US.ts"), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const QSet<QString> activeIds = activeProductionIds();

    for (const QString& catalogName : promotedLaunchCatalogs()) {
        const auto catalog = readTsCatalog(
            QStringLiteral(GAMEHQ_SOURCE_DIR "/i18n/app/") + catalogName, &error);
        QVERIFY2(error.isEmpty(), qPrintable(catalogName + QStringLiteral(": ") + error));
        QCOMPARE(catalog.size(), english.size());
        for (const QString& id : activeIds) {
            QVERIFY2(catalog.contains(id), qPrintable(catalogName + QStringLiteral(" missing ") + id));
            QCOMPARE(catalog[id].source, english[id].source);
            QVERIFY2(catalog[id].unfinished,
                     qPrintable(catalogName + QStringLiteral(" must await selective translation: ") + id));
            QVERIFY2(catalog[id].translation.trimmed().isEmpty(),
                     qPrintable(catalogName + QStringLiteral(" contains an unreviewed translation: ") + id));
        }
    }

    QTranslator source;
    QVERIFY(source.load(QStringLiteral(":/i18n/gamehq_en_US.qm")));
    QVERIFY(QCoreApplication::installTranslator(&source));
    for (const QString& catalogName : promotedLaunchCatalogs()) {
        QString resourceName = catalogName;
        resourceName.chop(3);
        QTranslator target;
        QVERIFY2(target.load(QStringLiteral(":/i18n/%1.qm").arg(resourceName)),
                 qPrintable(resourceName));
        QVERIFY(QCoreApplication::installTranslator(&target));
        QCOMPARE(qtTrId("gamehq.action.save"), QStringLiteral("Save"));
        QCoreApplication::removeTranslator(&target);
    }
    QCoreApplication::removeTranslator(&source);
}

void LocalizationCatalogTest::migratedP4OneIdsCoverEveryLaunchLocale()
{
    const QSet<QString> ids = translationIdsIn(p4OneSourceFiles());
    // Main.qml and SettingsView.qml are shared with p4-2 and now contribute
    // 19 additional feature/dialog IDs to this source-file set.
    QCOMPARE(ids.size(), 217);
    for (const QString &catalogName : translatedCatalogs()) {
        QString error;
        const auto catalog = readTsCatalog(
            QStringLiteral(GAMEHQ_SOURCE_DIR "/i18n/app/") + catalogName, &error);
        QVERIFY2(error.isEmpty(), qPrintable(catalogName + QStringLiteral(": ") + error));
        for (const QString &id : ids) {
            QVERIFY2(catalog.contains(id),
                     qPrintable(catalogName + QStringLiteral(" missing ") + id));
            const auto &entry = catalog[id];
            QVERIFY2(!entry.unfinished,
                     qPrintable(catalogName + QStringLiteral(" unfinished ") + id));
            QVERIFY2(!entry.translation.trimmed().isEmpty(),
                     qPrintable(catalogName + QStringLiteral(" empty ") + id));
            QVERIFY2(!entry.translation.startsWith(QStringLiteral("gamehq.")),
                     qPrintable(catalogName + QStringLiteral(" exposed ") + id));
        }
    }
}

void LocalizationCatalogTest::migratedProductionQmlIdsCoverEveryLaunchLocale()
{
    const QSet<QString> ids = translationIdsIn(productionQmlFiles());
    QCOMPARE(ids.size(), 581);

    for (const QString &catalogName : translatedCatalogs()) {
        QString error;
        const auto catalog = readTsCatalog(
            QStringLiteral(GAMEHQ_SOURCE_DIR "/i18n/app/") + catalogName, &error);
        QVERIFY2(error.isEmpty(), qPrintable(catalogName + QStringLiteral(": ") + error));
        for (const QString &id : ids) {
            QVERIFY2(catalog.contains(id),
                     qPrintable(catalogName + QStringLiteral(" missing ") + id));
            const auto &entry = catalog[id];
            QVERIFY2(!entry.unfinished,
                     qPrintable(catalogName + QStringLiteral(" unfinished ") + id));
            QVERIFY2(!entry.translation.trimmed().isEmpty(),
                     qPrintable(catalogName + QStringLiteral(" empty ") + id));
            QVERIFY2(!entry.translation.startsWith(QStringLiteral("gamehq.")),
                     qPrintable(catalogName + QStringLiteral(" exposed ") + id));
        }
    }
}

void LocalizationCatalogTest::migratedP4ThreeCppIdsCoverEveryLaunchLocale()
{
    QSet<QString> ids;
    for (const QString &id : translationIdsIn(p4ThreeSourceFiles())) {
        if (id.startsWith(QStringLiteral("gamehq.error."))
            || id.startsWith(QStringLiteral("gamehq.notification."))
            || id.startsWith(QStringLiteral("gamehq.startup."))) {
            ids.insert(id);
        }
    }
    QCOMPARE(ids.size(), 144);

    for (const QString &catalogName : translatedCatalogs()) {
        QString error;
        const auto catalog = readTsCatalog(
            QStringLiteral(GAMEHQ_SOURCE_DIR "/i18n/app/") + catalogName, &error);
        QVERIFY2(error.isEmpty(), qPrintable(catalogName + QStringLiteral(": ") + error));
        for (const QString &id : ids) {
            QVERIFY2(catalog.contains(id),
                     qPrintable(catalogName + QStringLiteral(" missing ") + id));
            const auto &entry = catalog[id];
            QVERIFY2(!entry.unfinished,
                     qPrintable(catalogName + QStringLiteral(" unfinished ") + id));
            QVERIFY2(!entry.translation.trimmed().isEmpty(),
                     qPrintable(catalogName + QStringLiteral(" empty ") + id));
            QVERIFY2(!entry.translation.startsWith(QStringLiteral("gamehq.")),
                     qPrintable(catalogName + QStringLiteral(" exposed ") + id));
        }
    }
}

void LocalizationCatalogTest::migratedP4ThreeCppHasNoHardcodedErrorAssignments()
{
    const QRegularExpression hardcoded(
        QStringLiteral("(?:\\berror(?:Out)?|\\bm_errorText|\\brollbackError)\\s*=\\s*"
                       "QStringLiteral\\(\\\"|\\bfail\\(QStringLiteral\\(\\\"|"
                       "\\bcancelPreparation\\(QStringLiteral\\(\\\"|\\btr\\(\\\""));
    QStringList failures;
    for (const QString &path : p4ThreeSourceFiles()) {
        QFile file(path);
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(path));
        const QStringList lines = QString::fromUtf8(file.readAll()).split(u'\n');
        for (qsizetype index = 0; index < lines.size(); ++index) {
            const QString line = lines[index];
            if (line.contains(QStringLiteral("Injected ")))
                continue;
            if (hardcoded.match(line).hasMatch())
                failures.append(QStringLiteral("%1:%2: %3").arg(path).arg(index + 1).arg(line.trimmed()));
        }
    }
    QVERIFY2(failures.isEmpty(), qPrintable(failures.join(u'\n')));
}

void LocalizationCatalogTest::migratedP4FourNativeIdsCoverEveryLaunchLocale()
{
    QSet<QString> ids;
    for (const QString& id : translationIdsIn(p4FourSourceFiles())) {
        if (id.startsWith(QStringLiteral("gamehq.action."))
            || id.startsWith(QStringLiteral("gamehq.tray."))
            || id.startsWith(QStringLiteral("gamehq.input.model."))
            || id.startsWith(QStringLiteral("gamehq.input.gesture."))
            || id.startsWith(QStringLiteral("gamehq.hdr."))) {
            ids.insert(id);
        }
    }
    QCOMPARE(ids.size(), 100);

    for (const QString& catalogName : translatedCatalogs()) {
        QString error;
        const auto catalog = readTsCatalog(
            QStringLiteral(GAMEHQ_SOURCE_DIR "/i18n/app/") + catalogName, &error);
        QVERIFY2(error.isEmpty(), qPrintable(catalogName + QStringLiteral(": ") + error));
        for (const QString& id : ids) {
            QVERIFY2(catalog.contains(id),
                     qPrintable(catalogName + QStringLiteral(" missing ") + id));
            const auto& entry = catalog[id];
            QVERIFY2(!entry.unfinished,
                     qPrintable(catalogName + QStringLiteral(" unfinished ") + id));
            QVERIFY2(!entry.translation.trimmed().isEmpty(),
                     qPrintable(catalogName + QStringLiteral(" empty ") + id));
            QVERIFY2(!entry.translation.startsWith(QStringLiteral("gamehq.")),
                     qPrintable(catalogName + QStringLiteral(" exposed ") + id));
        }
    }
}

void LocalizationCatalogTest::p4FourCachedPresentationIsIdDriven()
{
    const auto readSource = [](const QString& relative) {
        QFile file(QStringLiteral(GAMEHQ_SOURCE_DIR "/src/") + relative);
        if (!file.open(QIODevice::ReadOnly))
            return QString();
        return QString::fromUtf8(file.readAll());
    };

    const QString actionCatalog = readSource(QStringLiteral("input/ActionCatalog.cpp"));
    const QString tray = readSource(QStringLiteral("tray/TrayIcon.cpp"));
    const QString editorHeader = readSource(QStringLiteral("input/BindingEditorModel.h"));
    const QString controller = readSource(QStringLiteral("ui/AppController.cpp"));
    QVERIFY(!actionCatalog.isEmpty());
    QVERIFY(actionCatalog.contains(QStringLiteral("PresentationSpec")));
    QVERIFY(!actionCatalog.contains(QStringLiteral("QStringLiteral(\"Screenshot\")")));
    QVERIFY(!tray.contains(QStringLiteral("tr(\"")));
    QVERIFY(tray.contains(QStringLiteral("m_rescanAction->setText")));
    QVERIFY(tray.contains(QStringLiteral("m_quitAction->setText")));
    QVERIFY(!editorHeader.contains(
        QStringLiteral("m_lastFiredAction = QStringLiteral(\"No action fired yet\")")));
    QVERIFY(!controller.contains(QStringLiteral("return QStringLiteral(\"Not checked yet\")")));
}

void LocalizationCatalogTest::p4FiveFormattingIdsCoverEveryLaunchLocale()
{
    const QSet<QString> ids{
        QStringLiteral("gamehq.action.back_with_marker"),
        QStringLiteral("gamehq.format.percent"),
        QStringLiteral("gamehq.input.compatibility.convert_press"),
        QStringLiteral("gamehq.input.compatibility.convert_press_with_wait"),
        QStringLiteral("gamehq.input.conflict.already_assigned"),
        QStringLiteral("gamehq.input.conflict.press_timed"),
        QStringLiteral("gamehq.input.relation.chord_start_delay"),
        QStringLiteral("gamehq.input.relation.context_override"),
        QStringLiteral("gamehq.input.relation.higher_tap_delay"),
        QStringLiteral("gamehq.input.relation.redundant"),
        QStringLiteral("gamehq.input.relation.shared_gesture"),
        QStringLiteral("gamehq.player.position_duration"),
        QStringLiteral("gamehq.update.last_checked_with_marker"),
        QStringLiteral("gamehq.update.release_metadata"),
    };
    const QRegularExpression placeholders(QStringLiteral("%(?:n|[1-9][0-9]*)"));
    for (const QString& catalogName : translatedCatalogs()) {
        QString error;
        const auto catalog = readTsCatalog(
            QStringLiteral(GAMEHQ_SOURCE_DIR "/i18n/app/") + catalogName, &error);
        QVERIFY2(error.isEmpty(), qPrintable(catalogName + QStringLiteral(": ") + error));
        for (const QString& id : ids) {
            QVERIFY2(catalog.contains(id), qPrintable(catalogName + QStringLiteral(" missing ") + id));
            const CatalogEntry& entry = catalog[id];
            QVERIFY2(!entry.unfinished && !entry.translation.trimmed().isEmpty(),
                     qPrintable(catalogName + QStringLiteral(" incomplete ") + id));
            QSet<QString> sourceTokens;
            QSet<QString> targetTokens;
            auto sourceMatches = placeholders.globalMatch(entry.source);
            while (sourceMatches.hasNext())
                sourceTokens.insert(sourceMatches.next().captured());
            auto targetMatches = placeholders.globalMatch(entry.translation);
            while (targetMatches.hasNext())
                targetTokens.insert(targetMatches.next().captured());
            QCOMPARE(targetTokens, sourceTokens);
        }
        for (const QString& pluralId : {
                 QStringLiteral("gamehq.duration.minutes"),
                 QStringLiteral("gamehq.duration.seconds"),
                 QStringLiteral("gamehq.settings.library.imports.count"),
                 QStringLiteral("gamehq.settings.library.scan.added")}) {
            QVERIFY2(catalog.contains(pluralId), qPrintable(catalogName + QStringLiteral(" missing ") + pluralId));
            const CatalogEntry& plural = catalog[pluralId];
            QVERIFY2(!plural.unfinished && plural.translation.contains(QStringLiteral("%n")),
                     qPrintable(catalogName + QStringLiteral(" invalid plural ") + pluralId));
            QVERIFY2(!plural.translation.contains(QStringLiteral("(s)")),
                     qPrintable(catalogName + QStringLiteral(" English plural shortcut ") + pluralId));
        }
    }
}

void LocalizationCatalogTest::p4FiveFormattingUsesLocaleAwareSafeBoundaries()
{
    const auto readSource = [](const QString& relative) {
        QFile file(QStringLiteral(GAMEHQ_SOURCE_DIR "/src/") + relative);
        if (!file.open(QIODevice::ReadOnly))
            return QString();
        return QString::fromUtf8(file.readAll());
    };

    const QString releaseNotes = readSource(QStringLiteral("app/ReleaseNotes.cpp"));
    const QString gallery = readSource(QStringLiteral("ui/GalleryModel.cpp"));
    const QString app = readSource(QStringLiteral("app/App.cpp"));
    const QString toast = readSource(QStringLiteral("ui/qml/components/Toast.qml"));
    const QString about = readSource(QStringLiteral("ui/qml/components/AboutWhatsNewDialog.qml"));
    const QString updateBanner = readSource(QStringLiteral("ui/qml/components/UpdateBanner.qml"));
    const QString player = readSource(QStringLiteral("ui/qml/components/PlayerControls.qml"));
    const QString binding = readSource(QStringLiteral("input/BindingEditorModel.cpp"));

    QVERIFY(!releaseNotes.contains(QStringLiteral("QLocale::c()")));
    QVERIFY(releaseNotes.contains(QStringLiteral("QLocale::ShortFormat")));
    QVERIFY(gallery.contains(QStringLiteral("QLocale().toString(dateTime, QLocale::ShortFormat)")));
    QVERIFY(app.contains(QStringLiteral("m_gallery.get(), &GalleryModel::retranslate")));
    QVERIFY(app.contains(QStringLiteral("m_overlayGallery.get(), &GalleryModel::retranslate")));
    QVERIFY(toast.contains(QStringLiteral("languageManager.formatDateTime(root.when)")));
    QVERIFY(!about.contains(QStringLiteral("Qt.formatDate(")));
    QVERIFY(!about.contains(QStringLiteral("Qt.formatDateTime(")));
    QVERIFY(about.contains(QStringLiteral("escapedStyledText(block.lead)")));
    QVERIFY(!about.contains(QStringLiteral("<a href=")));
    QVERIFY(!updateBanner.contains(QStringLiteral(".toFixed(")));
    QVERIFY(!updateBanner.contains(QStringLiteral(".join(\" · \"")));
    QVERIFY(player.contains(QStringLiteral("gamehq.player.position_duration")));
    QVERIFY(!player.contains(QStringLiteral("+ \" / \" +")));
    QVERIFY(binding.contains(QStringLiteral("gamehq.input.compatibility.convert_press_with_wait")));
    QVERIFY(!binding.contains(QStringLiteral("QString consequence")));
}

void LocalizationCatalogTest::productionQmlHasNoHardcodedUserText()
{
    const QRegularExpression assignment(
        QStringLiteral("(?:^|[{:;,])\\s*(?:pageTitle|pageDescription|eyebrow|title|label|"
                       "description|status|badge|placeholderText|Accessible\\.name|text|"
                       "message|cancelLabel|confirmLabel|replaceLabel|retryLabel|"
                       "convertLabel|act|heading|desc|binding)\\s*:\\s*"
                       "\"([^\"]*)\""));
    const QRegularExpression alphabetic(QStringLiteral("[A-Za-z]"));
    const QRegularExpression color(QStringLiteral("^#[0-9A-Fa-f]+$"));
    const QRegularExpression escapedGlyph(QStringLiteral("^(?:\\\\u[0-9A-Fa-f]{4})+$"));
    const QRegularExpression stableUiToken(
        QStringLiteral("^(?:X|Alt\\+Shift\\+G|Ctrl\\+Shift\\+[SE]|Enter|F|E|"
                       "W / A / S / D|L1 / R1|(?:70|80|90|100)%|(?:720|1080)p|4K)$"));

    QStringList failures;
    for (const QString &path : productionQmlFiles()) {
        QFile file(path);
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(path));
        const QStringList lines = QString::fromUtf8(file.readAll()).split(u'\n');
        for (qsizetype index = 0; index < lines.size(); ++index) {
            auto matches = assignment.globalMatch(lines[index]);
            while (matches.hasNext()) {
                const QString literal = matches.next().captured(1).trimmed();
                if (!literal.contains(alphabetic) || color.match(literal).hasMatch()
                    || escapedGlyph.match(literal).hasMatch()
                    || stableUiToken.match(literal).hasMatch()) {
                    continue;
                }
                failures.append(QStringLiteral("%1:%2: %3")
                                    .arg(path).arg(index + 1).arg(literal));
            }
        }
    }
    QVERIFY2(failures.isEmpty(), qPrintable(failures.join(u'\n')));
}

void LocalizationCatalogTest::migratedP4OneFilesHaveNoHardcodedUserText()
{
    const QRegularExpression assignment(
        QStringLiteral("(?:^|[{:;,])\\s*(?:pageTitle|pageDescription|eyebrow|title|label|"
                       "description|status|placeholderText|Accessible\\.name|text)\\s*:\\s*"
                       "\"([^\"]*)\""));
    const QRegularExpression alphabetic(QStringLiteral("[A-Za-z]"));
    const QRegularExpression color(QStringLiteral("^#[0-9A-Fa-f]+$"));
    const QRegularExpression escapedGlyph(QStringLiteral("^(?:\\\\u[0-9A-Fa-f]{4})+$"));

    QStringList failures;
    const QString qmlRoot = QStringLiteral(GAMEHQ_SOURCE_DIR "/src/ui/qml/");
    for (const QString &path : p4OneSourceFiles()) {
        if (path == qmlRoot + QStringLiteral("Main.qml")
            || path == qmlRoot + QStringLiteral("SettingsView.qml")) {
            continue; // Feature dialogs in these mixed-ownership files belong to p4-2.
        }
        QFile file(path);
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(path));
        const QStringList lines = QString::fromUtf8(file.readAll()).split(u'\n');
        for (qsizetype index = 0; index < lines.size(); ++index) {
            auto matches = assignment.globalMatch(lines[index]);
            while (matches.hasNext()) {
                const QString literal = matches.next().captured(1).trimmed();
                if (!literal.contains(alphabetic) || color.match(literal).hasMatch()
                    || escapedGlyph.match(literal).hasMatch() || literal == QLatin1String("X")) {
                    continue;
                }
                failures.append(QStringLiteral("%1:%2: %3")
                                    .arg(path).arg(index + 1).arg(literal));
            }
        }
    }
    QVERIFY2(failures.isEmpty(), qPrintable(failures.join(u'\n')));
}

QTEST_GUILESS_MAIN(LocalizationCatalogTest)
#include "tst_localizationcatalog.moc"
