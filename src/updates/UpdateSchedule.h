#pragma once

#include <QDateTime>

// When an automatic update check is allowed to run.
//
// Two independent limits apply. The hourly interval is GameHQ's own politeness
// budget. The cooldown is GitHub's: after it reports a rate limit, checking
// again before the reset it named is guaranteed to fail and spends quota that
// is already exhausted. The cooldown used to be reported to the user and then
// forgotten, so the hourly wake walked straight back into the limit.
namespace UpdateSchedule
{
// GitHub's anonymous budget refills hourly, so this is the longest a rate limit
// can sensibly last when it declines to say when it resets.
inline constexpr qint64 kDefaultCooldownSecs = 15 * 60;
inline constexpr int kStartupDelayMs = 2000;
inline constexpr qint64 kAutomaticIntervalSecs = 60 * 60;
// The hourly timer is restarted when a check completes, so it can wake a moment
// before the stored timestamp is a full hour old. Without slack that wake would
// be skipped and the next check would happen two hours later.
inline constexpr qint64 kPeriodicSlackSecs = 60;

// All times UTC. An invalid lastChecked means "never checked"; an invalid
// nextAllowed means "no cooldown". Startup passes intervalSecs = 0 so a recent
// successful check cannot hide a release published since the previous launch.
bool automaticCheckAllowed(const QDateTime &lastChecked, const QDateTime &nextAllowed,
                           const QDateTime &now,
                           qint64 intervalSecs = kAutomaticIntervalSecs);

// Why an automatic check does or does not send a request. Kept beside the
// policy so the wiring only logs the answer and tests can cover every branch.
enum class AutomaticDecision
{
    Request,
    Disabled,      // the user turned automatic checking off
    Busy,          // a check, download or install already owns the service
    Cooldown,      // GitHub rate-limited us and its reset time has not passed
    RecentCheck,   // periodic wake within an hour of the last completed check
};

enum class Trigger
{
    Startup,
    Periodic,
};

// Startup ignores the last successful check: a release published since the
// previous launch must be found on this launch. Periodic wakes wait an hour.
AutomaticDecision decideAutomaticCheck(Trigger trigger, bool enabled, bool busy,
                                       const QDateTime &lastChecked,
                                       const QDateTime &nextAllowed, const QDateTime &now);

// True while a rate-limit cooldown is still in force, which is also what makes
// a manual check report the retry time instead of firing a doomed request.
bool inCooldown(const QDateTime &nextAllowed, const QDateTime &now);

// When a rate limit that resets at `resetAt` allows the next attempt. An
// invalid or already-past reset falls back to a fixed cooldown rather than
// permitting an immediate retry.
QDateTime cooldownUntil(const QDateTime &resetAt, const QDateTime &now,
                        qint64 defaultCooldownSecs = kDefaultCooldownSecs);
}
