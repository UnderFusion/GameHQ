#include "ui/CaptureLibraryService.h"

#include "config/Paths.h"
#include "storage/CaptureDatabase.h"
#include "storage/ThumbnailService.h"
#include "ui/GalleryModel.h"
#include "ui/ShellActions.h"

#include <QDateTime>
#include <QFile>
#include <QList>
#include <QDebug>
#include <algorithm>

CaptureFileOps CaptureFileOps::systemOps()
{
    return CaptureFileOps{
        [](const QString& path) { return QFile::exists(path); },
        [](const QString& path) { return QFile::remove(path); },
        [](const QString& path) { return QFile::moveToTrash(path); },
    };
}

CaptureLibraryService::CaptureLibraryService(CaptureDatabase* db, GalleryModel* gallery,
                                             GalleryModel* overlayGallery, CaptureFileOps fileOps)
    : m_db(db)
    , m_gallery(gallery)
    , m_overlayGallery(overlayGallery)
    , m_fileOps(std::move(fileOps))
{
}

// One capture, in the only order that stays recoverable: the media file first
// (a file that cannot be moved keeps its library row, so the capture never
// silently vanishes from the library while still occupying disk), then the row,
// and only then the thumbnail — a thumbnail is regenerable, but while the row
// is still there it is the only preview the library has.
//
// Only captures GameHQ wrote itself are deleted, and only into the Recycle Bin.
// Files found in imported or watched folders belong to the user or another
// app, so delete leaves both the original and its row alone. A failed move is
// a failure: there is never a fallback to a permanent delete.
CaptureLibraryService::ItemOutcome CaptureLibraryService::deleteItem(int id, const QString& file,
                                                                    const QString& thumb,
                                                                    const QString& source)
{
    if (source != QLatin1String("GameHQ")) {
        qInfo() << "Delete: kept imported original and its library entry" << id << file << source;
        return ItemOutcome::KeptImported;
    }

    bool mediaRemoved = false;
    if (!file.isEmpty() && m_fileOps.exists(file)) {
        if (!m_fileOps.trash(file)) {
            qWarning() << "Delete: could not move file to the Recycle Bin, keeping library entry"
                       << id << file;
            return ItemOutcome::MediaFailed;
        }
        mediaRemoved = true;
    }

    if (!m_db->deleteCapture(id)) {
        if (mediaRemoved) {
            // The file is gone and the row is not: report it loudly and keep the
            // row plus its thumbnail so a rescan can reconcile the difference.
            qCritical() << "Delete: file moved to the Recycle Bin but library row survived,"
                           " rescan needed" << id << file << thumb;
            return ItemOutcome::Inconsistent;
        }
        qWarning() << "Delete: library row could not be removed" << id << file;
        return ItemOutcome::DbFailed;
    }

    if (!thumb.isEmpty() && m_fileOps.exists(thumb) && !m_fileOps.remove(thumb))
        qWarning() << "Delete: could not remove thumbnail (regenerated on demand)" << thumb;
    return ItemOutcome::Removed;
}

void CaptureLibraryService::count(ItemOutcome outcome, CaptureDeletionResult& result)
{
    switch (outcome) {
    case ItemOutcome::Removed:
        ++result.removed;
        break;
    case ItemOutcome::Inconsistent:
        ++result.inconsistent;
        break;
    case ItemOutcome::KeptImported:
        ++result.keptImported;
        break;
    case ItemOutcome::MediaFailed:
    case ItemOutcome::DbFailed:
        ++result.failed;
        break;
    }
}

CaptureDeletionResult CaptureLibraryService::deleteCapture(GalleryModel* model, int row)
{
    CaptureDeletionResult result;
    if (!model)
        return result;
    const CaptureRecord* r = model->record(row);
    if (!r)
        return result;

    const int id = r->id;
    const QString file = r->filePath;

    result.requested = 1;
    const ItemOutcome outcome = deleteItem(id, file, r->thumbnailPath, r->source);
    count(outcome, result);
    if (outcome == ItemOutcome::Removed)
        qInfo() << "Deleted capture to the Recycle Bin" << id << file;

    refreshGalleries();
    return result;
}

CaptureDeletionResult CaptureLibraryService::deleteCaptures(GalleryModel* model,
                                                            const QVariantList& rows)
{
    CaptureDeletionResult result;
    if (!model)
        return result;

    struct Item { int row; int id; QString file; QString thumb; QString source; };
    QList<Item> items;
    items.reserve(rows.size());
    for (const QVariant& v : rows) {
        bool ok = false;
        const int row = v.toInt(&ok);
        if (!ok)
            continue;
        const CaptureRecord* r = model->record(row);
        if (!r)
            continue;
        items.append({ row, r->id, r->filePath, r->thumbnailPath, r->source });
    }
    if (items.isEmpty())
        return result;

    std::sort(items.begin(), items.end(),
              [](const Item& a, const Item& b) { return a.row > b.row; });

    result.requested = int(items.size());
    for (const Item& it : items)
        count(deleteItem(it.id, it.file, it.thumb, it.source), result);

    refreshGalleries();
    qInfo() << "Bulk delete:" << result.removed << "of" << result.requested
            << "moved to the Recycle Bin," << result.failed << "failed,"
            << result.inconsistent << "inconsistent," << result.keptImported
            << "imported originals kept";
    return result;
}

void CaptureLibraryService::openCapture(GalleryModel* model, int row) const
{
    if (!model)
        return;
    if (const CaptureRecord* r = model->record(row))
        ShellActions::openFile(r->filePath);
}

void CaptureLibraryService::showInFolder(GalleryModel* model, int row) const
{
    if (!model)
        return;
    if (const CaptureRecord* r = model->record(row))
        ShellActions::showInFolder(r->filePath);
}

CaptureCommitOutcome CaptureLibraryService::commitCapture(const QString& filePath,
                                                          const QString& type,
                                                          const QString& gameName,
                                                          const QString& executablePath)
{
    const int id = m_db->insertCapture(filePath, type, gameName,
                                       QDateTime::currentDateTime().toString(Qt::ISODate),
                                       QStringLiteral("GameHQ"), executablePath);
    if (id > 0) {
        const QString thumb = ThumbnailService::ensureThumbnail(
            filePath, type, Paths::thumbnailsDir());
        if (!thumb.isEmpty())
            m_db->setThumbnail(id, thumb);
    }
    refreshGalleries();
    return commitOutcome(id, filePath);
}

CaptureCommitOutcome CaptureLibraryService::commitClip(const QString& filePath,
                                                       const QString& gameName,
                                                       const QString& thumbnailPath,
                                                       const QString& executablePath)
{
    const int id = m_db->insertCapture(filePath, QStringLiteral("video"), gameName,
                                       QDateTime::currentDateTime().toString(Qt::ISODate),
                                       QStringLiteral("GameHQ"), executablePath);
    if (id > 0 && !thumbnailPath.isEmpty())
        m_db->setThumbnail(id, thumbnailPath);
    refreshGalleries();
    return commitOutcome(id, filePath);
}

// A rejected insert is not automatically a lost capture: the encoder may have
// written a perfectly good file that only the library missed. Check the disk
// before deciding which of the two things to tell the user.
CaptureCommitOutcome CaptureLibraryService::commitOutcome(int insertedId,
                                                          const QString& filePath) const
{
    if (insertedId > 0)
        return CaptureCommitOutcome::Indexed;
    // A refused insert is not always a missing row: the scanner may have
    // indexed the same file first, and insertCapture refuses duplicates. Ask
    // the library before telling the user it is not there.
    if (m_db->hasCapture(filePath))
        return CaptureCommitOutcome::Indexed;
    const bool onDisk = m_fileOps.exists && m_fileOps.exists(filePath);
    qWarning() << "Capture commit: no library row for" << filePath
               << (onDisk ? "— the file is still on disk" : "— the file is gone too");
    return onDisk ? CaptureCommitOutcome::MediaOnly : CaptureCommitOutcome::Failed;
}

void CaptureLibraryService::refreshGalleries()
{
    if (m_gallery)
        m_gallery->refresh();
    if (m_overlayGallery)
        m_overlayGallery->refresh();
}
