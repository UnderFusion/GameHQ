#pragma once
#include "storage/CaptureDatabase.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QThreadPool>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <memory>

class CaptureLocations;

// Scans the managed captures root + watched folders for media files and
// registers new ones in the database (with thumbnails for images). Rows whose
// media is gone from disk are hidden through CaptureDatabase's session-only
// missing list, never deleted, and shown again once the file is back.
// Game inference: <root>/<Game>/(Screenshots|Clips)/file → "<Game>";
// otherwise the file's parent folder name; fallback "Unknown Game".
class CaptureScanner : public QObject
{
    Q_OBJECT
public:
    CaptureScanner(CaptureDatabase* db, CaptureLocations* locations,
                   QString thumbnailsDir, QObject* parent = nullptr);

    struct Root
    {
        QString path;     // cleaned, '/' separators
        QString source;   // "GameHQ" | "Imported"
    };

    // What one pass changed in the visible library.
    struct Delta
    {
        int added = 0;      // new rows
        int hidden = 0;     // media found missing
        int restored = 0;   // missing media found again
        // A brand-new file was too fresh to index (GameHQ may still be
        // committing it with richer metadata); rescan these roots later.
        bool deferred = false;
        bool changed() const { return added > 0 || hidden > 0 || restored > 0; }
    };

    // Managed roots first, then watched folders, de-duplicated.
    QVector<Root> roots() const;

    ~CaptureScanner() override;

    // Synchronous full scan; returns number of newly added captures.
    int scanAll();
    // The same full scan without blocking the caller: the directory walk and
    // thumbnail decoding run on a worker, only the database writes come back
    // to this thread. Emits scanFinished(). A request while one runs is
    // folded into a single follow-up pass.
    void scanAllAsync();
    bool scanInProgress() const { return m_asyncRunning; }

    // Rescans only the given roots (as reported by CaptureFolderWatcher).
    // Files modified less than settleMs ago are left for a later pass so a
    // capture GameHQ is still committing is never indexed with less metadata.
    Delta reconcile(const QStringList& rootPaths, qint64 settleMs);
    // reconcile() without blocking the caller, on the same single worker as
    // scanAllAsync(). While a pass runs, requests coalesce: roots are merged
    // into one follow-up pass, and a queued full scan absorbs them. A result
    // walked under a root set that has since changed is dropped and replaced
    // by a full scan. Emits foldersReconciled().
    void reconcileAsync(const QStringList& rootPaths, qint64 settleMs);
    bool reconcileQueued() const { return !m_reconcileQueued.isEmpty(); }

signals:
    void scanFinished(int added);
    void foldersReconciled(const QStringList& roots, const CaptureScanner::Delta& delta);

private:
    struct FoundCapture
    {
        QString path;
        QString key;
        QString type;
        QString game;
        QString createdAt;
        QString source;
        QString thumbnail;
    };
    struct WalkResult
    {
        QVector<FoundCapture> found;              // not in the index yet
        QVector<QPair<QString, QString>> thumbnailFixes;   // known path -> new thumbnail
        QSet<QString> seen;                       // stored key of every media file found
        QSet<QString> presentElsewhere;           // unseen rows whose file still exists
        QSet<QString> statted;                    // unseen rows checked on the worker
        bool deferred = false;                    // a file was too fresh to index
        bool cancelled = false;                   // shutdown stopped the walk
    };
    // The filesystem half of a scan: walks the roots against an index
    // snapshot and decodes thumbnails. Touches no database, so it is safe on
    // a worker thread.
    // scope as in refreshMissing(); cancel, when set, stops the walk early.
    static WalkResult walk(const QVector<Root>& roots, const QHash<QString, CaptureIndexEntry>& index,
                           qint64 settleMs, const QString& thumbnailsDir,
                           const QStringList& scope = {}, const std::atomic_bool* cancel = nullptr);
    // The database half, on this thread. Re-reads the index so a capture
    // another pass inserted in the meantime is not inserted twice.
    int apply(const WalkResult& result, const QStringList& scope, Delta& delta);
    // A walk result being written to the database, possibly over several
    // event-loop turns.
    struct ApplyJob
    {
        WalkResult result;
        QHash<QString, CaptureIndexEntry> index;   // re-read when the pass lands
        QStringList scope;
        QVector<Root> walkedRoots;
        Delta delta;
        qsizetype next = 0;                         // next thumbnail fix, then capture
        bool full = false;
    };
    // Writes items until budgetMs passes (-1: all) in one transaction.
    // Returns true once every item is written.
    bool applySlice(ApplyJob& job, qint64 budgetMs);
    void continueApply(const std::shared_ptr<ApplyJob>& job);
    static constexpr qint64 kApplySliceMs = 25;
    // One background pass: a full scan (scope empty) or a reconcile of scope.
    void startPass(bool full, const QStringList& rootPaths, qint64 settleMs);
    void finishPass(bool full, const QVector<Root>& walkedRoots, const QStringList& scope,
                    const WalkResult& result);
    void startQueuedPass();
    // Syncs the missing list for live rows; scope limits it to rows under
    // those roots (empty = every row).
    void refreshMissing(const QHash<QString, CaptureIndexEntry>& index,
                        const WalkResult& result, const QStringList& scope, Delta& delta);

    CaptureDatabase* m_db;
    CaptureLocations* m_locations;
    QString m_thumbnailsDir;
    QThreadPool m_pool;   // one worker; the destructor waits for it
    bool m_asyncRunning = false;
    bool m_asyncQueued = false;               // a full scan follows the current pass
    QStringList m_reconcileQueued;            // roots for a follow-up reconcile
    qint64 m_reconcileQueuedSettleMs = 0;
    // Set by the destructor; the worker stops at the next file instead of
    // holding shutdown for a whole library walk.
    std::shared_ptr<std::atomic_bool> m_cancel = std::make_shared<std::atomic_bool>(false);
};

Q_DECLARE_METATYPE(CaptureScanner::Delta)
