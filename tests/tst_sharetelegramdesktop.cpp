#include "share/ShareService.h"
#include "share/providers/TelegramDesktopProvider.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

using namespace share;

// Telegram Desktop hand-off (t12). The launcher is injected, so nothing is
// started: the cases pin what GameHQ would run (the exact capture, the same
// Telegram profile the `tg` handler uses) and that the result is honest.
class TestShareTelegramDesktop : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;

    QString capture(const QString& name)
    {
        const QString path = m_dir.filePath(name);
        QFile f(path);
        f.open(QIODevice::WriteOnly);
        f.write("media");
        return path;
    }

    static TelegramDesktopProvider::Installation installed()
    {
        return { QStringLiteral("C:/Tools/TG/Telegram.exe"),
                 { QStringLiteral("-workdir"), QStringLiteral("D:\\TG Data") } };
    }

private slots:
    void parsesRegisteredHandler()
    {
        auto parsed = TelegramDesktopProvider::parseHandlerCommand(
            QStringLiteral("\"C:\\Users\\me\\AppData\\Roaming\\Telegram Desktop\\Telegram.exe\" -- \"%1\""));
        QCOMPARE(parsed.executable,
                 QStringLiteral("C:/Users/me/AppData/Roaming/Telegram Desktop/Telegram.exe"));
        QVERIFY(parsed.profileArgs.isEmpty());

        parsed = TelegramDesktopProvider::parseHandlerCommand(
            QStringLiteral("\"D:\\Portable\\Telegram.exe\" -workdir \"D:\\Portable\\data\" -- \"%1\""));
        QCOMPARE(parsed.executable, QStringLiteral("D:/Portable/Telegram.exe"));
        QCOMPARE(parsed.profileArgs,
                 (QStringList{ QStringLiteral("-workdir"), QStringLiteral("D:\\Portable\\data") }));

        QVERIFY(!TelegramDesktopProvider::parseHandlerCommand(QString()).isValid());
    }

    void notInstalledIsListedButUnavailable()
    {
        Service s;
        TelegramDesktopProvider p([] { return TelegramDesktopProvider::Installation(); },
                                  [](const QString&, const QStringList&) { return true; });
        s.registry()->add(&p);
        QVERIFY(s.open(capture("a.png")));
        const QVariantMap row = s.providers().first().toMap();
        QCOMPARE(row.value("id").toString(), QStringLiteral("telegram.desktop"));
        QCOMPARE(row.value("available").toBool(), false);
        QCOMPARE(row.value("availability").toString(), QStringLiteral("not_installed"));
        QCOMPARE(row.value("access").toString(), QStringLiteral("none"));
        QVERIFY(!s.requestTargets("telegram.desktop"));
    }

    void handsTheExactCaptureToTelegram_data()
    {
        QTest::addColumn<QString>("fileName");
        QTest::newRow("screenshot") << QStringLiteral("2026-09-29_14-00-00.jpg");
        QTest::newRow("clip") << QStringLiteral("clip with spaces.mp4");
    }

    void handsTheExactCaptureToTelegram()
    {
        QFETCH(QString, fileName);
        QString program;
        QStringList args;
        Service s;
        TelegramDesktopProvider p(&installed, [&](const QString& prog, const QStringList& a) {
            program = prog;
            args = a;
            return true;
        });
        s.registry()->add(&p);
        const QString file = capture(fileName);
        QVERIFY(s.open(file));
        QVERIFY(s.requestTargets("telegram.desktop"));
        QCOMPARE(s.targets().size(), 1);
        QVERIFY(!s.share("telegram.desktop", "telegram-desktop").isEmpty());

        QCOMPARE(program, QDir::toNativeSeparators(QStringLiteral("C:/Tools/TG/Telegram.exe")));
        // Same profile as the registered handler, then exactly one file.
        QCOMPARE(args, (QStringList{ QStringLiteral("-workdir"), QStringLiteral("D:\\TG Data"),
                                     QStringLiteral("-sendpath"),
                                     QDir::toNativeSeparators(QFileInfo(file).absoluteFilePath()) }));
        QCOMPARE(s.lastResult().value("outcome").toString(), QStringLiteral("handed_off"));
    }

    void launchFailureIsReportedNotRetried()
    {
        int launches = 0;
        Service s;
        TelegramDesktopProvider p(&installed, [&](const QString&, const QStringList&) {
            ++launches;
            return false;
        });
        s.registry()->add(&p);
        QVERIFY(s.open(capture("b.png")));
        QVERIFY(s.requestTargets("telegram.desktop"));
        s.share("telegram.desktop", "telegram-desktop");
        QCOMPARE(launches, 1);
        QCOMPARE(s.lastResult().value("outcome").toString(), QStringLiteral("failed"));
        QCOMPARE(s.lastResult().value("errorCode").toString(), QStringLiteral("launch_failed"));
    }
};

QTEST_MAIN(TestShareTelegramDesktop)
#include "tst_sharetelegramdesktop.moc"
