// Load-status reporting rules for the UI sounds (docs/sound-system.md).
//
// Pure logic: SoundLoadTracker deliberately knows nothing about Qt Multimedia,
// so these cases run without an audio device and without waiting on
// QSoundEffect's asynchronous loading. SoundEngine only maps
// QSoundEffect::Status onto SoundLoadTracker::Status and forwards the verdict.
#include "sound/SoundLoadTracker.h"

#include <QtTest>

using Status = SoundLoadTracker::Status;
using Report = SoundLoadTracker::Report;

class SoundEngineTest : public QObject
{
    Q_OBJECT
private slots:
    void loadingIsNotReported();
    void readyIsReportedOnce();
    void failureIsReportedOnceWithTheEventName();
    void statusAfterResolutionIsIgnored();
    void firstFailureRaisesTheOnlyWarning();
    void warningNamesEveryEventThatFailedSoFar();
    void aRunWithoutFailuresNeverWarns();
};

void SoundEngineTest::loadingIsNotReported()
{
    SoundLoadTracker tracker;
    QCOMPARE(tracker.note(QStringLiteral("nav_tick"), Status::Loading), Report::Nothing);
    QCOMPARE(tracker.note(QStringLiteral("nav_tick"), Status::Loading), Report::Nothing);
    QVERIFY(!tracker.hasFailures());
    QVERIFY(tracker.takeFailureWarning().isEmpty());
}

void SoundEngineTest::readyIsReportedOnce()
{
    SoundLoadTracker tracker;
    QCOMPARE(tracker.note(QStringLiteral("confirm"), Status::Loading), Report::Nothing);
    QCOMPARE(tracker.note(QStringLiteral("confirm"), Status::Ready), Report::Ready);
    // QSoundEffect re-emits statusChanged; the log must not repeat.
    QCOMPARE(tracker.note(QStringLiteral("confirm"), Status::Ready), Report::Nothing);
    QVERIFY(!tracker.hasFailures());
}

void SoundEngineTest::failureIsReportedOnceWithTheEventName()
{
    SoundLoadTracker tracker;
    QCOMPARE(tracker.note(QStringLiteral("screenshot"), Status::Error), Report::Failed);
    QCOMPARE(tracker.note(QStringLiteral("screenshot"), Status::Error), Report::Nothing);
    QVERIFY(tracker.hasFailures());
    QCOMPARE(tracker.failedEvents(), QStringList{QStringLiteral("screenshot")});

    // A source that never resolves is as unusable as a decode error.
    SoundLoadTracker nullTracker;
    QCOMPARE(nullTracker.note(QStringLiteral("error"), Status::Null), Report::Failed);
    QCOMPARE(nullTracker.failedEvents(), QStringList{QStringLiteral("error")});
}

void SoundEngineTest::statusAfterResolutionIsIgnored()
{
    SoundLoadTracker tracker;
    QCOMPARE(tracker.note(QStringLiteral("favorite"), Status::Ready), Report::Ready);
    // Teardown drops the backend back to Null. The effect did load; that is
    // not a new failure and must never reach the user.
    QCOMPARE(tracker.note(QStringLiteral("favorite"), Status::Null), Report::Nothing);
    QVERIFY(!tracker.hasFailures());
    QVERIFY(tracker.takeFailureWarning().isEmpty());
}

void SoundEngineTest::firstFailureRaisesTheOnlyWarning()
{
    SoundLoadTracker tracker;
    QCOMPARE(tracker.note(QStringLiteral("nav_tick"), Status::Error), Report::Failed);
    QCOMPARE(tracker.takeFailureWarning(), QStringLiteral("nav_tick"));

    // Every later failure is still logged, but the visible warning is spent.
    QCOMPARE(tracker.note(QStringLiteral("confirm"), Status::Error), Report::Failed);
    QVERIFY(tracker.takeFailureWarning().isEmpty());
    QCOMPARE(tracker.failedEvents(),
             QStringList({QStringLiteral("nav_tick"), QStringLiteral("confirm")}));
}

void SoundEngineTest::warningNamesEveryEventThatFailedSoFar()
{
    SoundLoadTracker tracker;
    // Two effects fail before anyone asks: the single warning covers both.
    QCOMPARE(tracker.note(QStringLiteral("screenshot"), Status::Error), Report::Failed);
    QCOMPARE(tracker.note(QStringLiteral("replay_saved"), Status::Null), Report::Failed);
    QCOMPARE(tracker.takeFailureWarning(), QStringLiteral("screenshot, replay_saved"));
    QVERIFY(tracker.takeFailureWarning().isEmpty());
}

void SoundEngineTest::aRunWithoutFailuresNeverWarns()
{
    SoundLoadTracker tracker;
    for (const QString& event : {QStringLiteral("nav_tick"), QStringLiteral("confirm"),
                                 QStringLiteral("screenshot")}) {
        QCOMPARE(tracker.note(event, Status::Loading), Report::Nothing);
        QCOMPARE(tracker.note(event, Status::Ready), Report::Ready);
    }
    QVERIFY(!tracker.hasFailures());
    QVERIFY(tracker.takeFailureWarning().isEmpty());
}

QTEST_APPLESS_MAIN(SoundEngineTest)
#include "tst_soundengine.moc"
