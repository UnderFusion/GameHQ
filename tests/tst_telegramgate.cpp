#include "config/ConfigKeys.h"
#include "config/ConfigManager.h"
#include "share/ShareService.h"
#include "share/TelegramWiring.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace share;

// t25: Telegram Integrated is owner-deferred. In a standard build no config
// value may register it, show it or instantiate TDLib for it, while Telegram
// Desktop stays registered and independent.
class TestTelegramGate : public QObject
{
    Q_OBJECT

    static QStringList ids(const Service& s)
    {
        QStringList out;
        for (Provider* p : s.registry()->providers())
            out << p->id();
        return out;
    }

private slots:
    void standardBuildIsNotCompiledIn()
    {
        QVERIFY2(!kTelegramIntegratedCompiledIn,
                 "tst_telegramgate is a standard-build regression; a developer build "
                 "(-DGAMEHQ_TELEGRAM_INTEGRATED=ON) is expected to register the provider");
    }

    void oldOrHandEditedConfigCannotEnableIt()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("config.json");
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"share.telegram_integrated\": true, \"share.enabled\": true}");
        f.close();
        ConfigManager config(path);
        QVERIFY(config.load());
        QCOMPARE(config.value("share.telegram_integrated").toBool(), true);   // the key is inert

        Service service;
        service.setConfig(&config);
        TelegramWiring wiring = registerTelegramProviders(&service, dir.path(), dir.path());

        QVERIFY(!ids(service).contains(QStringLiteral("telegram.integrated")));
        // Nothing TDLib-related exists: no runtime object, no account, no QML object.
        QVERIFY(!wiring.runtime);
        QVERIFY(!wiring.account);
        QVERIFY(wiring.accountObject == nullptr);
        // Settings list and Share list do not carry it either.
        for (const QVariant& row : service.providerSettings())
            QVERIFY(row.toMap().value("id").toString() != QLatin1String("telegram.integrated"));
        QVERIFY(service.registry()->find(QStringLiteral("telegram.integrated")) == nullptr);
    }

    void telegramDesktopStillRegistersAndIsIndependent()
    {
        QTemporaryDir dir;
        ConfigManager config(dir.filePath("config.json"));
        Service service;
        service.setConfig(&config);
        TelegramWiring wiring = registerTelegramProviders(&service, dir.path(), dir.path());

        QVERIFY(ids(service).contains(QStringLiteral("telegram.desktop")));
        QVERIFY(service.providerEnabled(QStringLiteral("telegram.desktop")));
        // Its own switch, not tied to anything TDLib.
        service.setProviderEnabled(QStringLiteral("telegram.desktop"), false);
        QVERIFY(!service.providerEnabled(QStringLiteral("telegram.desktop")));
        service.setProviderEnabled(QStringLiteral("telegram.desktop"), true);
        QVERIFY(service.providerEnabled(QStringLiteral("telegram.desktop")));
        bool listed = false;
        for (const QVariant& row : service.providerSettings())
            listed = listed || row.toMap().value("id").toString() == QLatin1String("telegram.desktop");
        QVERIFY(listed);
        QVERIFY(!wiring.runtime);
    }
};

QTEST_MAIN(TestTelegramGate)
#include "tst_telegramgate.moc"
