// Autostart ownership: which Run commands point at this GameHQ copy. A copy
// with autostart off removes the shared `GameHQ` Run value only when it is its
// own, so a portable/dev/smoke launch cannot wipe the installed registration.
#include "app/StartupManager.h"

#include <QDir>
#include <QtTest>

namespace {
QString native(const char* path)
{
    return QDir::toNativeSeparators(QString::fromLatin1(path));
}
QString quoted(const char* path)
{
    return QLatin1Char('"') + native(path) + QLatin1Char('"');
}
}

class TestStartupManager : public QObject
{
    Q_OBJECT
private slots:
    void commandTargets_data()
    {
        QTest::addColumn<QString>("command");
        QTest::addColumn<QString>("executable");
        QTest::addColumn<bool>("expected");
        const QString installed = QStringLiteral("I:/projects/apps/gamehq/installed/GameHQ.exe");
        QTest::newRow("quoted native")
            << quoted("I:/projects/apps/gamehq/installed/GameHQ.exe") << installed << true;
        QTest::newRow("case-insensitive")
            << quoted("i:/PROJECTS/Apps/GameHQ/Installed/GAMEHQ.EXE") << installed << true;
        QTest::newRow("quoted with arguments")
            << quoted("I:/projects/apps/gamehq/installed/GameHQ.exe") + QStringLiteral(" --minimized")
            << installed << true;
        QTest::newRow("bare with arguments")
            << native("I:/projects/apps/gamehq/installed/GameHQ.exe --x") << installed << true;
        QTest::newRow("other copy")
            << quoted("I:/projects/apps/gamehq/build/GameHQ.exe") << installed << false;
        QTest::newRow("empty command") << QString() << installed << false;
    }
    void commandTargets()
    {
        QFETCH(QString, command);
        QFETCH(QString, executable);
        QFETCH(bool, expected);
        QCOMPARE(StartupManager::commandTargets(command, executable), expected);
    }
};

QTEST_GUILESS_MAIN(TestStartupManager)
#include "tst_startupmanager.moc"
