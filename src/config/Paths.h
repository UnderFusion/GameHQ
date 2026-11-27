#pragma once
#include <QString>
#include <QStringList>
#include <QVector>

// Resolves all data locations. Portable mode = "portable.flag" file next to the exe
// or in its parent directory (dist layout keeps the exe in app/ and the data at
// the package root — docs/storage.md, docs/packaging.md). All getters return
// absolute paths without trailing slash.
namespace Paths
{
    bool isPortable();

    QString dataDir();        // config.json, gamehq.db
    QString databasePath();   // gamehq.db, adopting a legacy-named DB if present
    QString logsDir();
    QString thumbnailsDir();
    QString gameIconsDir();
    QString replayCacheDir();
    QString soundPacksDir();
    QString capturesRoot();   // per-game capture tree
    QString packageRoot();    // root launcher/.update directory (or exe dir in development)

    // Portable persistence. Values below the package root are stored as
    // "portable:/..." and resolved against the package's current location.
    QString toStoredPath(const QString& path);
    QString fromStoredPath(const QString& path);
    QString repairMovedPath(const QString& path);

    // repairMovedPath() for many paths at once (a library listing): the data
    // roots are resolved once, and a path already under one of them is
    // returned without touching the disk, which is what repairMovedPath()
    // would return for it anyway. Other paths take the same stat and rebase.
    class MovedPathRepairer
    {
    public:
        MovedPathRepairer();
        QString repair(const QString& path) const;

    private:
        struct Root { QString marker; QString root; QString prefix; };
        QString m_portableRoot;
        QVector<Root> m_roots;
    };

    // Same mapping against an explicit portable root ("" = not portable). The
    // real root is a process-wide constant discovered from the executable's
    // location, so these overloads exist to make the mapping itself testable.
    QString toStoredPath(const QString& path, const QString& portableRoot);
    QString fromStoredPath(const QString& path, const QString& portableRoot);

    // Creates every directory above if missing. Call once at startup.
    // Outcome of ensureDirectories(). The data root holds the database and
    // settings, so losing it is fatal; a missing cache or log directory only
    // costs diagnostics and is reported without stopping startup.
    struct DirectoryStatus
    {
        bool essentialReady = true;   // data root and its database-bearing dirs
        QStringList failedEssential;
        QStringList failedOptional;   // logs, caches, thumbnails, sounds
        bool allReady() const { return essentialReady && failedOptional.isEmpty(); }
    };

    DirectoryStatus ensureDirectories();
}
