#pragma once

#include <QString>
#include <QVariantList>

#include <functional>

class CaptureDatabase;
class GalleryModel;

// What a delete request actually did. Deleting a capture touches two stores —
// the media file on disk and the library row — so a single bool cannot tell a
// real failure from an already-absent file, nor a clean removal from a file
// that is gone while its row survived.
struct CaptureDeletionResult
{
    int requested = 0;
    int removed = 0;        // library rows actually tombstoned
    int failed = 0;         // nothing removed; row and thumbnail kept
    int inconsistent = 0;   // media gone but the row could not be tombstoned

    // The library changed, so views and game lists must be refreshed.
    bool libraryChanged() const { return removed > 0; }
    // Every requested item left the library.
    bool allRemoved() const { return requested > 0 && removed == requested; }
};

// Filesystem seam. Production passes the QFile-backed default; tests inject
// deterministic failures instead of racing real Windows file locks.
struct CaptureFileOps
{
    std::function<bool(const QString&)> exists;
    std::function<bool(const QString&)> remove;

    static CaptureFileOps systemOps();
};

class CaptureLibraryService
{
public:
    CaptureLibraryService(CaptureDatabase* db, GalleryModel* gallery, GalleryModel* overlayGallery,
                          CaptureFileOps fileOps = CaptureFileOps::systemOps());

    CaptureDeletionResult deleteCapture(GalleryModel* model, int row);
    CaptureDeletionResult deleteCaptures(GalleryModel* model, const QVariantList& rows);
    void openCapture(GalleryModel* model, int row) const;
    void showInFolder(GalleryModel* model, int row) const;

    void commitCapture(const QString& filePath, const QString& type,
                       const QString& gameName, const QString& executablePath = QString());
    void commitClip(const QString& filePath, const QString& gameName,
                    const QString& thumbnailPath, const QString& executablePath = QString());

private:
    enum class ItemOutcome {
        Removed,       // media handled and row tombstoned
        MediaFailed,   // media still on disk; row untouched
        DbFailed,      // media was already absent and the row survived
        Inconsistent,  // media deleted but the row survived
    };

    ItemOutcome deleteItem(int id, const QString& file, const QString& thumb);
    void refreshGalleries();

    CaptureDatabase* m_db;
    GalleryModel* m_gallery;
    GalleryModel* m_overlayGallery;
    CaptureFileOps m_fileOps;
};
