#include "ui/AppController.h"
#include "app/StartupManager.h"
#include "capture/AudioSourcePolicy.h"
#include "capture/CaptureUtil.h"
#include "capture/ScreenshotService.h"
#include "config/CaptureLocations.h"
#include "config/ConfigKeys.h"
#include "config/ConfigManager.h"
#include "config/SettingsCategories.h"
#include "config/SettingsApplyPolicy.h"
#include "config/Paths.h"
#include "core/GamePins.h"
#include "core/ProcessIdentity.h"
#include "core/WindowPlacement.h"
#include "diagnostics/Logger.h"
#include "input/InputDiagnostics.h"
#include "localization/NativeText.h"
#include "localization/LanguageManager.h"
#include "storage/CaptureDatabase.h"
#include "sound/SoundEngine.h"
#include "sound/SoundLevels.h"
#include "storage/CaptureFolderWatcher.h"
#include "storage/CaptureScanner.h"
#include "ui/CaptureLibraryService.h"
#include "ui/CurrentGameService.h"
#include "ui/SettingsRouter.h"
#include "ui/GalleryModel.h"
#include "ui/ShellActions.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QDebug>
#include <QGuiApplication>
#include <QSet>
#include <QHash>
#include <QImage>
#include <QFileInfo>
#include <QFile>
#include <QSysInfo>
#include <QCryptographicHash>
#include <QProcess>
#include <QScreen>
#include <QTimer>
#include <QVariantMap>
#include <QVideoFrame>
#include <QVideoSink>

#include <utility>

AppController::AppController(CaptureDatabase* db, CaptureScanner* scanner,
                             GalleryModel* gallery, GalleryModel* overlayGallery,
                             ConfigManager* config, CaptureLocations* locations,
                             StartupManager* startup, LanguageManager* languageManager,
                             QObject* parent)
    : QObject(parent)
    , m_db(db)
    , m_scanner(scanner)
    , m_gallery(gallery)
    , m_overlayGallery(overlayGallery)
    , m_config(config)
    , m_locations(locations)
    , m_startup(startup)
    , m_languageManager(languageManager)
    , m_captureLibrary(std::make_unique<CaptureLibraryService>(db, gallery, overlayGallery))
    , m_currentGame(std::make_unique<CurrentGameService>(db))
    , m_settings(std::make_unique<SettingsRouter>(startup, locations))
    , m_navigation(std::make_unique<NavigationState>(config))
{
    Q_ASSERT(m_languageManager);
    // Releases up to 0.7.10 stored the settings category as a sidebar index.
    m_navigation->migrateLegacyKeys();
    QString releaseNotesError;
    m_releaseNotes = ReleaseNotes::loadBundled(
        m_languageManager->effectiveLanguage(), *m_languageManager->localeRegistry(),
        &releaseNotesError);
    if (!releaseNotesError.isEmpty())
        qWarning() << "Release notes:" << releaseNotesError;
    connect(m_config, &ConfigManager::valueChanged,
            this, &AppController::configChanged);
    connect(m_config, &ConfigManager::valueChanged, this, [this](const QString& key) {
        if (key == ConfigKeys::UiPinnedGames)
            emit gamesChanged();
    });
    connect(m_config, &ConfigManager::groupReset,
            this, &AppController::configGroupReset);
    connect(m_locations, &CaptureLocations::locationsChanged,
            this, &AppController::captureLocationsChanged);

    connect(m_scanner, &CaptureScanner::scanFinished, this, &AppController::onScanFinished);
    // Armed by rescan(), which runs whenever the set of capture roots can change.
    m_folderWatcher = new CaptureFolderWatcher(this);
    connect(m_folderWatcher, &CaptureFolderWatcher::rootsChanged,
            this, &AppController::onCaptureFoldersChanged);

    // Qt's screen topology signals do not fire when Windows' per-display HDR
    // toggle changes. Poll only the lightweight DXGI/DisplayConfig snapshot;
    // the slow HEVC encoder probe remains a startup/manual-refresh operation.
    m_hdrPollTimer = new QTimer(this);
    m_hdrPollTimer->setInterval(1000);
    m_hdrPollTimer->setTimerType(Qt::CoarseTimer);
    connect(m_hdrPollTimer, &QTimer::timeout, this, &AppController::pollHdrStatus);
    m_hdrPollTimer->start();

    m_navigationSaveTimer = new QTimer(this);
    m_navigationSaveTimer->setSingleShot(true);
    m_navigationSaveTimer->setInterval(500);
    connect(m_navigationSaveTimer, &QTimer::timeout, this, [this] { m_config->save(); });
}

AppController::~AppController() = default;

QString AppController::releaseNotesTitle() const
{
    //% "Release notes"
    return qtTrId("gamehq.release_notes.title");
}

void AppController::retranslate()
{
    QString error;
    m_releaseNotes = ReleaseNotes::loadBundled(
        m_languageManager->effectiveLanguage(), *m_languageManager->localeRegistry(), &error);
    if (!error.isEmpty())
        qWarning() << "Release notes:" << error;
    emit translationsChanged();
}

void AppController::requestDesktopFocus()
{
    emit desktopFocusRequested();
}

QStringList AppController::watchedFolders() const
{
    return m_db->watchedFolders();
}

QString AppController::capturesRoot() const
{
    return QDir::toNativeSeparators(Paths::capturesRoot());
}

QString AppController::screenshotsRoot() const
{
    return QDir::toNativeSeparators(m_locations->screenshotsBaseRoot());
}

QString AppController::clipsRoot() const
{
    return QDir::toNativeSeparators(m_locations->clipsBaseRoot());
}

QStringList AppController::managedRoots() const
{
    QStringList roots;
    for (const QString& root : m_locations->managedRoots())
        roots.append(QDir::toNativeSeparators(root));
    return roots;
}

bool AppController::startMinimized() const
{
    return m_config->value(ConfigKeys::StartupMinimized, false).toBool();
}

QString AppController::version() const
{
    return QStringLiteral(GAMEHQ_VERSION);
}

int AppController::currentGameId() const
{
    return m_currentGame->currentGameId();
}

bool AppController::currentGameAvailable() const
{
    return m_currentGame->currentGameAvailable();
}

QString AppController::currentGameExecutableKey() const
{
    return m_currentGame->currentGameExecutableKey();
}

QString AppController::currentGameName() const
{
    return m_currentGame->currentGameName();
}

int AppController::overlayGameId() const
{
    return m_currentGame->currentGameId();
}

// Most recently captured first, with pinned games lifted above the rest.
QVariantList AppController::games() const
{
    const auto entries = m_db->listGames();
    const QStringList pinned = m_config->value(ConfigKeys::UiPinnedGames).toStringList();
    QStringList keys;
    keys.reserve(entries.size());
    for (const GameEntry& g : entries)
        keys.append(GamePins::key(g.id, g.executablePath));
    const QSet<QString> pinSet(pinned.cbegin(), pinned.cend());

    QVariantList out;
    for (int row : GamePins::pinnedFirst(keys, pinned)) {
        const GameEntry& g = entries.at(row);
        QVariantMap item;
        item.insert(QStringLiteral("id"), g.id);
        item.insert(QStringLiteral("name"), g.name);
        item.insert(QStringLiteral("iconPath"), g.iconPath);
        item.insert(QStringLiteral("pinned"), pinSet.contains(keys.at(row)));
        out.append(item);
    }
    return out;
}

void AppController::setGamePinned(int gameId, bool pinned)
{
    for (const GameEntry& g : m_db->listGames()) {
        if (g.id != gameId)
            continue;
        const QStringList before = m_config->value(ConfigKeys::UiPinnedGames).toStringList();
        const QStringList after = GamePins::withPin(before, GamePins::key(g.id, g.executablePath), pinned);
        if (after == before)
            return;
        m_config->setValue(ConfigKeys::UiPinnedGames, after);
        m_config->save();
        qInfo() << "Games:" << (pinned ? "pinned" : "unpinned") << g.name;
        return;
    }
}

void AppController::syncOverlayToForegroundGame()
{
    const bool stateChanged = m_currentGame->syncToForegroundGame();
    if (stateChanged) {
        emit currentGameChanged();
        emit overlayGameChanged();
    }
    if (m_currentGame->lastUpdateChangedGameMetadata())
        emit gamesChanged();
    if (m_overlayGallery) {
        // The game binding always follows the foreground game; only the
        // category the overlay was left on is remembered, per game.
        const int gameId = m_currentGame->currentGameAvailable() ? m_currentGame->currentGameId() : -1;
        const NavigationState::GalleryFilter filter = m_navigation->overlayFilter(gameId);
        m_overlayGallery->setFilter(filter.category, filter.gameId);
    }
}

void AppController::saveVideoFrame(QObject* videoSink, const QString& gameName,
                                   const QString& executablePath)
{
    if (!m_screenshots) {
        qWarning() << "saveVideoFrame: no screenshot service wired";
        return;
    }
    auto* sink = qobject_cast<QVideoSink*>(videoSink);
    if (!sink) {
        qWarning() << "saveVideoFrame: argument is not a QVideoSink";
        return;
    }
    // QVideoSink retains the last presented frame, so this works whether the
    // clip is paused on a chosen frame or grabbed mid-playback.
    const QImage frame = sink->videoFrame().toImage();
    m_screenshots->saveImage(frame, gameName, executablePath);
}

void AppController::updateForegroundGame(const QString& gameName, const QString& executablePath,
                                         bool recognizedGame)
{
    const bool stateChanged = m_currentGame->update(gameName, executablePath, recognizedGame);
    if (stateChanged) {
        emit currentGameChanged();
        emit overlayGameChanged();
    }

    if (m_currentGame->lastUpdateChangedGameMetadata())
        emit gamesChanged();
}

void AppController::updateReplayBufferState(ReplayBufferState::State state, const QString& gameName)
{
    if (m_replayBufferState == state && m_replayBufferGame == gameName)
        return;
    m_replayBufferState = state;
    m_replayBufferGame = gameName;
    emit replayBufferStateChanged();
}

void AppController::rescan()
{
    // The roots can change right now (a folder added or removed); the watcher
    // follows at once, the walk itself runs in the background.
    QStringList roots;
    for (const CaptureScanner::Root& root : m_scanner->roots())
        roots.append(root.path);
    m_folderWatcher->setRoots(roots);
    m_scanner->scanAllAsync();
}

void AppController::onScanFinished(int added)
{
    m_lastScanAdded = added;
    m_gallery->refresh();
    if (m_overlayGallery)
        m_overlayGallery->sync();
    emit gamesChanged();
    emit lastScanChanged();
    if (!m_navigationRestored) {
        m_navigationRestored = true;
        restoreGalleryFilter();
    }
}

void AppController::onCaptureFoldersChanged(const QStringList& roots)
{
    // A capture GameHQ just wrote is committed by its own service with the
    // game's executable path; give that commit time to land first.
    constexpr qint64 kSettleMs = 3000;
    const CaptureScanner::Delta delta = m_scanner->reconcile(roots, kSettleMs);
    if (delta.deferred)
        m_folderWatcher->recheckLater(roots, kSettleMs + 500);
    if (!delta.changed())
        return;
    m_gallery->sync();
    if (m_overlayGallery)
        m_overlayGallery->sync();
    emit gamesChanged();
}

void AppController::toggleFavorite(int row)
{
    m_gallery->toggleFavorite(row);
}

void AppController::deleteCapture(int row) { deleteCaptureFrom(m_gallery, row); }
void AppController::openCapture(int row) { openCaptureFrom(m_gallery, row); }
void AppController::showInFolder(int row) { showInFolderFrom(m_gallery, row); }

void AppController::deleteCaptures(const QVariantList& rows)
{
    // Only a library row that really went away changes the game list; a media
    // file removed without its row leaves the list exactly as it was.
    const CaptureDeletionResult result = m_captureLibrary->deleteCaptures(m_gallery, rows);
    if (result.libraryChanged())
        emit gamesChanged();
    reportDeletionFailures(result);
}

void AppController::deleteCaptureFrom(GalleryModel* model, int row)
{
    const CaptureDeletionResult result = m_captureLibrary->deleteCapture(model, row);
    if (result.libraryChanged())
        emit gamesChanged();
    reportDeletionFailures(result);
}

// A kept row is the recovery, not the explanation: without this the delete
// looks like it did nothing at all. Only the "file still in use" case is
// user-actionable; an inconsistent row is a rescan matter and stays in the log.
// Imported originals kept by policy get their own explanation.
void AppController::reportDeletionFailures(const CaptureDeletionResult& result)
{
    if (result.failed > 0)
        emit captureDeletionFailed(result.failed);
    if (result.keptImported > 0)
        emit captureDeletionKeptImported(result.keptImported);
}

void AppController::openCaptureFrom(GalleryModel* model, int row)
{
    m_captureLibrary->openCapture(model, row);
}

void AppController::showInFolderFrom(GalleryModel* model, int row)
{
    m_captureLibrary->showInFolder(model, row);
}

CaptureCommitOutcome AppController::commitCapture(const QString& filePath, const QString& type,
                                                 const QString& gameName,
                                                 const QString& executablePath)
{
    const CaptureCommitOutcome outcome =
        m_captureLibrary->commitCapture(filePath, type, gameName, executablePath);
    // The capture just made this a library game, so it may own a row.
    const bool stateChanged = m_currentGame->update(gameName, executablePath, true);
    if (stateChanged) {
        emit currentGameChanged();
        emit overlayGameChanged();
    }
    emit gamesChanged();
    return outcome;
}

CaptureCommitOutcome AppController::commitClip(const QString& filePath, const QString& gameName,
                                               const QString& thumbnailPath,
                                               const QString& executablePath)
{
    const CaptureCommitOutcome outcome =
        m_captureLibrary->commitClip(filePath, gameName, thumbnailPath, executablePath);
    // The capture just made this a library game, so it may own a row.
    const bool stateChanged = m_currentGame->update(gameName, executablePath, true);
    if (stateChanged) {
        emit currentGameChanged();
        emit overlayGameChanged();
    }
    emit gamesChanged();
    return outcome;
}

void AppController::rememberGameExecutable(const QString& gameName, const QString& executablePath)
{
    if (!m_db->rememberGameExecutable(gameName, executablePath))
        return;
    emit gamesChanged();
}

void AppController::addWatchedFolder(const QUrl& folderUrl)
{
    const QString path = folderUrl.toLocalFile();
    if (path.isEmpty())
        return;
    m_db->addWatchedFolder(path, QStringLiteral("Imported"));
    emit foldersChanged();
    rescan();
}
