#include "config/ConfigKeys.h"
#include "config/ConfigManager.h"
#include "localization/LanguageManager.h"
#include "localization/LanguagePreference.h"
#include "localization/LocaleRegistry.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <memory>

class FakeBootstrapStore final : public LanguageBootstrapStore
{
public:
    explicit FakeBootstrapStore(QString language = {}, bool present = true)
        : m_language(std::move(language)), m_present(present)
    {
    }

    bool hasValue() const override
    {
        ++checks;
        return m_present;
    }

    QString value() const override
    {
        ++reads;
        return m_language;
    }

    bool removeValue() override
    {
        ++removals;
        if (!removeSucceeds)
            return false;
        m_present = false;
        return true;
    }

    bool present() const { return m_present; }

    mutable int checks = 0;
    mutable int reads = 0;
    int removals = 0;
    bool removeSucceeds = true;

private:
    QString m_language;
    bool m_present = false;
};

class LanguagePreferenceTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;

    QString configPath() const
    {
        return m_dir.path() + QStringLiteral("/config.json");
    }

    std::unique_ptr<LocaleRegistry> registry() const
    {
        auto result = std::make_unique<LocaleRegistry>(false);
        QString error;
        if (!result->load(QStringLiteral(":/i18n/locales-test.json"), &error))
            qFatal("Locale fixture failed: %s", qPrintable(error));
        return result;
    }

    QJsonObject savedConfig() const
    {
        QFile file(configPath());
        if (!file.open(QIODevice::ReadOnly))
            return {};
        return QJsonDocument::fromJson(file.readAll()).object();
    }

private slots:
    void init()
    {
        QVERIFY(m_dir.isValid());
        QFile::remove(configPath());
    }

    void freshInstalledLaunchConsumesValidBootstrap()
    {
        auto locales = registry();
        ConfigManager config(configPath());
        QCOMPARE(config.loadOrQuarantine(), ConfigManager::LoadResult::Missing);
        FakeBootstrapStore bootstrap(QStringLiteral("pl_PL"));
        LanguagePreference preference(&config, locales.get());

        QCOMPARE(preference.initialLanguage(bootstrap, false), QStringLiteral("pl-PL"));
        QVERIFY(config.hasExplicitValue(QString(ConfigKeys::UiLanguage)));
        QCOMPARE(config.value(QString(ConfigKeys::UiLanguage)).toString(),
                 QStringLiteral("pl-PL"));
        QCOMPARE(savedConfig().value(ConfigKeys::UiLanguage).toString(),
                 QStringLiteral("pl-PL"));
        QVERIFY(!bootstrap.present());
        QCOMPARE(bootstrap.removals, 1);
    }

    void existingExplicitPreferenceWins()
    {
        auto locales = registry();
        ConfigManager config(configPath());
        QVERIFY(config.load());
        config.setValue(QString(ConfigKeys::UiLanguage), QStringLiteral("pl-PL"));
        QVERIFY(config.save());
        FakeBootstrapStore bootstrap(QStringLiteral("en-US"));
        LanguagePreference preference(&config, locales.get());

        QCOMPARE(preference.initialLanguage(bootstrap, false), QStringLiteral("pl-PL"));
        QCOMPARE(config.value(QString(ConfigKeys::UiLanguage)).toString(),
                 QStringLiteral("pl-PL"));
        QVERIFY(!bootstrap.present());
    }

    void explicitSystemSurvivesReloadAndBlocksBootstrap()
    {
        {
            ConfigManager config(configPath());
            QVERIFY(config.load());
            config.setValue(QString(ConfigKeys::UiLanguage), QStringLiteral("system"));
            QVERIFY(config.hasExplicitValue(QString(ConfigKeys::UiLanguage)));
            QVERIFY(config.save());
        }

        auto locales = registry();
        ConfigManager reloaded(configPath());
        QVERIFY(reloaded.load());
        QVERIFY(reloaded.hasExplicitValue(QString(ConfigKeys::UiLanguage)));
        FakeBootstrapStore bootstrap(QStringLiteral("pl-PL"));
        LanguagePreference preference(&reloaded, locales.get());

        QCOMPARE(preference.initialLanguage(bootstrap, false), QStringLiteral("system"));
        QVERIFY(!bootstrap.present());
    }

    void invalidBootstrapFailsSafely()
    {
        auto locales = registry();
        ConfigManager config(configPath());
        QVERIFY(config.load());
        FakeBootstrapStore bootstrap(QStringLiteral("not-a-locale"));
        LanguagePreference preference(&config, locales.get());

        QCOMPARE(preference.initialLanguage(bootstrap, false), QStringLiteral("system"));
        QVERIFY(!config.hasExplicitValue(QString(ConfigKeys::UiLanguage)));
        QVERIFY(!bootstrap.present());
    }

    void consumedBootstrapIsNotReplayed()
    {
        auto locales = registry();
        {
            ConfigManager config(configPath());
            QVERIFY(config.load());
            FakeBootstrapStore bootstrap(QStringLiteral("pl-PL"));
            LanguagePreference preference(&config, locales.get());
            QCOMPARE(preference.initialLanguage(bootstrap, false), QStringLiteral("pl-PL"));
        }

        ConfigManager reloaded(configPath());
        QVERIFY(reloaded.load());
        FakeBootstrapStore consumed({}, false);
        LanguagePreference preference(&reloaded, locales.get());
        QCOMPARE(preference.initialLanguage(consumed, false), QStringLiteral("pl-PL"));
        QCOMPARE(consumed.reads, 0);
    }

    void upgradeBootstrapNeverOverwritesExistingChoice()
    {
        auto locales = registry();
        {
            ConfigManager config(configPath());
            QVERIFY(config.load());
            FakeBootstrapStore firstInstall(QStringLiteral("pl-PL"));
            LanguagePreference preference(&config, locales.get());
            QCOMPARE(preference.initialLanguage(firstInstall, false), QStringLiteral("pl-PL"));
        }

        ConfigManager upgraded(configPath());
        QVERIFY(upgraded.load());
        FakeBootstrapStore upgrade(QStringLiteral("en-US"));
        LanguagePreference preference(&upgraded, locales.get());
        QCOMPARE(preference.initialLanguage(upgrade, false), QStringLiteral("pl-PL"));
        QCOMPARE(upgraded.value(QString(ConfigKeys::UiLanguage)).toString(),
                 QStringLiteral("pl-PL"));
        QVERIFY(!upgrade.present());
    }

    void resetReturnsToSystemWithoutResurrectingBootstrap()
    {
        auto locales = registry();
        ConfigManager config(configPath());
        QVERIFY(config.load());
        FakeBootstrapStore bootstrap(QStringLiteral("pl-PL"));
        LanguagePreference preference(&config, locales.get());
        const QString initial = preference.initialLanguage(bootstrap, false);
        LanguageManager manager(locales.get());
        QString error;
        QVERIFY2(manager.initialize(initial, {QStringLiteral("en-US")}, &error),
                 qPrintable(error));
        preference.bind(&manager);

        QVERIFY(config.resetValue(QString(ConfigKeys::UiLanguage)));
        QVERIFY(config.save());
        QCOMPARE(manager.requestedLanguage(), QStringLiteral("system"));
        QCOMPARE(manager.effectiveLanguage(), QStringLiteral("en-US"));
        QVERIFY(!config.hasExplicitValue(QString(ConfigKeys::UiLanguage)));
        QVERIFY(!bootstrap.present());

        LanguagePreference restarted(&config, locales.get());
        QCOMPARE(restarted.initialLanguage(bootstrap, false), QStringLiteral("system"));
    }

    void portableModeIgnoresInstallerBootstrap()
    {
        auto locales = registry();
        ConfigManager config(configPath());
        QVERIFY(config.load());
        FakeBootstrapStore bootstrap(QStringLiteral("pl-PL"));
        LanguagePreference preference(&config, locales.get());

        QCOMPARE(preference.initialLanguage(bootstrap, true), QStringLiteral("system"));
        QVERIFY(bootstrap.present());
        QCOMPARE(bootstrap.checks, 0);
        QCOMPARE(bootstrap.reads, 0);
        QVERIFY(!config.hasExplicitValue(QString(ConfigKeys::UiLanguage)));
    }

    void runtimeSelectionsPersistIncludingExplicitSystem()
    {
        auto locales = registry();
        ConfigManager config(configPath());
        QVERIFY(config.load());
        FakeBootstrapStore bootstrap({}, false);
        LanguagePreference preference(&config, locales.get());
        LanguageManager manager(locales.get());
        QString error;
        QVERIFY2(manager.initialize(preference.initialLanguage(bootstrap, false),
                                    {QStringLiteral("en-US")}, &error), qPrintable(error));
        preference.bind(&manager);

        manager.setRequestedLanguage(QStringLiteral("pl-PL"));
        QCOMPARE(config.value(QString(ConfigKeys::UiLanguage)).toString(),
                 QStringLiteral("pl-PL"));
        manager.setRequestedLanguage(QStringLiteral("system"));
        QVERIFY(config.hasExplicitValue(QString(ConfigKeys::UiLanguage)));
        QCOMPARE(config.value(QString(ConfigKeys::UiLanguage)).toString(),
                 QStringLiteral("system"));
        QCOMPARE(savedConfig().value(ConfigKeys::UiLanguage).toString(),
                 QStringLiteral("system"));
    }
};

QTEST_GUILESS_MAIN(LanguagePreferenceTest)
#include "tst_languagepreference.moc"
