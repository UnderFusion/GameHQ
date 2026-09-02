#include <QString>

QString fixtureText(const QString &name, int count)
{
    //% "<b>%n captures saved to GameHQ</b>"
    const QString captures = qtTrId("gamehq.fixture.captures", count);
    //% "Launch %1 in GameHQ"
    const QString launch = qtTrId("gamehq.fixture.launch").arg(name);
    //% "Keep %1 in GameHQ"
    const QString keep = qtTrId("gamehq.fixture.keep").arg(name);
    return captures + launch + keep;
}
