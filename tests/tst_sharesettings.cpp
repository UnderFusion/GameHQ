#include "config/ConfigManager.h"
#include "share/ShareService.h"
#include "share/providers/ClipboardShareProvider.h"
#include "share/providers/DiscordDesktopProvider.h"
#include "share/providers/DiscordWebhookProvider.h"
#include "share/providers/DiscordWebhookStore.h"
#include "share/providers/TelegramDesktopProvider.h"
#include "ui/NavigationState.h"

#include <QFile>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

using namespace share;

// t26: Sharing is a first-class Settings page and the only home of Share
// setup. Source-level checks pin the navigation and the Capture page (they read
// the real QML files); service-level checks drive the real built-in providers
// through the same enablement path the Sharing page uses.
class TestShareSettings : public QObject
{
    Q_OBJECT

    QString m_ns;
    QTemporaryDir m_dir;

    static QString readSource(const QString& relative)
    {
        QFile f(QStringLiteral(GAMEHQ_SOURCE_DIR) + QLatin1Char('/') + relative);
        if (!f.open(QIODevice::ReadOnly))
            return {};
        return QString::fromUtf8(f.readAll());
    }

    QString writeImage()
    {
        const QString path = m_dir.filePath("shot.png");
        QFile f(path);
        f.open(QIODevice::WriteOnly);
        f.write("png-bytes");
        return path;
    }

    struct Rig
    {
        std::unique_ptr<ConfigManager> config;
        std::unique_ptr<Service> service;
        DiscordWebhookProvider* webhook = nullptr;
    };

    Rig makeRig(const QString& configName = QStringLiteral("config.json"))
    {
        Rig r;
        r.config = std::make_unique<ConfigManager>(m_dir.filePath(configName));
        r.config->load();
        r.service = std::make_unique<Service>();
        r.service->setConfig(r.config.get());
        Service* s = r.service.get();
        // Same registration order as the app.
        s->registry()->add(new TelegramDesktopProvider(
            [] { return TelegramDesktopProvider::Installation{ QStringLiteral("C:/Telegram.exe"), {} }; },
            [](const QString&, const QStringList&) { return true; }, s));
        s->registry()->add(new DiscordDesktopProvider(
            [] { return DiscordDesktopProvider::Installation{ QStringLiteral("C:/Discord.exe"), {} }; },
            [](const QString&, const QStringList&) { return true; },
            [](const Request&) { return true; }, s));
        r.webhook = new DiscordWebhookProvider(
            std::make_unique<DiscordWebhookStore>(
                m_dir.filePath("share/discord-webhooks.json"), SecretStore(m_ns),
                [](const QString& url) { return url.startsWith(QLatin1String("https://")); }),
            s);
        s->registry()->add(r.webhook);
        s->registry()->add(new ClipboardShareProvider(s));
        return r;
    }

    static QStringList chooserIds(const Service& s)
    {
        QStringList out;
        for (const QVariant& v : s.providers())
            out << v.toMap().value("id").toString();
        return out;
    }

private slots:
    void initTestCase()
    {
        m_ns = QStringLiteral("GameHQ.Test.Sharing.") + QUuid::createUuid().toString(QUuid::Id128);
    }
    void cleanupTestCase() { SecretStore(m_ns).removeAll(QStringLiteral("discord.webhook")); }
    void cleanup()
    {
        SecretStore(m_ns).removeAll(QStringLiteral("discord.webhook"));
        QFile::remove(m_dir.filePath("config.json"));
        QFile::remove(m_dir.filePath("share/discord-webhooks.json"));
    }

    // ---- navigation -------------------------------------------------------

    void sharingIsItsOwnLeftNavEntryBetweenNotificationsAndAdvanced()
    {
        const QString qml = readSource("src/ui/qml/SettingsView.qml");
        QVERIFY(!qml.isEmpty());
        const QStringList expected = { "general", "capture", "replay", "input", "library",
                                       "notifications_sound", "sharing", "advanced", "about" };
        QCOMPARE(NavigationState::settingsCategoryKeys(), expected);

        // The QML key list is the persisted contract: it must equal the C++ one.
        const auto keys = QRegularExpression(R"(categoryKeys:\s*\[([^\]]*)\])").match(qml);
        QVERIFY(keys.hasMatch());
        QStringList qmlKeys;
        auto it = QRegularExpression("\"([a-z_]+)\"").globalMatch(keys.captured(1));
        while (it.hasNext())
            qmlKeys << it.next().captured(1);
        QCOMPARE(qmlKeys, expected);

        // One label per key, in the same order, Sharing among them.
        QStringList labels;
        auto lit = QRegularExpression(R"re(qsTrId\("gamehq\.settings\.category\.([a-z_]+)"\))re").globalMatch(qml);
        while (lit.hasNext())
            labels << lit.next().captured(1);
        QCOMPARE(labels, expected);

        // The stacked pages follow the same order.
        const int stack = qml.indexOf("StackLayout {");
        QVERIFY(stack > 0);
        QStringList pages;
        auto pit = QRegularExpression(R"(\b([A-Z][A-Za-z]+)SettingsPage \{)").globalMatch(qml.mid(stack));
        while (pit.hasNext())
            pages << pit.next().captured(1);
        QCOMPARE(pages, (QStringList{ "General", "Capture", "Replay", "Input", "Library",
                                      "Feedback", "Sharing", "Advanced", "About" }));
    }

    void everyInputMethodReachesPagesThroughTheSameGenericHandlers()
    {
        // Mouse click, keyboard and controller all go through selectCategory()
        // over categories.length, so a new page needs no per-input wiring.
        const QString qml = readSource("src/ui/qml/SettingsView.qml");
        QVERIFY(qml.count("root.selectCategory(") >= 3);
        QVERIFY(qml.count("categories.length") >= 4);
        QVERIFY(!qml.contains(QRegularExpression(R"(currentCategory\s*[=!]==?\s*[0-9])")));
    }

    void legacyNumericCategoryStillMigratesToTheRightPage()
    {
        // A pre-0.7.11 index counted pages before Sharing existed.
        ConfigManager config(m_dir.filePath("legacy.json"));
        config.setValue("ui.settings_category", 6);   // "advanced" in the old order
        NavigationState nav(&config);
        nav.migrateLegacyKeys();
        QCOMPARE(nav.settingsCategory(), QStringLiteral("advanced"));
    }

    // ---- the Capture page --------------------------------------------------

    void capturePageHasNoShareControls()
    {
        const QString qml = readSource("src/ui/qml/settings/CaptureSettingsPage.qml").toLower();
        QVERIFY(!qml.isEmpty());
        for (const char* word : { "share", "webhook", "add-on", "discord", "telegram", "clipboard" })
            QVERIFY2(!qml.contains(QLatin1String(word)),
                     qPrintable(QStringLiteral("Capture page still mentions '%1'").arg(QLatin1String(word))));
    }

    void sharingPageOwnsEveryShareSetting()
    {
        const QString qml = readSource("src/ui/qml/settings/SharingSettingsPage.qml");
        QVERIFY(qml.contains("share.enabled"));                    // master switch
        QVERIFY(qml.contains("providerSettings()"));               // per-method toggles
        QVERIFY(qml.contains("\"share.provider.\" + entry.modelData.id + \".enabled\""));
        QVERIFY(qml.contains("ShareDestinationsSection"));         // Discord channel setup
        QVERIFY(qml.contains("share.external_providers"));         // add-ons
        // No provider is hard-coded into the page.
        QVERIFY(!qml.contains("telegram.desktop"));
        QVERIFY(!qml.contains("discord.webhook"));
    }

    // ---- per-method toggles ------------------------------------------------

    void everySupportedMethodHasItsOwnSwitch()
    {
        Rig r = makeRig();
        QStringList ids;
        for (const QVariant& v : r.service->providerSettings())
            ids << v.toMap().value("id").toString();
        QCOMPARE(ids, (QStringList{ "telegram.desktop", "discord.desktop", "discord.webhook", "clipboard" }));
        QVERIFY(!ids.contains(QStringLiteral("telegram.integrated")));
    }

    void eachMethodTogglesIndependentlyAndDisappearsFromTheChooser()
    {
        Rig r = makeRig();
        QVERIFY(r.service->open(writeImage()));
        const QStringList all = { "telegram.desktop", "discord.desktop", "discord.webhook", "clipboard" };
        QCOMPARE(chooserIds(*r.service), all);

        for (const QString& id : all) {
            r.service->setProviderEnabled(id, false);
            QStringList expected = all;
            expected.removeAll(id);
            QCOMPARE(chooserIds(*r.service), expected);   // only this one is gone
            r.service->setProviderEnabled(id, true);
            QCOMPARE(chooserIds(*r.service), all);
        }
    }

    void theOwnersExampleWorks()
    {
        // Telegram Desktop ON, everything else OFF.
        Rig r = makeRig();
        QVERIFY(r.service->open(writeImage()));
        for (const QString& id : { "discord.desktop", "discord.webhook", "clipboard" })
            r.service->setProviderEnabled(id, false);
        QCOMPARE(chooserIds(*r.service), (QStringList{ "telegram.desktop" }));
        QVERIFY(r.service->requestTargets(QStringLiteral("telegram.desktop")));
        // A disabled method cannot be used even if a stale caller still names it.
        QVERIFY(!r.service->requestTargets(QStringLiteral("clipboard")));
        QCOMPARE(r.service->lastError(), QStringLiteral("provider_disabled"));
        QVERIFY(r.service->share(QStringLiteral("discord.desktop"), QStringLiteral("x")).isEmpty());
        QCOMPARE(r.service->lastError(), QStringLiteral("provider_disabled"));
    }

    void switchesSurviveARestart()
    {
        {
            Rig r = makeRig();
            r.service->setProviderEnabled(QStringLiteral("clipboard"), false);
            r.service->setProviderEnabled(QStringLiteral("discord.desktop"), false);
            QVERIFY(r.config->save());
        }
        Rig again = makeRig();
        QVERIFY(!again.service->providerEnabled(QStringLiteral("clipboard")));
        QVERIFY(!again.service->providerEnabled(QStringLiteral("discord.desktop")));
        QVERIFY(again.service->providerEnabled(QStringLiteral("telegram.desktop")));
        QVERIFY(again.service->providerEnabled(QStringLiteral("discord.webhook")));
    }

    void masterSwitchDisablesEverythingWithoutDeletingAnything()
    {
        Rig r = makeRig();
        QVERIFY(r.service->open(writeImage()));
        r.service->setProviderEnabled(QStringLiteral("clipboard"), false);
        r.service->close();
        r.service->setSharingEnabled(false);
        QVERIFY(!r.service->open(writeImage()));
        QCOMPARE(r.service->lastError(), QStringLiteral("sharing_disabled"));
        for (const QString& id : { "telegram.desktop", "discord.desktop", "discord.webhook", "clipboard" })
            QVERIFY(!r.service->providerEnabled(id));
        r.service->setSharingEnabled(true);
        QVERIFY(r.service->open(writeImage()));
        // The individual choice made before is remembered.
        QVERIFY(!r.service->providerEnabled(QStringLiteral("clipboard")));
        QVERIFY(r.service->providerEnabled(QStringLiteral("telegram.desktop")));
    }

    // ---- saved configuration survives disable / re-enable -----------------

    void discordDestinationsSurviveDisableAndReEnable()
    {
        Rig r = makeRig();
        QCOMPARE(r.webhook->addSavedDestination(QStringLiteral("Squad"),
                                                QStringLiteral("https://discord.com/api/webhooks/123456789012345678/abcdefghijklmnopqrstuvwxyz0123456789")),
                 QString());
        QCOMPARE(r.webhook->savedDestinations().size(), 1);
        const QString id = r.webhook->savedDestinations().first().id;
        QVERIFY(r.webhook->setSavedDestinationPinned(id, true));
        QVERIFY(r.service->open(writeImage()));

        r.service->setProviderEnabled(QStringLiteral("discord.webhook"), false);
        QVERIFY(!chooserIds(*r.service).contains(QStringLiteral("discord.webhook")));
        // Nothing was removed while it was off.
        QCOMPARE(r.webhook->savedDestinations().size(), 1);
        QVERIFY(r.webhook->savedDestinations().first().pinned);
        bool found = false;
        SecretStore(m_ns).read(QStringLiteral("discord.webhook"), QStringLiteral("dest-") + id, &found);
        QVERIFY(!SecretStore(m_ns).names(QStringLiteral("discord.webhook")).isEmpty());
        Q_UNUSED(found);

        // The Sharing page still lists the destinations while the method is off.
        const QVariantList mgmt = r.service->savedDestinationProviders();
        QCOMPARE(mgmt.size(), 1);
        QCOMPARE(mgmt.first().toMap().value("destinations").toList().size(), 1);

        r.service->setProviderEnabled(QStringLiteral("discord.webhook"), true);
        QVERIFY(chooserIds(*r.service).contains(QStringLiteral("discord.webhook")));
        QVERIFY(r.service->requestTargets(QStringLiteral("discord.webhook")));
        QCOMPARE(r.service->targets().size(), 1);
        QCOMPARE(r.service->targets().first().toMap().value("name").toString(), QStringLiteral("Squad"));
    }

    void disablingTheMasterSwitchKeepsDestinationsToo()
    {
        Rig r = makeRig();
        r.webhook->addSavedDestination(QStringLiteral("Squad"),
                                       QStringLiteral("https://discord.com/api/webhooks/123456789012345678/abcdefghijklmnopqrstuvwxyz0123456789"));
        r.service->setSharingEnabled(false);
        QCOMPARE(r.webhook->savedDestinations().size(), 1);
        r.service->setSharingEnabled(true);
        QCOMPARE(r.webhook->savedDestinations().size(), 1);
    }
};

QTEST_MAIN(TestShareSettings)
#include "tst_sharesettings.moc"
