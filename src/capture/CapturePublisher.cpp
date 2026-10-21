#include "capture/CapturePublisher.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QSaveFile>
#include <QSet>

namespace CapturePublisher
{
const QString kPendingSuffix = QStringLiteral(".part");

namespace
{
// Enough room for a burst of captures inside one timestamp tick; past this
// something is wrong and failing is better than spinning.
constexpr int kMaxNameAttempts = 1000;

// Ownership of .part files. A capture root can be a shared folder such as
// Videos or Downloads, where other programs leave their own .part downloads,
// so the sweep may only ever delete files this journal proves GameHQ created.
// Every reservation is recorded before its path is handed out and dropped
// once it is published or discarded; `g_active` holds the ones this process
// is still writing, which the sweep never touches whatever their age.
QMutex g_mutex;
QString g_journalPath;
QSet<QString> g_active;

QString normalized(const QString& path)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(path)).toLower();
}

QStringList readJournalLocked()
{
    QStringList entries;
    if (g_journalPath.isEmpty())
        return entries;
    QFile file(g_journalPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return entries;
    while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine()).trimmed();
        if (!line.isEmpty())
            entries.append(line);
    }
    return entries;
}

void writeJournalLocked(const QStringList& entries)
{
    if (g_journalPath.isEmpty())
        return;
    QSaveFile file(g_journalPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "Capture: could not write the pending-file journal" << g_journalPath;
        return;
    }
    for (const QString& entry : entries)
        file.write(entry.toUtf8() + '\n');
    if (!file.commit())
        qWarning() << "Capture: could not write the pending-file journal" << g_journalPath;
}

void recordReservation(const QString& pendingPath)
{
    QMutexLocker lock(&g_mutex);
    g_active.insert(normalized(pendingPath));
    if (g_journalPath.isEmpty())
        return;
    QFile file(g_journalPath);
    if (file.open(QIODevice::Append | QIODevice::Text))
        file.write(QDir::fromNativeSeparators(pendingPath).toUtf8() + '\n');
    else
        qWarning() << "Capture: could not record" << pendingPath << "in the pending-file journal";
}

// The reservation is no longer being written. A .part still on disk keeps its
// journal entry so a later sweep may clear it.
void releaseReservation(const QString& pendingPath, bool stillOnDisk)
{
    QMutexLocker lock(&g_mutex);
    const QString key = normalized(pendingPath);
    g_active.remove(key);
    if (stillOnDisk)
        return;
    QStringList entries = readJournalLocked();
    const qsizetype before = entries.size();
    entries.removeIf([&key](const QString& entry) { return normalized(entry) == key; });
    if (entries.size() != before)
        writeJournalLocked(entries);
}
}

void setOwnershipJournal(const QString& journalPath)
{
    QMutexLocker lock(&g_mutex);
    g_journalPath = journalPath;
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
        recordReservation(pending.fileName());
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
        releaseReservation(reservation.pendingPath, QFileInfo::exists(reservation.pendingPath));
        return false;
    }
    releaseReservation(reservation.pendingPath, false);
    return true;
}

void discard(const Reservation& reservation)
{
    if (!reservation.isValid())
        return;
    if (QFile::exists(reservation.pendingPath) && !QFile::remove(reservation.pendingPath))
        qWarning() << "Capture: could not remove the unfinished" << reservation.pendingPath;
    releaseReservation(reservation.pendingPath, QFileInfo::exists(reservation.pendingPath));
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

    QMutexLocker lock(&g_mutex);
    if (g_journalPath.isEmpty())
        return 0;   // no proof of ownership, so nothing may be deleted

    const QString rootPrefix = normalized(root) + QLatin1Char('/');
    const QDateTime now = QDateTime::currentDateTime();
    const QString pendingSuffix = kPendingSuffix.toLower();
    const QStringList entries = readJournalLocked();
    QStringList kept;
    int removed = 0;
    for (const QString& entry : entries) {
        const QString key = normalized(entry);
        if (!key.startsWith(rootPrefix) || g_active.contains(key)) {
            kept.append(entry);   // another root's, or still being written
            continue;
        }
        if (!key.endsWith(pendingSuffix))
            continue;   // never a reservation: drop the entry, keep the file
        const QFileInfo info(entry);
        if (!info.exists() || info.isSymLink() || !info.isFile())
            continue;   // published, discarded or replaced by something else
        if (info.lastModified().secsTo(now) < maxAgeSecs) {
            kept.append(entry);   // another instance may still be encoding it
            continue;
        }
        if (QFile::remove(info.absoluteFilePath())) {
            ++removed;
        } else {
            qWarning() << "Capture: could not remove the stale" << info.absoluteFilePath();
            kept.append(entry);
        }
    }
    if (kept.size() != entries.size())
        writeJournalLocked(kept);
    if (removed > 0)
        qInfo() << "Capture: removed" << removed << "unfinished capture file(s) under" << root;
    return removed;
}
}
