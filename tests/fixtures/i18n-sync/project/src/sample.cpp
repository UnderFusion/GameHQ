#include <QString>

QString fixtureText(int count)
{
    //% "Open %1"
    const QString open = qtTrId("gamehq.fixture.open").arg(QStringLiteral("item"));
    //% "<b>%n files</b>"
    const QString files = qtTrId("gamehq.fixture.files", count);
    //% "New message"
    const QString added = qtTrId("gamehq.fixture.new");
    return open + files + added;
}
