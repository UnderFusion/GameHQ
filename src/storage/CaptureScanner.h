#pragma once
#include "storage/CaptureDatabase.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QThreadPool>
#include <QString>
#include <QStringList>
#include <QVector>

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

signals:
    void scanFinished(int added);

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
        bool deferred = false;                    // a file was too fresh to index
    };
    // The filesystem half of a scan: walks the roots against an index
    // snapshot and decodes thumbnails. Touches no database, so it is safe on
    // a worker thread.
    static WalkResult walk(const QVector<Root>& roots, const QHash<QString, CaptureIndexEntry>& index,
                           qint64 settleMs, const QString& thumbnailsDir);
    // The database half, on this thread. Re-reads the index so a capture
    // another pass inserted in the meantime is not inserted twice.
    int apply(const WalkResult& result, const QStringList& scope, Delta& delta);
    void finishAsyncScan(const WalkResult& result);
    // Syncs the missing list for live rows; scope limits it to rows under
    // those roots (empty = every row).
    void refreshMissing(const QHash<QString, CaptureIndexEntry>& index,
                        const QSet<QString>& seen, const QStringList& scope, Delta& delta);

    CaptureDatabase* m_db;
    CaptureLocations* m_locations;
    QString m_thumbnailsDir;
    QThreadPool m_pool;   // one worker; the destructor waits for it
    bool m_asyncRunning = false;
    bool m_asyncQueued = false;
};
