#include "updates/UpdateSchedule.h"

#include <QDateTime>
#include <QTest>

// A rate limit from GitHub was shown to the user and then forgotten, so the
// hourly automatic wake asked again — into a budget that was already spent, and
// restarting GameHQ reset nothing because the cooldown lived only in memory.
class TestUpdateSchedule : public QObject
{
    Q_OBJECT

private:
    static QDateTime at(qint64 epochSeconds)
    {
        return QDateTime::fromSecsSinceEpoch(epochSeconds, QTimeZone::UTC);
    }

    static constexpr qint64 kNow = 1'700'000'000;
    static constexpr qint64 kHour = 60 * 60;
    static constexpr qint64 kDay = 24 * 60 * 60;

private slots:
    void aFirstEverCheckIsAllowed()
    {
        QVERIFY(UpdateSchedule::automaticCheckAllowed(QDateTime(), QDateTime(), at(kNow)));
    }

    void startupDoesNotWaitForARecentSuccessfulCheck()
    {
        // A release can be published immediately after the previous check.
        QVERIFY(UpdateSchedule::automaticCheckAllowed(at(kNow - 60), QDateTime(), at(kNow), 0));
        QVERIFY(UpdateSchedule::automaticCheckAllowed(at(kNow), QDateTime(), at(kNow), 0));
    }

    void startupStillHonoursThePersistedGitHubCooldown()
    {
        QVERIFY(!UpdateSchedule::automaticCheckAllowed(at(kNow - 60), at(kNow + 600), at(kNow), 0));
        QVERIFY(UpdateSchedule::automaticCheckAllowed(at(kNow - 60), at(kNow), at(kNow), 0));
    }

    void periodicChecksRunAfterOneHourRatherThanOneDay()
    {
        QVERIFY(!UpdateSchedule::automaticCheckAllowed(at(kNow - kHour + 1), QDateTime(), at(kNow)));
        QVERIFY(UpdateSchedule::automaticCheckAllowed(at(kNow - kHour), QDateTime(), at(kNow)));
        QVERIFY(UpdateSchedule::automaticCheckAllowed(at(kNow - 2 * kHour), QDateTime(), at(kNow)));
    }

    void aCooldownOutranksADueCheck()
    {
        // Long overdue by the hourly rule, but GitHub said not yet. Asking
        // anyway just spends quota that is already gone.
        QVERIFY(!UpdateSchedule::automaticCheckAllowed(at(kNow - 30 * kDay), at(kNow + 600),
                                                       at(kNow)));
        // Once it expires, the overdue check runs.
        QVERIFY(UpdateSchedule::automaticCheckAllowed(at(kNow - 30 * kDay), at(kNow - 1),
                                                      at(kNow)));
    }

    void cooldownBoundariesAreExact()
    {
        QVERIFY(UpdateSchedule::inCooldown(at(kNow + 1), at(kNow)));
        // The instant it is reached, it is over.
        QVERIFY(!UpdateSchedule::inCooldown(at(kNow), at(kNow)));
        QVERIFY(!UpdateSchedule::inCooldown(at(kNow - 1), at(kNow)));
        // No cooldown recorded at all.
        QVERIFY(!UpdateSchedule::inCooldown(QDateTime(), at(kNow)));
    }

    void aResetTimeIsUsedWhenGitHubGivesOne()
    {
        QCOMPARE(UpdateSchedule::cooldownUntil(at(kNow + 900), at(kNow)), at(kNow + 900));
    }

    void aMissingOrStaleResetStillProducesACooldown()
    {
        // GitHub declined to say when, so back off by a fixed amount rather
        // than retrying immediately.
        QCOMPARE(UpdateSchedule::cooldownUntil(QDateTime(), at(kNow)),
                 at(kNow + UpdateSchedule::kDefaultCooldownSecs));
        // A reset already in the past would otherwise mean "retry now", which
        // is how a limit turns into a retry loop.
        QCOMPARE(UpdateSchedule::cooldownUntil(at(kNow - 60), at(kNow)),
                 at(kNow + UpdateSchedule::kDefaultCooldownSecs));
        QCOMPARE(UpdateSchedule::cooldownUntil(at(kNow), at(kNow)),
                 at(kNow + UpdateSchedule::kDefaultCooldownSecs));
    }

    void startupRequestsEvenFiveMinutesAfterTheLastCheck()
    {
        using namespace UpdateSchedule;
        QCOMPARE(decideAutomaticCheck(Trigger::Startup, true, false, at(kNow - 300), QDateTime(),
                                      at(kNow)),
                 AutomaticDecision::Request);
        // The same wake on the hourly timer would wait.
        QCOMPARE(decideAutomaticCheck(Trigger::Periodic, true, false, at(kNow - 300), QDateTime(),
                                      at(kNow)),
                 AutomaticDecision::RecentCheck);
    }

    void anHourlyWakeSlightlyEarlyStillRequests()
    {
        using namespace UpdateSchedule;
        // The timer restarts when a check completes, so it can wake a moment
        // before the stored timestamp is a full hour old.
        QCOMPARE(decideAutomaticCheck(Trigger::Periodic, true, false, at(kNow - kHour + 5),
                                      QDateTime(), at(kNow)),
                 AutomaticDecision::Request);
        QCOMPARE(decideAutomaticCheck(Trigger::Periodic, true, false, at(kNow - kHour / 2),
                                      QDateTime(), at(kNow)),
                 AutomaticDecision::RecentCheck);
    }

    void disabledAutomaticChecksNeverRequest()
    {
        using namespace UpdateSchedule;
        for (const Trigger trigger : {Trigger::Startup, Trigger::Periodic})
            QCOMPARE(decideAutomaticCheck(trigger, false, false, QDateTime(), QDateTime(), at(kNow)),
                     AutomaticDecision::Disabled);
    }

    void aBusyServiceIsNotInterrupted()
    {
        using namespace UpdateSchedule;
        QCOMPARE(decideAutomaticCheck(Trigger::Periodic, true, true, at(kNow - kDay), QDateTime(),
                                      at(kNow)),
                 AutomaticDecision::Busy);
    }

    void startupWaitsForAGitHubCooldown()
    {
        using namespace UpdateSchedule;
        QCOMPARE(decideAutomaticCheck(Trigger::Startup, true, false, at(kNow - kDay),
                                      at(kNow + 600), at(kNow)),
                 AutomaticDecision::Cooldown);
    }

    void aClockThatWentBackwardsDoesNotLockCheckingOut()
    {
        // A last-check timestamp in the future would otherwise block automatic
        // checks until real time caught up with it.
        QVERIFY(UpdateSchedule::automaticCheckAllowed(at(kNow + 10 * kDay), QDateTime(), at(kNow)));
    }
};

QTEST_GUILESS_MAIN(TestUpdateSchedule)
#include "tst_updateschedule.moc"
