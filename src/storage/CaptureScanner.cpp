#include "storage/CaptureScanner.h"
#include "storage/CaptureDatabase.h"
#include "storage/ThumbnailService.h"
#include "config/CaptureLocations.h"
#include "config/Paths.h"
#include "core/GameIdentity.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QDebug>

#include <algorithm>

namespace
{
const QStringList kImageSuffixes = { "png", "jpg", "jpeg", "bmp", "webp" };
const QStringList kVideoSuffixes = { "mp4", "mkv", "mov", "avi", "webm" };

QString typeForSuffix(const QString& suffix)
{
    if (kImageSuffixes.contains(suffix))
        return QStringLiteral("screenshot");
    if (kVideoSuffixes.contains(suffix))
        return QStringLiteral("video");
    return {};
}
} // namespace

CaptureScanner::CaptureScanner(CaptureDatabase* db, CaptureLocations* locations,
                               QString thumbnailsDir, QObject* parent)
    : QObject(parent)
    , m_db(db)
    , m_locations(locations)
    , m_thumbnailsDir(std::move(thumbnailsDir))
{
}

QVector<CaptureScanner::Root> CaptureScanner::roots() const
{
    QVector<Root> out;
    QStringList seen;
    const auto add = [&](const QString& path, const QString& source) {
        if (path.trimmed().isEmpty())
            return;
        const QString clean = QDir::cleanPath(QDir::fromNativeSeparators(path));
        if (seen.contains(clean, Qt::CaseInsensitive))
            return;
        seen.append(clean);
        out.append({ clean, source });
    };
    if (m_locations) {
        for (const QString& root : m_locations->managedRoots())
            add(root, QStringLiteral("GameHQ"));
    }
    for (const QString& folder : m_db->watchedFolders())
        add(folder, QStringLiteral("Imported"));
    return out;
}

int CaptureScanner::scanAll()
{
    // One snapshot of the captures table up front; the walk below then diffs
    // against memory instead of issuing two queries per file on disk.
    QHash<QString, CaptureIndexEntry> index = m_db->captureIndex();
    QSet<QString> seen;

    Delta delta;
    for (const Root& root : roots())
        delta.added += scanFolder(root.path, root.source, index, seen, 0, nullptr);
    refreshMissing(index, seen, {}, delta);
    qInfo() << "Scan: finished," << delta.added << "new capture(s), indexed" << index.size()
            << "known path(s)," << m_db->missingCaptureKeys().size() << "missing on disk";
    emit scanFinished(delta.added);
    return delta.added;
}

CaptureScanner::Delta CaptureScanner::reconcile(const QStringList& rootPaths, qint64 settleMs)
{
    Delta delta;
    if (rootPaths.isEmpty())
        return delta;
    QHash<QString, CaptureIndexEntry> index = m_db->captureIndex();
    QSet<QString> seen;
    QStringList scope;
    for (const Root& root : roots()) {
        if (!rootPaths.contains(root.path, Qt::CaseInsensitive))
            continue;
        scope.append(root.path);
        delta.added += scanFolder(root.path, root.source, index, seen, settleMs, &delta.deferred);
    }
    if (scope.isEmpty())
        return delta;
    refreshMissing(index, seen, scope, delta);
    if (delta.changed()) {
        qInfo() << "Scan: folder change," << delta.added << "added," << delta.hidden
                << "missing," << delta.restored << "back on disk";
    }
    return delta;
}

void CaptureScanner::refreshMissing(const QHash<QString, CaptureIndexEntry>& index,
                                    const QSet<QString>& seen, const QStringList& scope,
                                    Delta& delta)
{
    const QSet<QString> wasMissing = m_db->missingCaptureKeys();
    QStringList nowMissing;
    QStringList nowPresent;
    for (auto it = index.cbegin(); it != index.cend(); ++it) {
        if (it->deleted)
            continue;
        const QString& key = it.key();
        if (!scope.isEmpty()) {
            const QString resolved = QDir::cleanPath(Paths::fromStoredPath(key));
            const bool inScope = std::any_of(scope.cbegin(), scope.cend(), [&](const QString& root) {
                return resolved.startsWith(root + QLatin1Char('/'), Qt::CaseInsensitive);
            });
            if (!inScope)
                continue;
        }
        // A file the walk just saw is present; anything else (outside every
        // root, moved portable package, offline drive) gets one stat.
        const bool present = seen.contains(key) || QFileInfo::exists(Paths::repairMovedPath(key));
        const bool missing = wasMissing.contains(key);
        if (!present && !missing)
            nowMissing.append(key);
        else if (present && missing)
            nowPresent.append(key);
    }
    delta.hidden += m_db->setCapturesMissing(nowMissing, true);
    delta.restored += m_db->setCapturesMissing(nowPresent, false);
}

int CaptureScanner::scanFolder(const QString& root, const QString& source,
                               QHash<QString, CaptureIndexEntry>& index, QSet<QString>& seen,
                               qint64 settleMs, bool* deferred)
{
    if (root.isEmpty() || !QDir(root).exists())
        return 0;

    const QDateTime settledBefore = QDateTime::currentDateTimeUtc().addMSecs(-settleMs);
    int added = 0;
    QDirIterator it(root, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = QDir::cleanPath(it.next());
        const QFileInfo info(path);
        const QString type = typeForSuffix(info.suffix().toLower());
        if (type.isEmpty())
            continue;

        const QString key = CaptureDatabase::storedPathKey(path);
        seen.insert(key);
        const auto known = index.constFind(key);
        if (known != index.constEnd()) {
            if (!known->deleted && !ThumbnailService::isUsableThumbnail(known->thumbnailPath)) {
                const QString thumb = ThumbnailService::ensureThumbnail(path, type, m_thumbnailsDir);
                if (!thumb.isEmpty() && m_db->setThumbnailForCapture(path, thumb))
                    index[key].thumbnailPath = thumb;
            }
            continue;
        }

        if (settleMs > 0 && info.lastModified().toUTC() > settledBefore) {
            if (deferred)
                *deferred = true;
            continue;
        }

        const QString game = GameIdentity::inferFromPath(root, path);
        const QString createdAt =
            info.birthTime().isValid() ? info.birthTime().toUTC().toString(Qt::ISODate)
                                       : info.lastModified().toUTC().toString(Qt::ISODate);
        const int id = m_db->insertCapture(path, type, game, createdAt, source);
        if (id < 0)
            continue;

        const QString thumb = ThumbnailService::ensureThumbnail(path, type, m_thumbnailsDir);
        if (!thumb.isEmpty())
            m_db->setThumbnail(id, thumb);
        // Keep the snapshot authoritative for the rest of the walk, so a path
        // reachable from two roots is not inserted twice.
        index.insert(key, { thumb, false });
        ++added;
    }
    return added;
}
