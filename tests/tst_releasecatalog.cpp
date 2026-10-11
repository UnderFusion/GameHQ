#include "updates/ReleaseCatalog.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

// Update discovery reads a page of GitHub releases and picks the app release
// out of it. The repo also publishes playnite-v* plugin releases, so a page too
// small could contain none of the app's own tags at all — the checker then saw
// nothing to offer and reported "up to date" forever.
class TestReleaseCatalog : public QObject
{
    Q_OBJECT

private:
    static QJsonObject asset(const QString& name, qint64 size = 1024)
    {
        return QJsonObject{
            { QStringLiteral("name"), name },
            { QStringLiteral("size"), double(size) },
            { QStringLiteral("browser_download_url"),
              QStringLiteral("https://example.invalid/") + name }
        };
    }

    // A complete, installable app release.
    static QJsonObject appRelease(const QString& tag, bool complete = true)
    {
        QJsonArray assets{
            asset(QStringLiteral("GameHQ-%1-win64-update.zip").arg(tag.mid(1))),
            asset(QStringLiteral("GameHQ-%1-win64-update.zip.sha256").arg(tag.mid(1))),
            asset(QStringLiteral("gamehq-release.json"))
        };
        if (complete)
            assets.append(asset(QStringLiteral("gamehq-release.sig")));
        return QJsonObject{
            { QStringLiteral("tag_name"), tag },
            { QStringLiteral("name"), QStringLiteral("GameHQ ") + tag },
            { QStringLiteral("draft"), false },
            { QStringLiteral("prerelease"), false },
            { QStringLiteral("html_url"), QStringLiteral("https://example.invalid/") + tag },
            { QStringLiteral("assets"), assets }
        };
    }

    // A GitHub pre-release: every beta of one version ships the same
    // version-named update archive, so the archive name drops the -betaN tag.
    static QJsonObject betaRelease(const QString& tag, bool prerelease = true)
    {
        const QString version = tag.mid(1).section(QLatin1Char('-'), 0, 0);
        QJsonObject release = appRelease(QStringLiteral("v") + version);
        release[QStringLiteral("tag_name")] = tag;
        release[QStringLiteral("prerelease")] = prerelease;
        return release;
    }

    static QJsonObject pluginRelease(const QString& tag)
    {
        return QJsonObject{
            { QStringLiteral("tag_name"), tag },
            { QStringLiteral("draft"), false },
            { QStringLiteral("prerelease"), false },
            { QStringLiteral("assets"), QJsonArray{ asset(QStringLiteral("plugin.pext")) } }
        };
    }

private slots:
    void picksTheHighestInstallableAppRelease()
    {
        const QJsonArray page{
            pluginRelease(QStringLiteral("playnite-v0.3.0")),
            appRelease(QStringLiteral("v0.6.10")),
            pluginRelease(QStringLiteral("playnite-v0.2.0")),
            appRelease(QStringLiteral("v0.6.9"))
        };
        const auto best = ReleaseCatalog::selectBest(page);
        QVERIFY(best.has_value());
        QCOMPARE(best->version, QStringLiteral("0.6.10"));
        QCOMPARE(best->zipName, QStringLiteral("GameHQ-0.6.10-win64-update.zip"));
        QVERIFY(best->hasCompleteUpdateAssets());
        QVERIFY(!best->checksumUrl.isEmpty());
    }

    void versionsAreComparedNumericallyNotAsText()
    {
        const QJsonArray page{ appRelease(QStringLiteral("v0.6.9")),
                               appRelease(QStringLiteral("v0.6.10")) };
        QCOMPARE(ReleaseCatalog::selectBest(page)->version, QStringLiteral("0.6.10"));
    }

    void skipsWhatItCannotInstallOrVerify()
    {
        // Draft, prerelease, a plugin tag, and a release whose signature asset
        // has not finished uploading: none of them may be offered.
        QJsonObject draft = appRelease(QStringLiteral("v9.9.9"));
        draft[QStringLiteral("draft")] = true;
        QJsonObject pre = appRelease(QStringLiteral("v9.9.8"));
        pre[QStringLiteral("prerelease")] = true;
        const QJsonArray page{ draft, pre,
                               appRelease(QStringLiteral("v9.9.7"), /*complete=*/false),
                               pluginRelease(QStringLiteral("playnite-v9.9.6")),
                               appRelease(QStringLiteral("v0.6.1")) };
        QCOMPARE(ReleaseCatalog::selectBest(page)->version, QStringLiteral("0.6.1"));
    }

    void betasAreOfferedOnlyAfterOptingIn()
    {
        const QJsonArray page{ betaRelease(QStringLiteral("v0.7.10-beta2")),
                               appRelease(QStringLiteral("v0.7.9")) };
        QCOMPARE(ReleaseCatalog::selectBest(page)->identity, QStringLiteral("0.7.9"));

        const auto beta = ReleaseCatalog::selectBest(page, /*includeBetas=*/true);
        QVERIFY(beta.has_value());
        QCOMPARE(beta->identity, QStringLiteral("0.7.10-beta2"));
        QCOMPARE(beta->version, QStringLiteral("0.7.10"));   // what the signed manifest says
        QCOMPARE(beta->zipName, QStringLiteral("GameHQ-0.7.10-win64-update.zip"));
        QVERIFY(beta->prerelease);
        QVERIFY(beta->hasCompleteUpdateAssets());
    }

    void aStableReleaseOutranksEveryBetaOfItsVersion()
    {
        const QJsonArray betas{ betaRelease(QStringLiteral("v0.7.10-beta9")),
                                betaRelease(QStringLiteral("v0.7.10-beta10")),
                                appRelease(QStringLiteral("v0.7.9")) };
        QCOMPARE(ReleaseCatalog::selectBest(betas, true)->identity,
                 QStringLiteral("0.7.10-beta10"));   // numeric, not text

        QJsonArray withStable = betas;
        withStable.append(appRelease(QStringLiteral("v0.7.10")));
        const auto best = ReleaseCatalog::selectBest(withStable, true);
        QCOMPARE(best->identity, QStringLiteral("0.7.10"));
        QVERIFY(!best->prerelease);
    }

    void aMislabelledReleaseNeverCrossesChannels()
    {
        // A pre-release with a stable-looking tag, and a beta tag published as
        // a full release: neither may be offered on either channel.
        const QJsonArray page{ betaRelease(QStringLiteral("v9.9.9"), /*prerelease=*/true),
                               betaRelease(QStringLiteral("v9.9.8-beta1"), /*prerelease=*/false),
                               appRelease(QStringLiteral("v0.6.1")) };
        QCOMPARE(ReleaseCatalog::selectBest(page)->identity, QStringLiteral("0.6.1"));
        QCOMPARE(ReleaseCatalog::selectBest(page, true)->identity, QStringLiteral("0.6.1"));
    }

    void aCachedBetaSurvivesTheSnapshotRoundTrip()
    {
        const auto beta = ReleaseCatalog::selectBest(
            QJsonArray{ betaRelease(QStringLiteral("v0.7.10-beta3")) }, true);
        QVERIFY(beta.has_value());
        const auto restored = ReleaseCatalog::restoreSnapshot(ReleaseCatalog::cacheSnapshot(*beta));
        QVERIFY(restored.has_value());
        QCOMPARE(restored->identity, QStringLiteral("0.7.10-beta3"));
        QVERIFY(restored->prerelease);
    }

    void aPageWithoutAnAppReleaseYieldsNothing()
    {
        QCOMPARE(ReleaseCatalog::selectBest(QJsonArray{}), std::nullopt);
        QCOMPARE(ReleaseCatalog::selectBest(QJsonArray{
                     pluginRelease(QStringLiteral("playnite-v1.0.0")) }), std::nullopt);
    }

    void pagingContinuesOnlyWhileAPageIsFullAndTheCapAllowsIt()
    {
        // A short page is the last page.
        QVERIFY(!ReleaseCatalog::mayHaveMorePages(ReleaseCatalog::kPageSize - 1, 1));
        QVERIFY(ReleaseCatalog::mayHaveMorePages(ReleaseCatalog::kPageSize, 1));
        // And the cap stops it from walking an unbounded history against a
        // 60-requests-an-hour budget.
        QVERIFY(!ReleaseCatalog::mayHaveMorePages(ReleaseCatalog::kPageSize,
                                                  ReleaseCatalog::kMaxPages));
    }

    void bothGitHubRateLimitsAreRecognised()
    {
        const qint64 now = 1'700'000'000;
        // Primary limit: the remaining counter reaches zero and a reset time
        // is given.
        const auto primary = ReleaseCatalog::rateLimitFrom(403, "0", "1700000600", QByteArray(), now);
        QVERIFY(primary.limited);
        QCOMPARE(primary.resetEpochSeconds, qint64(1'700'000'600));

        // Secondary limit: Retry-After seconds and no counter at all. This was
        // read as an ordinary failure, which retried straight back into it.
        const auto secondary = ReleaseCatalog::rateLimitFrom(403, QByteArray(), QByteArray(), "60", now);
        QVERIFY(secondary.limited);
        QCOMPARE(secondary.resetEpochSeconds, now + 60);

        // 429 carries the same meaning.
        QVERIFY(ReleaseCatalog::rateLimitFrom(429, QByteArray(), QByteArray(), "30", now).limited);

        // A limit with no usable time still counts, it just cannot say when.
        const auto vague = ReleaseCatalog::rateLimitFrom(403, QByteArray(), QByteArray(), "soon", now);
        QVERIFY(vague.limited);
        QCOMPARE(vague.resetEpochSeconds, qint64(0));
    }

    void anOrdinaryRefusalIsNotTreatedAsARateLimit()
    {
        const qint64 now = 1'700'000'000;
        // No counter and no Retry-After: calling this a rate limit would stop
        // update checks for everyone behind a shared address.
        QVERIFY(!ReleaseCatalog::rateLimitFrom(403, QByteArray(), QByteArray(), QByteArray(), now).limited);
        // Requests still remaining is not a limit either.
        QVERIFY(!ReleaseCatalog::rateLimitFrom(403, "17", "1700000600", QByteArray(), now).limited);
        // And no other status is one, whatever the headers say.
        QVERIFY(!ReleaseCatalog::rateLimitFrom(200, "0", "1700000600", "60", now).limited);
        QVERIFY(!ReleaseCatalog::rateLimitFrom(500, QByteArray(), QByteArray(), "60", now).limited);
    }
};

QTEST_GUILESS_MAIN(TestReleaseCatalog)
#include "tst_releasecatalog.moc"
