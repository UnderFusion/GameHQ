#pragma once
#include "storage/CaptureDatabase.h"

#include <QHash>
#include <QObject>
#include <QSet>
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

    // Synchronous full scan; returns number of newly added captures.
    // Called at startup and from the UI "Rescan" action; move to a worker
    // thread once libraries grow beyond a few thousand files.
    int scanAll();

    // Rescans only the given roots (as reported by CaptureFolderWatcher).
    // Files modified less than settleMs ago are left for a later pass so a
    // capture GameHQ is still committing is never indexed with less metadata.
    Delta reconcile(const QStringList& rootPaths, qint64 settleMs);

signals:
    void scanFinished(int added);

private:
    // index is the whole-table snapshot taken by scanAll(); scanFolder reads it
    // instead of querying per file and keeps it current as it inserts.
    // seen collects the stored key of every media file the walk found.
    int scanFolder(const QString& root, const QString& source,
                   QHash<QString, CaptureIndexEntry>& index, QSet<QString>& seen,
                   qint64 settleMs, bool* deferred);
    // Syncs the missing list for live rows; scope limits it to rows under
    // those roots (empty = every row).
    void refreshMissing(const QHash<QString, CaptureIndexEntry>& index,
                        const QSet<QString>& seen, const QStringList& scope, Delta& delta);

    CaptureDatabase* m_db;
    CaptureLocations* m_locations;
    QString m_thumbnailsDir;
};
