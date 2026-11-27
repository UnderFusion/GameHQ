#include "config/Paths.h"

#include "config/LegacyMigration.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStringList>

namespace
{
QString exeDir()
{
    return QCoreApplication::applicationDirPath();
}

// Portable mode: "portable.flag" next to the exe (dev builds) or one level up
// (dist layout, where the real exe lives in app/ and user data belongs at the
// package root — see docs/packaging.md). Empty string = not portable.
QString portableRoot()
{
    static const QString root = [] {
        const QString exe = exeDir();
        if (QFileInfo::exists(exe + QStringLiteral("/portable.flag")))
            return exe;
        const QString parent = QFileInfo(exe).absolutePath();
        if (QFileInfo::exists(parent + QStringLiteral("/portable.flag")))
            return parent;
        return QString();
    }();
    return root;
}
} // namespace

namespace Paths
{

bool isPortable()
{
    return !portableRoot().isEmpty();
}

QString dataDir()
{
    if (isPortable()) {
        return LegacyMigration::adoptDirectory(
            portableRoot() + QStringLiteral("/gamehq-data"),
            { portableRoot() + QStringLiteral("/saveplay-data"),
              portableRoot() + QStringLiteral("/playhq-data") });
    }
    const QString appData = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return LegacyMigration::adoptDirectory(
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation),
        { appData + QStringLiteral("/SavePlay/SavePlay"),
          appData + QStringLiteral("/PlayHQ/PlayHQ") });
}

QString databasePath()
{
    const QString current = dataDir() + QStringLiteral("/gamehq.db");
    LegacyMigration::adoptFile(current, { dataDir() + QStringLiteral("/saveplay.db"),
                                          dataDir() + QStringLiteral("/playhq.db") });
    return current;
}

QString logsDir()        { return dataDir() + QStringLiteral("/logs"); }
QString thumbnailsDir()  { return dataDir() + QStringLiteral("/thumbnails"); }
QString gameIconsDir()   { return dataDir() + QStringLiteral("/game-icons"); }
QString replayCacheDir() { return dataDir() + QStringLiteral("/replay-cache"); }
QString soundPacksDir()  { return dataDir() + QStringLiteral("/sound-packs"); }

QString packageRoot()
{
    const QFileInfo appDir(exeDir());
    if (appDir.fileName().compare(QStringLiteral("app"), Qt::CaseInsensitive) == 0)
        return appDir.absolutePath();
    return exeDir();
}

QString capturesRoot()
{
    if (isPortable())
        return portableRoot() + QStringLiteral("/Captures");
    const QString movies = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    // Existing capture roots remain where they are: capture paths are persisted
    // absolutely and user media is never moved automatically. New installs use
    // the GameHQ root; CaptureLocations keeps historical roots scanned.
    return LegacyMigration::preferExisting(movies + QStringLiteral("/GameHQ"),
                                           { movies + QStringLiteral("/SavePlay"),
                                             movies + QStringLiteral("/PlayHQ") });
}

QString toStoredPath(const QString& path, const QString& root)
{
    if (path.trimmed().isEmpty())
        return {};
    const QString absolute = QDir::cleanPath(QFileInfo(fromStoredPath(path, root)).absoluteFilePath());
    if (root.isEmpty())
        return absolute;
    const QString relative = QDir(root).relativeFilePath(absolute);
    // On another drive — or for a UNC share — relativeFilePath() gives the
    // candidate back unchanged, still absolute. Such a path is not inside the
    // package and must never be filed under "portable:/", which would later be
    // resolved as <package>/D:/... and lose the capture.
    if (!QDir::isRelativePath(relative))
        return absolute;
    if (relative == QStringLiteral("..") || relative.startsWith(QStringLiteral("../")))
        return absolute;
    return QStringLiteral("portable:/") + relative;
}

QString fromStoredPath(const QString& path, const QString& root)
{
    if (path.trimmed().isEmpty())
        return {};
    const QString clean = QDir::fromNativeSeparators(path.trimmed());
    const QString prefix = QStringLiteral("portable:/");
    if (clean.startsWith(prefix, Qt::CaseInsensitive)) {
        const QString remainder = clean.mid(prefix.size());
        // Recovery for rows an older build wrote: a cross-drive or UNC path
        // could end up behind this prefix even though it was never relative to
        // the package. Honour what it actually is.
        if (!QDir::isRelativePath(remainder))
            return QDir::cleanPath(remainder);
        return root.isEmpty() ? clean : QDir::cleanPath(root + QLatin1Char('/') + remainder);
    }
    if (QDir::isRelativePath(clean) && !root.isEmpty())
        return QDir::cleanPath(root + QLatin1Char('/') + clean);
    return QDir::cleanPath(clean);
}

QString toStoredPath(const QString& path)
{
    return toStoredPath(path, portableRoot());
}

QString fromStoredPath(const QString& path)
{
    return fromStoredPath(path, portableRoot());
}

QString repairMovedPath(const QString& path)
{
    if (path.trimmed().isEmpty())
        return {};
    const QString resolved = fromStoredPath(path);
    if (QFileInfo::exists(resolved))
        return resolved;

    const QString normalized = QDir::fromNativeSeparators(resolved);
    struct Rebase { const char* marker; QString root; };
    const Rebase roots[] = {
        { "/Captures/", capturesRoot() },
        { "/gamehq-data/thumbnails/", thumbnailsDir() },
        { "/gamehq-data/game-icons/", gameIconsDir() },
        { "/gamehq-data/replay-cache/", replayCacheDir() }
    };
    for (const Rebase& candidate : roots) {
        const QString marker = QString::fromLatin1(candidate.marker);
        const qsizetype at = normalized.lastIndexOf(marker, -1, Qt::CaseInsensitive);
        if (at < 0)
            continue;
        const QString rebased = QDir::cleanPath(candidate.root + QLatin1Char('/')
                                                + normalized.mid(at + marker.size()));
        if (QFileInfo::exists(rebased))
            return rebased;
    }
    return resolved;
}

MovedPathRepairer::MovedPathRepairer()
    : m_portableRoot(portableRoot())
{
    const auto add = [this](const char* marker, const QString& root) {
        const QString clean = QDir::cleanPath(QDir::fromNativeSeparators(root));
        m_roots.append({ QString::fromLatin1(marker), clean, clean + QLatin1Char('/') });
    };
    add("/Captures/", capturesRoot());
    add("/gamehq-data/thumbnails/", thumbnailsDir());
    add("/gamehq-data/game-icons/", gameIconsDir());
    add("/gamehq-data/replay-cache/", replayCacheDir());
}

QString MovedPathRepairer::repair(const QString& path) const
{
    if (path.trimmed().isEmpty())
        return {};
    const QString resolved = fromStoredPath(path, m_portableRoot);
    const QString normalized = QDir::fromNativeSeparators(resolved);
    // Already where the data lives now: nothing to repair, existing or not.
    for (const Root& r : m_roots) {
        if (normalized.startsWith(r.prefix, Qt::CaseInsensitive))
            return resolved;
    }
    if (QFileInfo::exists(resolved))
        return resolved;
    for (const Root& r : m_roots) {
        const qsizetype at = normalized.lastIndexOf(r.marker, -1, Qt::CaseInsensitive);
        if (at < 0)
            continue;
        const QString rebased = QDir::cleanPath(r.root + QLatin1Char('/')
                                                + normalized.mid(at + r.marker.size()));
        if (QFileInfo::exists(rebased))
            return rebased;
    }
    return resolved;
}

DirectoryStatus ensureDirectories()
{
    DirectoryStatus status;
    // The data root carries config.json and gamehq.db. Without it there is
    // nothing to start, so it is reported separately from the directories that
    // only hold regenerable material.
    for (const QString& dir : { dataDir(), capturesRoot() }) {
        if (!QDir().mkpath(dir)) {
            status.essentialReady = false;
            status.failedEssential.append(dir);
        }
    }
    for (const QString& dir : { logsDir(), thumbnailsDir(), gameIconsDir(),
                                replayCacheDir(), soundPacksDir() }) {
        if (!QDir().mkpath(dir))
            status.failedOptional.append(dir);
    }
    return status;
}

} // namespace Paths
