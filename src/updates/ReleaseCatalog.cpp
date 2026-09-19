#include "updates/ReleaseCatalog.h"
#include <QJsonDocument>
#include "updates/VersionNumber.h"

#include <QDateTime>
#include <QJsonObject>
#include <QJsonValue>

namespace
{
// Fixed asset names emitted by tools/release-manifest. They are version-free
// on purpose: the version lives inside the signed bytes.
constexpr QLatin1StringView kManifestAssetName("gamehq-release.json");
constexpr QLatin1StringView kSignatureAssetName("gamehq-release.sig");

QString updateZipName(const QString &normalizedVersion)
{
    return QStringLiteral("GameHQ-%1-win64-update.zip").arg(normalizedVersion);
}

QString updateZipChecksumName(const QString &normalizedVersion)
{
    return updateZipName(normalizedVersion) + QStringLiteral(".sha256");
}
}

namespace ReleaseCatalog
{
std::optional<ReleaseInfo> selectBest(const QJsonArray &releases)
{
    // The repo also publishes playnite-v* plugin releases, so /releases/latest
    // cannot be trusted to name the app. Scan the page and keep the highest
    // version that is actually installable; a release without both manifest
    // assets is skipped because there would be nothing to verify it with.
    std::optional<VersionNumber> bestVersion;
    std::optional<ReleaseInfo> best;

    for (const QJsonValue &releaseValue : releases) {
        const QJsonObject obj = releaseValue.toObject();
        if (obj.value(QStringLiteral("draft")).toBool()
            || obj.value(QStringLiteral("prerelease")).toBool())
            continue;

        const auto version = VersionNumber::parse(obj.value(QStringLiteral("tag_name")).toString());
        if (!version.has_value())
            continue;
        const QString normalizedVersion = version->toString();

        const QString expectedZip = updateZipName(normalizedVersion);
        const QString expectedChecksum = updateZipChecksumName(normalizedVersion);
        ReleaseInfo candidate;
        for (const QJsonValue &assetValue : obj.value(QStringLiteral("assets")).toArray()) {
            const QJsonObject asset = assetValue.toObject();
            const QString name = asset.value(QStringLiteral("name")).toString();
            const QString url = asset.value(QStringLiteral("browser_download_url")).toString();
            if (name == expectedZip) {
                candidate.zipUrl = url;
                candidate.zipSize = static_cast<qint64>(asset.value(QStringLiteral("size")).toDouble());
            } else if (name == expectedChecksum) {
                candidate.checksumUrl = url;
            } else if (name == kManifestAssetName) {
                candidate.manifestUrl = url;
            } else if (name == kSignatureAssetName) {
                candidate.signatureUrl = url;
            }
        }
        if (candidate.zipUrl.isEmpty() || candidate.manifestUrl.isEmpty()
            || candidate.signatureUrl.isEmpty())
            continue;   // not an installable app release (or assets still uploading)

        if (bestVersion.has_value() && *version <= *bestVersion)
            continue;

        candidate.version = normalizedVersion;
        candidate.tag = obj.value(QStringLiteral("tag_name")).toString();
        candidate.name = obj.value(QStringLiteral("name")).toString();
        candidate.notes = obj.value(QStringLiteral("body")).toString();
        candidate.publishedAt = QDateTime::fromString(
            obj.value(QStringLiteral("published_at")).toString(), Qt::ISODate);
        candidate.webUrl = obj.value(QStringLiteral("html_url")).toString();
        candidate.zipName = expectedZip;
        candidate.prerelease = false;
        candidate.draft = false;

        bestVersion = version;
        best = candidate;
    }
    return best;
}

bool mayHaveMorePages(int releasesOnPage, int pagesFetched)
{
    return releasesOnPage >= kPageSize && pagesFetched < kMaxPages;
}

RateLimit rateLimitFrom(int httpStatus, const QByteArray &remainingHeader,
                        const QByteArray &resetHeader, const QByteArray &retryAfterHeader,
                        qint64 nowEpochSeconds)
{
    if (httpStatus != 403 && httpStatus != 429)
        return {};

    if (!remainingHeader.isEmpty() && remainingHeader.trimmed().toLongLong() == 0) {
        return { true, resetHeader.isEmpty() ? 0 : resetHeader.trimmed().toLongLong() };
    }
    if (!retryAfterHeader.isEmpty()) {
        // The secondary limit answers with Retry-After in seconds and no
        // remaining counter. Negative or unparsable values mean "later" with
        // no time attached rather than "now".
        bool ok = false;
        const qint64 seconds = retryAfterHeader.trimmed().toLongLong(&ok);
        if (ok && seconds > 0)
            return { true, nowEpochSeconds + seconds };
        return { true, 0 };
    }
    return {};
}
}

QByteArray ReleaseCatalog::cacheSnapshot(const ReleaseInfo &release)
{
    QJsonArray assets;
    auto asset = [&assets](const QString &name, const QString &url, qint64 size) {
        assets.append(QJsonObject{{"name", name}, {"browser_download_url", url}, {"size", size}});
    };
    asset(release.zipName, release.zipUrl, release.zipSize);
    asset(QStringLiteral("gamehq-release.json"), release.manifestUrl, 0);
    asset(QStringLiteral("gamehq-release.sig"), release.signatureUrl, 0);
    QJsonObject object{{"tag_name", release.tag}, {"name", release.name},
        {"body", release.notes.left(128 * 1024)},
        {"published_at", release.publishedAt.toString(Qt::ISODate)},
        {"html_url", release.webUrl}, {"draft", release.draft},
        {"prerelease", release.prerelease}, {"assets", assets}};
    return QJsonDocument(QJsonArray{object}).toJson(QJsonDocument::Compact);
}

std::optional<ReleaseInfo> ReleaseCatalog::restoreSnapshot(const QByteArray &bytes)
{
    if (bytes.isEmpty() || bytes.size() > 1024 * 1024)
        return std::nullopt;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes);
    if (!doc.isArray() || doc.array().size() != 1)
        return std::nullopt;
    const auto release = selectBest(doc.array());
    if (!release || !release->hasCompleteUpdateAssets())
        return std::nullopt;
    return release;
}
