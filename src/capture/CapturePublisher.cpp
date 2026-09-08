#include "capture/CapturePublisher.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>

namespace CapturePublisher
{
const QString kPendingSuffix = QStringLiteral(".part");

namespace
{
// Enough room for a burst of captures inside one timestamp tick; past this
// something is wrong and failing is better than spinning.
constexpr int kMaxNameAttempts = 1000;
}

Reservation reserve(const QString& dir, const QString& stampFormat, const QString& suffix)
{
    const QString stamp = QDateTime::currentDateTime().toString(stampFormat);
    for (int attempt = 1; attempt <= kMaxNameAttempts; ++attempt) {
        const QString name = attempt == 1
            ? stamp + suffix
            : stamp + QStringLiteral("_%1").arg(attempt) + suffix;
        const QString finalPath = dir + QLatin1Char('/') + name;
        if (QFile::exists(finalPath))
            continue;

        // NewOnly fails when the file already exists, and the check and the
        // creation are one operation. That is the whole point: the previous
        // exists()-then-write could hand the same name to two threads.
        QFile pending(finalPath + kPendingSuffix);
        if (!pending.open(QIODevice::NewOnly | QIODevice::WriteOnly))
            continue;   // another encode owns this name
        pending.close();
        return { finalPath, pending.fileName() };
    }
    qWarning() << "Capture: could not reserve a capture name in" << dir;
    return {};
}

bool publish(const Reservation& reservation, QString* error)
{
    if (!reservation.isValid()) {
        if (error)
            *error = QStringLiteral("no capture file was reserved");
        return false;
    }
    QFile pending(reservation.pendingPath);
    // rename() refuses to clobber, which is what we want: the final name was
    // reserved for this capture and nothing else may have taken it.
    if (!pending.rename(reservation.finalPath)) {
        if (error)
            *error = pending.errorString();
        return false;
    }
    return true;
}

void discard(const Reservation& reservation)
{
    if (!reservation.isValid())
        return;
    if (QFile::exists(reservation.pendingPath) && !QFile::remove(reservation.pendingPath))
        qWarning() << "Capture: could not remove the unfinished" << reservation.pendingPath;
}

QString companionPath(const QString& finalPath, const QString& dir, const QString& suffix)
{
    if (finalPath.isEmpty())
        return {};
    return dir + QLatin1Char('/') + QFileInfo(finalPath).completeBaseName() + suffix;
}

int sweepStale(const QString& root, qint64 maxAgeSecs)
{
    if (root.isEmpty() || !QFileInfo::exists(root))
        return 0;

    const QDateTime now = QDateTime::currentDateTime();
    int removed = 0;
    QDirIterator it(root, { QLatin1Char('*') + kPendingSuffix }, QDir::Files | QDir::NoSymLinks,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QFileInfo info = it.fileInfo();
        if (info.lastModified().secsTo(now) < maxAgeSecs)
            continue;   // an encode may still be running
        if (QFile::remove(info.absoluteFilePath()))
            ++removed;
        else
            qWarning() << "Capture: could not remove the stale" << info.absoluteFilePath();
    }
    if (removed > 0)
        qInfo() << "Capture: removed" << removed << "unfinished capture file(s) under" << root;
    return removed;
}
}
