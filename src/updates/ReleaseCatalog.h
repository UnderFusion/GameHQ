#pragma once

#include "updates/ReleaseInfo.h"

#include <QByteArray>
#include <QJsonArray>
#include <QString>

#include <optional>

// The pure half of update discovery: picking the app release out of a GitHub
// releases page, and deciding whether a refused response is a rate limit.
// GitHubReleaseSource keeps only the networking, so all of this is testable
// without touching the network.
namespace ReleaseCatalog
{
// Anonymous GitHub allows 60 requests an hour, so paging has to stay bounded.
// 100 is the API maximum per page and three pages covers years of releases
// even with a plugin tag between every app tag.
inline constexpr int kPageSize = 100;
inline constexpr int kMaxPages = 3;

// The highest non-draft, non-prerelease release whose tag is an exact vX.Y.Z
// app version and which carries the assets this app knows how to install.
// Nothing here establishes trust - it only locates candidate URLs.
std::optional<ReleaseInfo> selectBest(const QJsonArray &releases);
// Bounded discovery snapshot only; package trust is still checked separately.
QByteArray cacheSnapshot(const ReleaseInfo &release);
std::optional<ReleaseInfo> restoreSnapshot(const QByteArray &bytes);

// Whether another page could still hold a newer app release. A short page is
// the last page, so there is nothing more to ask for.
bool mayHaveMorePages(int releasesOnPage, int pagesFetched);

struct RateLimit
{
    bool limited = false;
    qint64 resetEpochSeconds = 0;   // 0 when GitHub did not say when
};

// GitHub signals its primary limit with x-ratelimit-remaining: 0 and its
// secondary limit with Retry-After and no such header. Reading only the first
// made a secondary limit look like an ordinary failure, which then retried
// straight back into it. A bare 403/429 with neither header stays an ordinary
// error: treating that as a limit would stop update checks for everyone behind
// a shared address.
RateLimit rateLimitFrom(int httpStatus, const QByteArray &remainingHeader,
                        const QByteArray &resetHeader, const QByteArray &retryAfterHeader,
                        qint64 nowEpochSeconds);
}
