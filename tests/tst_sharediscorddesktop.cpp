#include "share/ShareService.h"
#include "share/providers/DiscordDesktopProvider.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

using namespace share;

// Discord Desktop hand-off (t14). Locator, launcher and clipboard are injected,
// so nothing is started or copied: the cases pin what GameHQ would run, that
// the exact capture is copied first, and that the result is honest.
class TestShareDiscordDesktop : public QObject
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

    static DiscordDesktopProvider::Installation installed()
    {
        return { QStringLiteral("C:/Users/me/AppData/Local/Discord/Update.exe"),
                 { QStringLiteral("--processStart"), QStringLiteral("Discord.exe") } };
    }

private slots:
    void parsesRegisteredHandler()
    {
        auto parsed = DiscordDesktopProvider::parseHandlerCommand(QStringLiteral(
            "\"C:\\Users\\me\\AppData\\Local\\Discord\\Update.exe\" --processStart Discord.exe "
            "--process-start-args \"--url -- \\\"%1\\\"\""));
        QCOMPARE(parsed.executable,
                 QStringLiteral("C:/Users/me/AppData/Local/Discord/Update.exe"));
        // The URL placeholder must not be passed on.
        QCOMPARE(parsed.launchArgs,
                 (QStringList{ QStringLiteral("--processStart"), QStringLiteral("Discord.exe") }));

        parsed = DiscordDesktopProvider::parseHandlerCommand(
            QStringLiteral("\"C:\\Discord\\Discord.exe\" \"%1\""));
        QCOMPARE(parsed.executable, QStringLiteral("C:/Discord/Discord.exe"));
        QVERIFY(parsed.launchArgs.isEmpty());

        QVERIFY(!DiscordDesktopProvider::parseHandlerCommand(QString()).isValid());
    }

    void notInstalledIsListedButUnavailable()
    {
        Service s;
        DiscordDesktopProvider p([] { return DiscordDesktopProvider::Installation(); },
                                 [](const QString&, const QStringList&) { return true; },
                                 [](const Request&) { return true; });
        s.registry()->add(&p);
        QVERIFY(s.open(capture("a.png")));
        const QVariantMap row = s.providers().first().toMap();
        QCOMPARE(row.value("id").toString(), QStringLiteral("discord.desktop"));
        QCOMPARE(row.value("available").toBool(), false);
        QCOMPARE(row.value("availability").toString(), QStringLiteral("not_installed"));
        QCOMPARE(row.value("access").toString(), QStringLiteral("none"));
        QVERIFY(!s.requestTargets("discord.desktop"));
    }

    void copiesTheExactCaptureThenOpensDiscord_data()
    {
        QTest::addColumn<QString>("fileName");
        QTest::newRow("screenshot") << QStringLiteral("2026-09-29_14-00-00.jpg");
        QTest::newRow("clip") << QStringLiteral("clip with spaces.mp4");
    }

    void copiesTheExactCaptureThenOpensDiscord()
    {
        QFETCH(QString, fileName);
        QStringList order;
        QString copied;
        QString program;
        QStringList args;
        Service s;
        DiscordDesktopProvider p(&installed,
                                 [&](const QString& prog, const QStringList& a) {
                                     order << QStringLiteral("launch");
                                     program = prog;
                                     args = a;
                                     return true;
                                 },
                                 [&](const Request& r) {
                                     order << QStringLiteral("copy");
                                     copied = r.filePath();
                                     return true;
                                 });
        s.registry()->add(&p);
        const QString file = capture(fileName);
        QVERIFY(s.open(file));
        QVERIFY(s.requestTargets("discord.desktop"));
        QCOMPARE(s.targets().size(), 1);
        QVERIFY(!s.share("discord.desktop", "discord-desktop").isEmpty());

        QCOMPARE(order, (QStringList{ QStringLiteral("copy"), QStringLiteral("launch") }));
        QCOMPARE(QFileInfo(copied).absoluteFilePath(), QFileInfo(file).absoluteFilePath());
        QCOMPARE(program, QDir::toNativeSeparators(installed().executable));
        QCOMPARE(args, installed().launchArgs);
        QCOMPARE(s.lastResult().value("outcome").toString(), QStringLiteral("handed_off"));
        QCOMPARE(s.lastResult().value("detail").toString(), QStringLiteral("paste"));
    }

    void launchFailureStillLeavesTheFileCopied()
    {
        int launches = 0;
        Service s;
        DiscordDesktopProvider p(&installed,
                                 [&](const QString&, const QStringList&) {
                                     ++launches;
                                     return false;
                                 },
                                 [](const Request&) { return true; });
        s.registry()->add(&p);
        QVERIFY(s.open(capture("b.png")));
        QVERIFY(s.requestTargets("discord.desktop"));
        s.share("discord.desktop", "discord-desktop");
        QCOMPARE(launches, 1);
        QCOMPARE(s.lastResult().value("outcome").toString(), QStringLiteral("copied"));
        QCOMPARE(s.lastResult().value("errorCode").toString(), QStringLiteral("launch_failed"));
    }

    void clipboardFailureDoesNotOpenDiscord()
    {
        int launches = 0;
        Service s;
        DiscordDesktopProvider p(&installed,
                                 [&](const QString&, const QStringList&) {
                                     ++launches;
                                     return true;
                                 },
                                 [](const Request&) { return false; });
        s.registry()->add(&p);
        QVERIFY(s.open(capture("c.png")));
        QVERIFY(s.requestTargets("discord.desktop"));
        s.share("discord.desktop", "discord-desktop");
        QCOMPARE(launches, 0);
        QCOMPARE(s.lastResult().value("outcome").toString(), QStringLiteral("failed"));
        QCOMPARE(s.lastResult().value("errorCode").toString(),
                 QStringLiteral("clipboard_unavailable"));
    }
};

QTEST_MAIN(TestShareDiscordDesktop)
#include "tst_sharediscorddesktop.moc"
