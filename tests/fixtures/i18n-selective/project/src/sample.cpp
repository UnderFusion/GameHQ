#include <QString>

QString fixtureText(const QString &name)
{
    //% "Open %1 in GameHQ"
    const QString launch = qtTrId("gamehq.fixture.launch").arg(name);
    //% "Keep %1 in GameHQ"
    const QString keep = qtTrId("gamehq.fixture.keep").arg(name);
    //% "Remove old entry"
    const QString removed = qtTrId("gamehq.fixture.removed");
    return launch + keep + removed;
}
