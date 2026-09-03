#include "launcher/LauncherLocalization.h"
#include "launcher/LauncherResources.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>

namespace
{
QString readText(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(file.readAll());
}

bool writeText(const QString& path, const QByteArray& value)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(value) == value.size();
}

std::filesystem::path nativePath(const QString& path)
{
    return std::filesystem::path(path.toStdWString());
}
}

class TestLauncherLocalization : public QObject
{
    Q_OBJECT

private slots:
    void allProductionLocalesAndAliasesResolve()
    {
        const QJsonDocument manifest = QJsonDocument::fromJson(
            readText(QStringLiteral(GAMEHQ_LOCALES_JSON)).toUtf8());
        QVERIFY(manifest.isObject());

        int productionCount = 0;
        QSet<LANGID> languageIds;
        for (const QJsonValue& value : manifest.object().value(QStringLiteral("locales")).toArray()) {
            const QJsonObject locale = value.toObject();
            if (locale.value(QStringLiteral("tier")).toInt() != 1
                || locale.value(QStringLiteral("state")).toString() != QStringLiteral("enabled")) {
                continue;
            }
            ++productionCount;
            const QString tag = locale.value(QStringLiteral("tag")).toString();
            const std::wstring canonical = launcher::localization::canonicalTag(tag.toStdWString());
            QVERIFY2(canonical == tag.toStdWString(), qPrintable(tag));
            const LANGID language = launcher::localization::languageIdForTag(canonical);
            QVERIFY(language != 0);
            languageIds.insert(language);

            for (const QJsonValue& aliasValue : locale.value(QStringLiteral("aliases")).toArray()) {
                const QString alias = aliasValue.toString();
                QVERIFY2(launcher::localization::canonicalTag(alias.toStdWString()) == canonical,
                         qPrintable(alias));
                QCOMPARE(launcher::localization::languageIdForTag(alias.toStdWString()), language);
            }
        }
        QCOMPARE(productionCount, 16);
        QCOMPARE(languageIds.size(), 16);
        QVERIFY(launcher::localization::canonicalTag(L"en-XA").empty());
        QVERIFY(launcher::localization::canonicalTag(L"ar-XB").empty());
        QCOMPARE(launcher::localization::languageIdForTag(L"en-XA"),
                 launcher::localization::kEnglishLanguage);
    }

    void persistedSelectionAndFallbackAreSafe()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString config = directory.filePath(QStringLiteral("config.json"));

        QVERIFY(writeText(config,
            R"({"capture":{"modes":[1,true,null]},"ui.language":"es-MX","tail":"ok"})"));
        QVERIFY(launcher::localization::readPersistedLanguage(nativePath(config))
                == std::wstring(L"es-MX"));
        QCOMPARE(launcher::localization::resolveLanguage(
                     launcher::localization::readPersistedLanguage(nativePath(config)), {L"de-DE"}),
                 launcher::localization::languageIdForTag(L"es-419"));

        QVERIFY(writeText(config, R"({"ui.language":"uk-UA",)"));
        QVERIFY(launcher::localization::readPersistedLanguage(nativePath(config)).empty());
        QCOMPARE(launcher::localization::resolveLanguage({}, {L"uk-UA"}),
                 launcher::localization::languageIdForTag(L"uk-UA"));

        QVERIFY(writeText(config, R"({"ui.language":42})"));
        QVERIFY(launcher::localization::readPersistedLanguage(nativePath(config)).empty());
        QCOMPARE(launcher::localization::resolveLanguage(L"unknown", {L"it-IT"}),
                 launcher::localization::languageIdForTag(L"it-IT"));
        QCOMPARE(launcher::localization::resolveLanguage(L"system", {L"fr-CA"}),
                 launcher::localization::languageIdForTag(L"fr-FR"));
        QCOMPARE(launcher::localization::resolveLanguage({}, {L"ar-SA"}),
                 launcher::localization::kEnglishLanguage);
        QCOMPARE(launcher::localization::resolveLanguage({}, {}),
                 launcher::localization::kEnglishLanguage);
    }

    void portableAndInstalledConfigPathsStayIndependentOfQt()
    {
        QTemporaryDir package;
        QTemporaryDir roaming;
        QVERIFY(package.isValid());
        QVERIFY(roaming.isValid());

        const auto root = nativePath(package.path());
        const auto appData = nativePath(roaming.path());
        QVERIFY(launcher::localization::configurationPath(root, appData)
                == appData / L"GameHQ" / L"config.json");

        QVERIFY(writeText(package.filePath(QStringLiteral("portable.flag")), {}));
        QVERIFY(launcher::localization::configurationPath(root, appData)
                == root / L"gamehq-data" / L"config.json");
    }

    void unicodeResourcesCompileLoadAndFallbackToEnglish()
    {
        const HINSTANCE module = GetModuleHandleW(nullptr);
        QVERIFY(module != nullptr);
        const std::vector<std::wstring> tags = {
            L"en-US", L"zh-Hans", L"ru-RU", L"es-ES", L"pt-BR", L"de-DE",
            L"ja-JP", L"fr-FR", L"pl-PL", L"ko-KR", L"zh-Hant", L"tr-TR",
            L"th-TH", L"es-419", L"uk-UA", L"it-IT"
        };
        const unsigned int ids[] = {
            IDS_LAUNCHER_PATH_TOO_DEEP, IDS_LAUNCHER_UPDATE_ACTIVE,
            IDS_LAUNCHER_EXE_MISSING, IDS_LAUNCHER_COMMAND_TOO_LONG,
            IDS_LAUNCHER_START_FAILED
        };
        const std::wstring english = launcher::localization::loadString(
            module, IDS_LAUNCHER_PATH_TOO_DEEP, launcher::localization::kEnglishLanguage);
        QVERIFY(!english.empty());

        for (const std::wstring& tag : tags) {
            const LANGID language = launcher::localization::languageIdForTag(tag);
            QVERIFY(launcher::localization::loadString(module, IDS_LAUNCHER_TITLE, language)
                    == std::wstring(L"GameHQ"));
            std::wstring combined;
            for (unsigned int id : ids) {
                const std::wstring value = launcher::localization::loadString(module, id, language);
                QVERIFY2(!value.empty(), QString::fromStdWString(tag).toUtf8().constData());
                QVERIFY(value.find(L"GameHQ") != std::wstring::npos);
                combined += value;
            }
            if (tag != L"en-US") {
                QVERIFY(combined != english);
                QVERIFY(std::any_of(combined.begin(), combined.end(),
                                    [](wchar_t character) { return character > 0x7f; }));
            }
            QVERIFY(launcher::localization::loadString(
                        module, IDS_LAUNCHER_ENGLISH_FALLBACK_PROBE, language)
                    == std::wstring(L"English fallback probe"));
        }
    }

    void failurePathsAreResourceDrivenAndStaticBoundaryIsPreserved()
    {
        const QString source = readText(QStringLiteral(LAUNCHER_MAIN_SOURCE));
        const QString resources = readText(QStringLiteral(LAUNCHER_RC_SOURCE));
        const QString cmake = readText(QStringLiteral(LAUNCHER_CMAKE_SOURCE));
        const QString package = readText(QStringLiteral(LAUNCHER_PACKAGE_SOURCE));
        QVERIFY(!source.isEmpty());
        QVERIFY(!resources.isEmpty());

        for (const QString& literal : {
                 QStringLiteral("GameHQ is installed too deep"),
                 QStringLiteral("GameHQ is being updated"),
                 QStringLiteral("not found next to the launcher"),
                 QStringLiteral("command line passed to GameHQ"),
                 QStringLiteral("Failed to start app\\GameHQ.exe")}) {
            QVERIFY2(!source.contains(literal), qPrintable(literal));
        }
        QCOMPARE(source.count(QStringLiteral("MessageBoxW(")), 1);
        QCOMPARE(source.count(QStringLiteral("return 1;")), 6);
        QCOMPARE(source.count(QStringLiteral("return 0;")), 2);
        for (const QString& id : {
                 QStringLiteral("IDS_LAUNCHER_PATH_TOO_DEEP"),
                 QStringLiteral("IDS_LAUNCHER_UPDATE_ACTIVE"),
                 QStringLiteral("IDS_LAUNCHER_EXE_MISSING"),
                 QStringLiteral("IDS_LAUNCHER_COMMAND_TOO_LONG"),
                 QStringLiteral("IDS_LAUNCHER_START_FAILED")}) {
            QCOMPARE(source.count(id), 1);
            QCOMPARE(resources.count(id), 16);
        }

        QRegularExpression languageLine(QStringLiteral("(?m)^LANGUAGE\\s+"));
        QCOMPARE(resources.count(languageLine), 16);
        QVERIFY(!resources.contains(QStringLiteral("en-XA")));
        QVERIFY(!resources.contains(QStringLiteral("ar-XB")));
        QVERIFY(cmake.contains(QStringLiteral("launcher/LauncherStrings.rc")));
        QVERIFY(cmake.contains(QStringLiteral("target_link_options(GameHQLauncher PRIVATE -static)")));
        QVERIFY(!cmake.contains(QStringLiteral("target_link_libraries(GameHQLauncher PRIVATE Qt")));
        QVERIFY(package.contains(QStringLiteral("$launcherExe = Join-Path $source 'GameHQLauncher.exe'")));
        QVERIFY(package.contains(QStringLiteral(
            "Copy-Item -LiteralPath $launcherExe -Destination (Join-Path $target 'GameHQ.exe')")));
    }
};

QTEST_MAIN(TestLauncherLocalization)
#include "tst_launcherlocalization.moc"
