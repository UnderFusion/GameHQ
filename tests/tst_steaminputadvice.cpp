// Steam Input conflict help: the pure halves. Which Steam app installed a game
// (install manifests only), which advised buttons drive a GameHQ shortcut, and
// the per-game "reviewed" state that re-warns when the mapping changes.
#include "games/SteamAppLookup.h"
#include "input/BindingPattern.h"
#include "input/ControlId.h"
#include "input/SteamInputAdvice.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

namespace {
BindingResolver::Binding row(const QString& action, const QString& trigger, bool unbound = false)
{
    BindingResolver::Binding b;
    b.deviceGroup = QStringLiteral("controller");
    b.actionId = action;
    b.triggerCode = trigger;
    b.unbound = unbound;
    return b;
}

void writeFile(const QString& path, const QByteArray& content)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(content);
}
}

class SteamInputAdviceTest : public QObject
{
    Q_OBJECT
private slots:
    void lookupFindsAppIdFromInstallManifest()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString steamapps = root.path() + QStringLiteral("/Steam/steamapps");
        QVERIFY(QDir().mkpath(steamapps + QStringLiteral("/common/ELDEN RING/Game")));
        writeFile(steamapps + QStringLiteral("/appmanifest_1245620.acf"),
                  "\"AppState\"\n{\n\t\"appid\"\t\t\"1245620\"\n\t\"name\"\t\t\"ELDEN RING\"\n"
                  "\t\"installdir\"\t\t\"ELDEN RING\"\n}\n");
        writeFile(steamapps + QStringLiteral("/appmanifest_10.acf"),
                  "\"AppState\"\n{\n\t\"name\"\t\t\"Other\"\n\t\"installdir\"\t\t\"Other\"\n}\n");

        const SteamApp app = SteamAppLookup::forExecutable(
            steamapps + QStringLiteral("/common/elden ring/Game/eldenring.exe"));
        QCOMPARE(app.appId, QStringLiteral("1245620"));
        QCOMPARE(app.name, QStringLiteral("ELDEN RING"));

        QVERIFY(!SteamAppLookup::forExecutable(
                     root.path() + QStringLiteral("/Epic/Game/game.exe")).isValid());
        QVERIFY(!SteamAppLookup::forExecutable(
                     steamapps + QStringLiteral("/common/Missing/x.exe")).isValid());
    }

    void onlyGlobalShortcutsOnAdvisedButtonsCount()
    {
        const QVector<BindingResolver::Binding> table = {
            row(QStringLiteral("global.screenshot"), ControlId::Capture),
            row(QStringLiteral("global.toggle_overlay"),
                TriggerSpec::orderedChord(ControlId::Guide, ControlId::FaceSouth).serialize()),
            // Overlay-only navigation never fires in game; an unbound row is inert.
            row(QStringLiteral("overlay.navigate_up"), ControlId::Capture),
            row(QStringLiteral("global.save_replay"), ControlId::Guide, true),
        };
        QCOMPARE(SteamInputAdvice::boundControls(table),
                 QStringList({ControlId::Capture, ControlId::Guide}));

        const QVector<BindingResolver::Binding> overlayOnly = {
            row(QStringLiteral("overlay.navigate_up"), ControlId::Guide),
            row(QStringLiteral("global.screenshot"), ControlId::FaceSouth),
        };
        QVERIFY(SteamInputAdvice::boundControls(overlayOnly).isEmpty());
    }

    void reviewedStateRewarnsOnNewlyBoundButton()
    {
        const QStringList both = {ControlId::Capture, ControlId::Guide};
        QCOMPARE(SteamInputAdvice::pendingControls(both, {}), both);

        const QString ack = SteamInputAdvice::serializeAck({ControlId::Capture});
        QCOMPARE(SteamInputAdvice::pendingControls({ControlId::Capture}, ack), QStringList());
        // The user later binds PS as well: only the new button comes back.
        QCOMPARE(SteamInputAdvice::pendingControls(both, ack), QStringList({ControlId::Guide}));
        // "Don't show for this game" hides everything, whatever gets bound later.
        QVERIFY(SteamInputAdvice::pendingControls(
                    both, QString::fromLatin1(SteamInputAdvice::kDismissedAll)).isEmpty());
    }

    void steamLinksOnlyForNumericAppIds()
    {
        QCOMPARE(SteamInputAdvice::gameLayoutUrl(QStringLiteral("1245620")),
                 QStringLiteral("steam://controllerconfig/1245620"));
        QVERIFY(SteamInputAdvice::gameLayoutUrl(QStringLiteral("12/../x")).isEmpty());
        QVERIFY(SteamInputAdvice::gameLayoutUrl({}).isEmpty());
        QCOMPARE(SteamInputAdvice::controllerSettingsUrl(),
                 QStringLiteral("steam://settings/controller"));
    }
};

QTEST_GUILESS_MAIN(SteamInputAdviceTest)
#include "tst_steaminputadvice.moc"
