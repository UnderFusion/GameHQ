#include "storage/CaptureScanner.h"
#include "core/ShutdownWait.h"
#include "storage/CaptureDatabase.h"
#include "storage/ThumbnailService.h"
#include "config/CaptureLocations.h"
#include "config/Paths.h"
#include "core/GameIdentity.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QTimer>
#include <QDebug>
#include <QElapsedTimer>

#include <algorithm>
#include <utility>

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
    // Cancelled first, so the wait is one file's work, not a whole library.
    m_cancel->store(true);
    ShutdownWait::untilDone("library scan", [this](int ms) { return m_pool.waitForDone(ms); });
}

namespace
{
bool underAnyRoot(const QString& storedKey, const QStringList& scope)
{
    if (scope.isEmpty())
        return true;
    const QString resolved = QDir::cleanPath(Paths::fromStoredPath(storedKey));
    return std::any_of(scope.cbegin(), scope.cend(), [&](const QString& root) {
        return resolved.startsWith(root + QLatin1Char('/'), Qt::CaseInsensitive);
    });
}

bool sameRoots(const QVector<CaptureScanner::Root>& a, const QVector<CaptureScanner::Root>& b)
{
    if (a.size() != b.size())
        return false;
    for (qsizetype i = 0; i < a.size(); ++i) {
        if (a[i].source != b[i].source || a[i].path.compare(b[i].path, Qt::CaseInsensitive) != 0)
            return false;
    }
    return true;
}
} // namespace

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
    startPass(true, {}, 0);
}

void CaptureScanner::reconcileAsync(const QStringList& rootPaths, qint64 settleMs)
{
    if (rootPaths.isEmpty())
        return;
    if (m_asyncRunning) {
        // Bounded: one merged root set, never a queue of passes.
        for (const QString& root : rootPaths) {
            if (!m_reconcileQueued.contains(root, Qt::CaseInsensitive))
                m_reconcileQueued.append(root);
        }
        m_reconcileQueuedSettleMs = qMax(m_reconcileQueuedSettleMs, settleMs);
        return;
    }
    startPass(false, rootPaths, settleMs);
}

void CaptureScanner::startPass(bool full, const QStringList& rootPaths, qint64 settleMs)
{
    const QVector<Root> current = roots();
    QVector<Root> walkRoots;
    QStringList scope;
    if (full) {
        walkRoots = current;
    } else {
        for (const Root& root : current) {
            if (!rootPaths.contains(root.path, Qt::CaseInsensitive))
                continue;
            walkRoots.append(root);
            scope.append(root.path);
        }
        if (scope.isEmpty()) {
            // None of them is a capture root any more: nothing to reconcile.
            emit foldersReconciled(rootPaths, Delta{});
            startQueuedPass();
            return;
        }
    }
    m_asyncRunning = true;
    // Immutable inputs for the worker; the database stays on this thread.
    const QHash<QString, CaptureIndexEntry> index = m_db->captureIndex();
    const QString thumbnailsDir = m_thumbnailsDir;
    const std::shared_ptr<std::atomic_bool> cancel = m_cancel;
    m_pool.start([this, full, current, walkRoots, scope, index, settleMs, thumbnailsDir, cancel] {
        const WalkResult result = walk(walkRoots, index, settleMs, thumbnailsDir, scope, cancel.get());
        if (result.cancelled)
            return;
        // Queued onto this object's thread; dropped if it is gone by then.
        QMetaObject::invokeMethod(
            this, [this, full, current, scope, result] { finishPass(full, current, scope, result); },
            Qt::QueuedConnection);
    });
}

void CaptureScanner::finishPass(bool full, const QVector<Root>& walkedRoots,
                                const QStringList& scope, const WalkResult& result)
{
    auto job = std::make_shared<ApplyJob>();
    job->full = full;
    job->walkedRoots = walkedRoots;
    job->scope = scope;
    job->result = result;
    job->delta.deferred = result.deferred;
    job->index = m_db->captureIndex();
    continueApply(job);
}

// The database half of a background pass, in slices of kApplySliceMs so a
// folder of thousands of new files never holds the GUI thread for long.
void CaptureScanner::continueApply(const std::shared_ptr<ApplyJob>& job)
{
    if (!sameRoots(job->walkedRoots, roots())) {
        // A folder was added, removed or retyped while this pass ran: what is
        // left of it describes a configuration that is gone.
        qInfo() << "Scan: capture roots changed during a background pass; rescanning";
        m_asyncRunning = false;
        m_asyncQueued = true;
        startQueuedPass();
        return;
    }
    if (!applySlice(*job, kApplySliceMs)) {
        QTimer::singleShot(0, this, [this, job] { continueApply(job); });
        return;
    }
    Delta& delta = job->delta;
    refreshMissing(m_db->captureIndex(), job->result, job->scope, delta);
    m_asyncRunning = false;
    if (job->full) {
        qInfo() << "Scan: finished in the background," << delta.added << "new capture(s),"
                << m_db->missingCaptureKeys().size() << "missing on disk";
        emit scanFinished(delta.added);
    } else {
        if (delta.changed()) {
            qInfo() << "Scan: folder change," << delta.added << "added," << delta.hidden
                    << "missing," << delta.restored << "back on disk";
        }
        emit foldersReconciled(job->scope, delta);
    }
    startQueuedPass();
}

void CaptureScanner::startQueuedPass()
{
    if (m_asyncRunning)
        return;   // a slot connected to our signals already started one
    if (m_asyncQueued) {
        // A full pass covers every root, so queued folder changes ride along.
        m_asyncQueued = false;
        m_reconcileQueued.clear();
        m_reconcileQueuedSettleMs = 0;
        startPass(true, {}, 0);
        return;
    }
    if (!m_reconcileQueued.isEmpty()) {
        const QStringList rootPaths = std::exchange(m_reconcileQueued, {});
        const qint64 settleMs = std::exchange(m_reconcileQueuedSettleMs, 0);
        startPass(false, rootPaths, settleMs);
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
    const WalkResult result = walk(scoped, m_db->captureIndex(), settleMs, m_thumbnailsDir, scope);
    delta.deferred = result.deferred;
    apply(result, scope, delta);
    if (delta.changed()) {
        qInfo() << "Scan: folder change," << delta.added << "added," << delta.hidden
                << "missing," << delta.restored << "back on disk";
    }
    return delta;
}

void CaptureScanner::refreshMissing(const QHash<QString, CaptureIndexEntry>& index,
                                    const WalkResult& result, const QStringList& scope,
                                    Delta& delta)
{
    const QSet<QString> wasMissing = m_db->missingCaptureKeys();
    QStringList nowMissing;
    QStringList nowPresent;
    for (auto it = index.cbegin(); it != index.cend(); ++it) {
        if (it->deleted)
            continue;
        const QString& key = it.key();
        if (!underAnyRoot(key, scope))
            continue;
        // A file the walk just saw is present; anything else (outside every
        // root, moved portable package, offline drive) gets one stat, made on
        // the worker for rows it knew about and here only for rows inserted
        // since its snapshot.
        const bool present = result.seen.contains(key) || result.presentElsewhere.contains(key)
            || (!result.statted.contains(key) && QFileInfo::exists(Paths::repairMovedPath(key)));
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
                                                qint64 settleMs, const QString& thumbnailsDir,
                                                const QStringList& scope,
                                                const std::atomic_bool* cancel)
{
    WalkResult result;
    const auto cancelled = [&] {
        if (cancel && cancel->load()) {
            result.cancelled = true;
            return true;
        }
        return false;
    };
    QSet<QString> foundKeys;   // a path reachable from two roots is found once
    const QDateTime settledBefore = QDateTime::currentDateTimeUtc().addMSecs(-settleMs);
    for (const Root& root : roots) {
        if (root.path.isEmpty() || !QDir(root.path).exists())
            continue;
        QDirIterator it(root.path, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            if (cancelled())
                return result;
            const QString path = QDir::cleanPath(it.next());
            const QFileInfo info(path);
            const QString type = typeForSuffix(info.suffix().toLower());
            if (type.isEmpty())
                continue;

            const QString key = CaptureDatabase::storedPathKey(path);
            result.seen.insert(key);
            const auto known = index.constFind(key);
            if (known != index.constEnd()) {
                if (!known->deleted && !ThumbnailService::isCurrentThumbnail(known->thumbnailPath, path, type)) {
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
    // The existence checks refreshMissing() needs for rows the walk did not
    // see; an offline drive can stall each one, so not on the caller's thread.
    for (auto it = index.cbegin(); it != index.cend(); ++it) {
        if (it->deleted || result.seen.contains(it.key()) || !underAnyRoot(it.key(), scope))
            continue;
        if (cancelled())
            return result;
        result.statted.insert(it.key());
        if (QFileInfo::exists(Paths::repairMovedPath(it.key())))
            result.presentElsewhere.insert(it.key());
    }
    return result;
}

bool CaptureScanner::applySlice(ApplyJob& job, qint64 budgetMs)
{
    const qsizetype fixes = job.result.thumbnailFixes.size();
    const qsizetype total = fixes + job.result.found.size();
    if (job.next >= total)
        return true;
    QElapsedTimer timer;
    timer.start();
    const bool batched = m_db->beginWriteBatch();
    while (job.next < total) {
        const qsizetype i = job.next++;
        if (i < fixes) {
            const auto& fix = job.result.thumbnailFixes.at(i);
            m_db->setThumbnailForCapture(fix.first, fix.second);
        } else {
            const FoundCapture& capture = job.result.found.at(i - fixes);
            // Committed by its own service or another pass meanwhile?
            // insertCapture() also re-checks, for rows newer than the index.
            if (job.index.contains(capture.key))
                continue;
            const int id = m_db->insertCapture(capture.path, capture.type, capture.game,
                                               capture.createdAt, capture.source);
            if (id < 0)
                continue;
            if (!capture.thumbnail.isEmpty())
                m_db->setThumbnail(id, capture.thumbnail);
            job.index.insert(capture.key, { capture.thumbnail, false });
            ++job.delta.added;
        }
        if (budgetMs >= 0 && timer.elapsed() >= budgetMs)
            break;
    }
    if (batched)
        m_db->commitWriteBatch();
    return job.next >= total;
}

int CaptureScanner::apply(const WalkResult& result, const QStringList& scope, Delta& delta)
{
    ApplyJob job;
    job.result = result;
    job.scope = scope;
    job.index = m_db->captureIndex();
    applySlice(job, -1);
    delta.added += job.delta.added;
    refreshMissing(job.index, result, scope, delta);
    return job.delta.added;
}
