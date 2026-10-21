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
    m_pool.setMaxThreadCount(1);
}

CaptureScanner::~CaptureScanner()
{
    // The worker posts its result to this object; finish it before going.
    m_pool.waitForDone();
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
    const WalkResult result = walk(roots(), m_db->captureIndex(), 0, m_thumbnailsDir);
    Delta delta;
    apply(result, {}, delta);
    qInfo() << "Scan: finished," << delta.added << "new capture(s),"
            << m_db->missingCaptureKeys().size() << "missing on disk";
    emit scanFinished(delta.added);
    return delta.added;
}

void CaptureScanner::scanAllAsync()
{
    if (m_asyncRunning) {
        m_asyncQueued = true;
        return;
    }
    m_asyncRunning = true;
    const QVector<Root> scanRoots = roots();
    const QHash<QString, CaptureIndexEntry> index = m_db->captureIndex();
    const QString thumbnailsDir = m_thumbnailsDir;
    m_pool.start([this, scanRoots, index, thumbnailsDir] {
        const WalkResult result = walk(scanRoots, index, 0, thumbnailsDir);
        // Queued onto this object's thread; dropped if it is gone by then.
        QMetaObject::invokeMethod(this, [this, result] { finishAsyncScan(result); },
                                  Qt::QueuedConnection);
    });
}

void CaptureScanner::finishAsyncScan(const WalkResult& result)
{
    Delta delta;
    apply(result, {}, delta);
    qInfo() << "Scan: finished in the background," << delta.added << "new capture(s),"
            << m_db->missingCaptureKeys().size() << "missing on disk";
    m_asyncRunning = false;
    emit scanFinished(delta.added);
    if (m_asyncQueued) {
        m_asyncQueued = false;
        scanAllAsync();
    }
}

CaptureScanner::Delta CaptureScanner::reconcile(const QStringList& rootPaths, qint64 settleMs)
{
    Delta delta;
    if (rootPaths.isEmpty())
        return delta;
    QVector<Root> scoped;
    QStringList scope;
    for (const Root& root : roots()) {
        if (!rootPaths.contains(root.path, Qt::CaseInsensitive))
            continue;
        scoped.append(root);
        scope.append(root.path);
    }
    if (scope.isEmpty())
        return delta;
    const WalkResult result = walk(scoped, m_db->captureIndex(), settleMs, m_thumbnailsDir);
    delta.deferred = result.deferred;
    apply(result, scope, delta);
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

CaptureScanner::WalkResult CaptureScanner::walk(const QVector<Root>& roots,
                                                const QHash<QString, CaptureIndexEntry>& index,
                                                qint64 settleMs, const QString& thumbnailsDir)
{
    WalkResult result;
    QSet<QString> foundKeys;   // a path reachable from two roots is found once
    const QDateTime settledBefore = QDateTime::currentDateTimeUtc().addMSecs(-settleMs);
    for (const Root& root : roots) {
        if (root.path.isEmpty() || !QDir(root.path).exists())
            continue;
        QDirIterator it(root.path, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = QDir::cleanPath(it.next());
            const QFileInfo info(path);
            const QString type = typeForSuffix(info.suffix().toLower());
            if (type.isEmpty())
                continue;

            const QString key = CaptureDatabase::storedPathKey(path);
            result.seen.insert(key);
            const auto known = index.constFind(key);
            if (known != index.constEnd()) {
                if (!known->deleted && !ThumbnailService::isUsableThumbnail(known->thumbnailPath)) {
                    const QString thumb = ThumbnailService::ensureThumbnail(path, type, thumbnailsDir);
                    if (!thumb.isEmpty())
                        result.thumbnailFixes.append({ path, thumb });
                }
                continue;
            }
            if (foundKeys.contains(key))
                continue;

            if (settleMs > 0 && info.lastModified().toUTC() > settledBefore) {
                result.deferred = true;
                continue;
            }

            FoundCapture capture;
            capture.path = path;
            capture.key = key;
            capture.type = type;
            capture.game = GameIdentity::inferFromPath(root.path, path);
            capture.createdAt =
                info.birthTime().isValid() ? info.birthTime().toUTC().toString(Qt::ISODate)
                                           : info.lastModified().toUTC().toString(Qt::ISODate);
            capture.source = root.source;
            capture.thumbnail = ThumbnailService::ensureThumbnail(path, type, thumbnailsDir);
            foundKeys.insert(key);
            result.found.append(capture);
        }
    }
    return result;
}

int CaptureScanner::apply(const WalkResult& result, const QStringList& scope, Delta& delta)
{
    QHash<QString, CaptureIndexEntry> index = m_db->captureIndex();
    for (const auto& fix : result.thumbnailFixes)
        m_db->setThumbnailForCapture(fix.first, fix.second);

    int added = 0;
    for (const FoundCapture& capture : result.found) {
        if (index.contains(capture.key))
            continue;   // committed by its own service or another pass meanwhile
        const int id = m_db->insertCapture(capture.path, capture.type, capture.game,
                                           capture.createdAt, capture.source);
        if (id < 0)
            continue;
        if (!capture.thumbnail.isEmpty())
            m_db->setThumbnail(id, capture.thumbnail);
        index.insert(capture.key, { capture.thumbnail, false });
        ++added;
    }
    delta.added += added;
    refreshMissing(index, result.seen, scope, delta);
    return added;
}
