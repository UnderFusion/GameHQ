#pragma once
#include <QObject>
#include <QVariantList>
#include <QUrl>
#include <memory>
#include "app/ReleaseNotes.h"
#include "capture/HdrCapabilities.h"
#include "ui/GalleryModel.h"

class CaptureDatabase;
class CaptureScanner;
class CaptureLibraryService;
struct CaptureDeletionResult;
class CaptureLocations;
class ConfigManager;
class CurrentGameService;
class LanguageManager;
class ScreenshotService;
class SettingsRouter;
class StartupManager;
class QTimer;

// The single QML-facing bridge ("app" context property). QML never talks to
// engines or Win32 directly — see docs/architecture.md dependency rules.
class AppController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(GalleryModel* gallery READ gallery CONSTANT)
    Q_PROPERTY(QString version READ version CONSTANT)
    Q_PROPERTY(QString releaseNotesTitle READ releaseNotesTitle NOTIFY translationsChanged)
    Q_PROPERTY(QString releaseNotesVersion READ releaseNotesVersion NOTIFY translationsChanged)
    Q_PROPERTY(QString releaseNotesLocale READ releaseNotesLocale NOTIFY translationsChanged)
    Q_PROPERTY(QVariantList releaseNotesSections READ releaseNotesSections NOTIFY translationsChanged)
    Q_PROPERTY(QVariantList releaseNotesReleases READ releaseNotesReleases NOTIFY translationsChanged)
    Q_PROPERTY(QVariantList games READ games NOTIFY gamesChanged)
    Q_PROPERTY(QString category READ category NOTIFY filterChanged)
    Q_PROPERTY(int gameId READ gameId NOTIFY filterChanged)
    Q_PROPERTY(int currentGameId READ currentGameId NOTIFY currentGameChanged)
    Q_PROPERTY(bool currentGameAvailable READ currentGameAvailable NOTIFY currentGameChanged)
    Q_PROPERTY(int overlayGameId READ overlayGameId NOTIFY overlayGameChanged)
    Q_PROPERTY(QStringList watchedFolders READ watchedFolders NOTIFY foldersChanged)
    Q_PROPERTY(QString capturesRoot READ capturesRoot CONSTANT)
    Q_PROPERTY(QString screenshotsRoot READ screenshotsRoot NOTIFY captureLocationsChanged)
    Q_PROPERTY(QString clipsRoot READ clipsRoot NOTIFY captureLocationsChanged)
    Q_PROPERTY(QStringList managedRoots READ managedRoots NOTIFY captureLocationsChanged)
    Q_PROPERTY(int lastScanAdded READ lastScanAdded NOTIFY lastScanChanged)
    Q_PROPERTY(bool lastScanAvailable READ lastScanAvailable NOTIFY lastScanChanged)
    Q_PROPERTY(bool startMinimized READ startMinimized CONSTANT)
    Q_PROPERTY(bool portableMode READ portableMode CONSTANT)
    Q_PROPERTY(QString dataRoot READ dataRoot CONSTANT)
    Q_PROPERTY(QString logsRoot READ logsRoot CONSTANT)
    Q_PROPERTY(bool replayBufferActive READ replayBufferActive NOTIFY replayBufferStateChanged)
    Q_PROPERTY(QString replayBufferGame READ replayBufferGame NOTIFY replayBufferStateChanged)
    Q_PROPERTY(bool hdrDisplayActive READ hdrDisplayActive NOTIFY hdrStatusChanged)
    Q_PROPERTY(QString hdrStatusText READ hdrStatusText NOTIFY hdrStatusChanged)
    Q_PROPERTY(QString hdrDetailText READ hdrDetailText NOTIFY hdrStatusChanged)

public:
    AppController(CaptureDatabase* db, CaptureScanner* scanner,
                  GalleryModel* gallery, GalleryModel* overlayGallery,
                  ConfigManager* config, CaptureLocations* locations,
                  StartupManager* startup, LanguageManager* languageManager,
                  QObject* parent = nullptr);
    ~AppController() override;

    GalleryModel* gallery() const { return m_gallery; }
    QString version() const;
    QString releaseNotesTitle() const;
    QString releaseNotesVersion() const { return m_releaseNotes.version(); }
    QString releaseNotesLocale() const { return m_releaseNotes.locale(); }
    QVariantList releaseNotesSections() const { return m_releaseNotes.sections(); }
    QVariantList releaseNotesReleases() const { return m_releaseNotes.releases(); }
    QVariantList games() const;
    QString category() const { return m_category; }
    int gameId() const { return m_gameId; }
    int currentGameId() const;
    bool currentGameAvailable() const;
    int overlayGameId() const;
    QStringList watchedFolders() const;
    QString capturesRoot() const;
    QString screenshotsRoot() const;
    QString clipsRoot() const;
    QStringList managedRoots() const;
    int lastScanAdded() const { return m_lastScanAdded; }
    bool lastScanAvailable() const { return m_lastScanAdded >= 0; }
    bool startMinimized() const;
    bool portableMode() const;
    QString dataRoot() const;
    QString logsRoot() const;
    bool replayBufferActive() const { return m_replayBufferActive; }
    QString replayBufferGame() const { return m_replayBufferGame; }
    bool hdrDisplayActive() const;
    QString hdrStatusText() const;
    QString hdrDetailText() const;

    // Re-probes display HDR state and encoder support, logs the full report and
    // updates the Advanced page. Called once at startup and from its Refresh
    // button — HDR is a runtime toggle, so a cached answer goes stale silently.
    Q_INVOKABLE void refreshHdrStatus();

    Q_INVOKABLE void setCategory(const QString& category);
    Q_INVOKABLE void setGame(int gameId);
    Q_INVOKABLE void setGameCategory(const QString& category, int gameId);
    Q_INVOKABLE void rescan();
    Q_INVOKABLE void toggleFavorite(int row);
    Q_INVOKABLE void deleteCapture(int row);     // removes file + thumbnail, tombstones DB row
    Q_INVOKABLE void deleteCaptures(const QVariantList& rows);  // bulk: sorts desc, single refresh
    Q_INVOKABLE void openCapture(int row);       // default app (viewer/player)
    Q_INVOKABLE void showInFolder(int row);      // Explorer with file selected

    // Same three actions, but against an explicit model/row — needed by the
    // overlay's own GalleryModel instance (milestone 0.6), whose row indices
    // are meaningless against the main window's m_gallery.
    Q_INVOKABLE void deleteCaptureFrom(GalleryModel* model, int row);
    Q_INVOKABLE void openCaptureFrom(GalleryModel* model, int row);
    Q_INVOKABLE void showInFolderFrom(GalleryModel* model, int row);
    Q_INVOKABLE void syncOverlayToForegroundGame();

    // Save the frame currently shown by a QML video surface (its QVideoSink) as
    // a screenshot under the clip's game. Called from the overlay/lightbox when
    // Share is pressed while a clip is focused — the live frame lives in QML, so
    // C++ receives the sink and extracts the image from it.
    Q_INVOKABLE void saveVideoFrame(QObject* videoSink, const QString& gameName,
                                    const QString& executablePath = QString());
    void setScreenshotService(ScreenshotService* screenshots) { m_screenshots = screenshots; }

    Q_INVOKABLE void addWatchedFolder(const QUrl& folderUrl);
    Q_INVOKABLE void removeWatchedFolder(const QString& path);
    Q_INVOKABLE QString setCaptureRoot(const QString& kind, const QUrl& folderUrl);
    Q_INVOKABLE QString resetCaptureRoot(const QString& kind);
    Q_INVOKABLE void openDataFolder();
    Q_INVOKABLE void openLogsFolder();
    // A QML top-level child (the desktop lightbox) has closed. Ask App to
    // restore the native foreground to the desktop window; requestActivate()
    // alone is not reliable under the Windows foreground lock.
    Q_INVOKABLE void requestDesktopFocus();
    Q_INVOKABLE QString beginPortableImport(const QUrl& folderUrl);
    Q_INVOKABLE void quitApplication();
    // Copies version/mode/paths to the clipboard for bug reports (Advanced page).
    Q_INVOKABLE void copyDiagnosticSummary() const;
    // Restores one settings category (a small set of related config prefixes)
    // to defaults; unlike resetAllConfig, other categories are untouched.
    Q_INVOKABLE void resetCategory(const QString& category);

    // Records a freshly-captured file (screenshot/clip) in the DB, builds its
    // thumbnail and refreshes the gallery + games sidebar. Called from App when
    // ScreenshotService (0.4) / ReplayService (0.5) produce a file.
    void commitCapture(const QString& filePath, const QString& type,
                       const QString& gameName, const QString& executablePath = QString());

    // Like commitCapture, but for a saved replay clip (type "video") whose video
    // thumbnail was already produced by the exporter (ThumbnailService is image-only).
    void commitClip(const QString& filePath, const QString& gameName,
                    const QString& thumbnailPath, const QString& executablePath = QString());
    void rememberGameExecutable(const QString& gameName, const QString& executablePath);
    void updateForegroundGame(const QString& gameName, const QString& executablePath);
    // FramePumpService::recordingStateChanged — drives the Replay Settings buffer-state row.
    void updateReplayBufferState(bool active, const QString& gameName);
    void retranslate();

    // config.json access (flat dotted keys, see ConfigManager); setConfig saves.
    Q_INVOKABLE QVariant config(const QString& key, const QVariant& fallback) const;
    Q_INVOKABLE void setConfig(const QString& key, const QVariant& value);
    Q_INVOKABLE QVariant configDefault(const QString& key, const QVariant& fallback = {}) const;
    Q_INVOKABLE bool configIsDefault(const QString& key) const;
    Q_INVOKABLE void resetConfig(const QString& key);
    Q_INVOKABLE void resetConfigGroup(const QString& prefix);
    Q_INVOKABLE void resetAllConfig();

signals:
    void gamesChanged();
    void filterChanged();
    void currentGameChanged();
    void overlayGameChanged();
    void foldersChanged();
    void captureLocationsChanged();
    void configChanged(const QString& key, const QVariant& value);
    void configGroupReset(const QString& prefix);
    // Any replay.* config key changed (fps, resolution, length, ...).
    void replaySettingsChanged();
    void replayBufferStateChanged();
    void lastScanChanged();
    void hdrStatusChanged();
    void translationsChanged();
    // Emitted only after a previously-probed display changes HDR/topology state.
    void hdrDisplayConfigurationChanged();
    void desktopFocusRequested();
    // At least one capture could not be deleted: its media file survived, so the
    // row stayed. Carries how many failed, so the UI can word it for one or many.
    void captureDeletionFailed(int count);

private:
    void pollHdrStatus();
    void reportDeletionFailures(const CaptureDeletionResult& result);

    CaptureDatabase* m_db;
    CaptureScanner* m_scanner;
    GalleryModel* m_gallery;
    GalleryModel* m_overlayGallery;
    ConfigManager* m_config;
    CaptureLocations* m_locations;
    StartupManager* m_startup;
    LanguageManager* m_languageManager;
    ScreenshotService* m_screenshots = nullptr;   // owned by App; set post-construction
    std::unique_ptr<CaptureLibraryService> m_captureLibrary;
    std::unique_ptr<CurrentGameService> m_currentGame;
    std::unique_ptr<SettingsRouter> m_settings;
    ReleaseNotes m_releaseNotes;
    QString m_category = QStringLiteral("all");
    int m_gameId = -1;
    bool m_replayBufferActive = false;
    QString m_replayBufferGame;
    capture::HdrReport m_hdr;
    bool m_hdrProbed = false;
    QTimer* m_hdrPollTimer = nullptr;
    int m_lastScanAdded = -1;   // -1 = not yet scanned this session
};
