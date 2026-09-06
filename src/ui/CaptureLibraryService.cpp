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
// (a file that cannot be removed keeps its library row, so the capture never
// silently vanishes from the library while still occupying disk), then the row,
// and only then the thumbnail — a thumbnail is regenerable, but while the row
// is still there it is the only preview the library has.
CaptureLibraryService::ItemOutcome CaptureLibraryService::deleteItem(int id, const QString& file,
                                                                    const QString& thumb)
{
    bool mediaRemoved = false;
    if (!file.isEmpty() && m_fileOps.exists(file)) {
        if (!m_fileOps.remove(file)) {
            qWarning() << "Delete: could not remove file, keeping library entry" << id << file;
            return ItemOutcome::MediaFailed;
        }
        mediaRemoved = true;
    }

    if (!m_db->deleteCapture(id)) {
        if (mediaRemoved) {
            // The file is gone and the row is not: report it loudly and keep the
            // row plus its thumbnail so a rescan can reconcile the difference.
            qCritical() << "Delete: file removed but library row survived, rescan needed"
                        << id << file << thumb;
            return ItemOutcome::Inconsistent;
        }
        qWarning() << "Delete: library row could not be removed" << id << file;
        return ItemOutcome::DbFailed;
    }

    if (!thumb.isEmpty() && m_fileOps.exists(thumb) && !m_fileOps.remove(thumb))
        qWarning() << "Delete: could not remove thumbnail (regenerated on demand)" << thumb;
    return ItemOutcome::Removed;
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
    const QString thumb = r->thumbnailPath;

    result.requested = 1;
    switch (deleteItem(id, file, thumb)) {
    case ItemOutcome::Removed:
        ++result.removed;
        qInfo() << "Deleted capture" << id << file;
        break;
    case ItemOutcome::Inconsistent:
        ++result.inconsistent;
        break;
    case ItemOutcome::MediaFailed:
    case ItemOutcome::DbFailed:
        ++result.failed;
        break;
    }

    refreshGalleries();
    return result;
}

CaptureDeletionResult CaptureLibraryService::deleteCaptures(GalleryModel* model,
                                                            const QVariantList& rows)
{
    CaptureDeletionResult result;
    if (!model)
        return result;

    struct Item { int row; int id; QString file; QString thumb; };
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
        items.append({ row, r->id, r->filePath, r->thumbnailPath });
    }
    if (items.isEmpty())
        return result;

    std::sort(items.begin(), items.end(),
              [](const Item& a, const Item& b) { return a.row > b.row; });

    result.requested = int(items.size());
    for (const Item& it : items) {
        switch (deleteItem(it.id, it.file, it.thumb)) {
        case ItemOutcome::Removed:
            ++result.removed;
            break;
        case ItemOutcome::Inconsistent:
            ++result.inconsistent;
            break;
        case ItemOutcome::MediaFailed:
        case ItemOutcome::DbFailed:
            ++result.failed;
            break;
        }
    }

    refreshGalleries();
    qInfo() << "Bulk delete:" << result.removed << "of" << result.requested << "removed,"
            << result.failed << "failed," << result.inconsistent << "inconsistent";
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

void CaptureLibraryService::commitCapture(const QString& filePath, const QString& type,
                                          const QString& gameName, const QString& executablePath)
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
}

void CaptureLibraryService::commitClip(const QString& filePath, const QString& gameName,
                                       const QString& thumbnailPath, const QString& executablePath)
{
    const int id = m_db->insertCapture(filePath, QStringLiteral("video"), gameName,
                                       QDateTime::currentDateTime().toString(Qt::ISODate),
                                       QStringLiteral("GameHQ"), executablePath);
    if (id > 0 && !thumbnailPath.isEmpty())
        m_db->setThumbnail(id, thumbnailPath);
    refreshGalleries();
}

void CaptureLibraryService::refreshGalleries()
{
    if (m_gallery)
        m_gallery->refresh();
    if (m_overlayGallery)
        m_overlayGallery->refresh();
}
