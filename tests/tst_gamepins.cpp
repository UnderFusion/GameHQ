#include "core/GamePins.h"

#include <QTest>

// Pinned games sort above the rest; each group keeps its most-recent-first order.
class TestGamePins : public QObject
{
    Q_OBJECT

private slots:
    void pinnedRowsComeFirstInRecencyOrder()
    {
        // Database order is most recent first: a, b, c, d.
        const QStringList rows{ "a", "b", "c", "d" };
        QCOMPARE(GamePins::pinnedFirst(rows, { "d", "b" }), (QList<int>{ 1, 3, 0, 2 }));
    }

    void noPinsKeepsOrder()
    {
        QCOMPARE(GamePins::pinnedFirst({ "a", "b" }, {}), (QList<int>{ 0, 1 }));
    }

    void pinsForMissingGamesAreIgnored()
    {
        QCOMPARE(GamePins::pinnedFirst({ "a", "b" }, { "gone", "b" }), (QList<int>{ 1, 0 }));
    }

    void withPinAddsOnceAndRemoves()
    {
        QStringList pins = GamePins::withPin({}, "a", true);
        pins = GamePins::withPin(pins, "a", true);
        QCOMPARE(pins, QStringList{ "a" });
        QCOMPARE(GamePins::withPin(pins, "a", false), QStringList{});
    }

    void keyFallsBackToIdWithoutExecutable()
    {
        QCOMPARE(GamePins::key(7, QString()), QStringLiteral("id:7"));
        QVERIFY(GamePins::key(7, "games/foo/foo.exe") != QStringLiteral("id:7"));
    }
};

QTEST_GUILESS_MAIN(TestGamePins)
#include "tst_gamepins.moc"
