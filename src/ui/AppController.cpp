#include "ui/AppController.h"
#include "app/StartupManager.h"
#include "capture/ScreenshotService.h"
#include "config/CaptureLocations.h"
#include "config/ConfigKeys.h"
#include "config/ConfigManager.h"
#include "config/SettingsCategories.h"
#include "config/Paths.h"
#include "core/ProcessIdentity.h"
#include "diagnostics/Logger.h"
#include "input/InputDiagnostics.h"
#include "storage/CaptureDatabase.h"
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
#include <QHash>
#include <QImage>
#include <QFileInfo>
#include <QProcess>
#include <QTimer>
#include <QVariantMap>
#include <QVideoFrame>
#include <QVideoSink>

#include <utility>

namespace
{
bool parseCaptureKind(const QString& value, CaptureLocations::Kind& kind)
{
    if (value == QStringLiteral("screenshots")) {
        kind = CaptureLocations::Kind::Screenshots;
        return true;
    }
    if (value == QStringLiteral("clips")) {
        kind = CaptureLocations::Kind::Clips;
        return true;
    }
    return false;
}

// replay.clip_sound / replay.clip_notify only affect the "saved" toast, not the
// recording pipeline — rearming the buffer for them would discard its ring for
// no benefit. Every other replay.* key (and audio.enabled) is an actual
// recording parameter and must re-arm a running buffer to take effect.
bool isReplayBufferParamKey(const QString& key)
{
    if (key == ConfigKeys::ReplayClipSound || key == ConfigKeys::ReplayClipNotify)
        return false;
    return key.startsWith(QStringLiteral("replay.")) || key == ConfigKeys::AudioEnabled;
}
}

AppController::AppController(CaptureDatabase* db, CaptureScanner* scanner,
                             GalleryModel* gallery, GalleryModel* overlayGallery,
                             ConfigManager* config, CaptureLocations* locations,
                             StartupManager* startup,
                             QObject* parent)
    : QObject(parent)
    , m_db(db)
    , m_scanner(scanner)
    , m_gallery(gallery)
    , m_overlayGallery(overlayGallery)
    , m_config(config)
    , m_locations(locations)
    , m_startup(startup)
    , m_captureLibrary(std::make_unique<CaptureLibraryService>(db, gallery, overlayGallery))
    , m_currentGame(std::make_unique<CurrentGameService>(db))
    , m_settings(std::make_unique<SettingsRouter>(startup, locations))
    , m_releaseNotes(ReleaseNotes::loadBundled())
{
    connect(m_config, &ConfigManager::valueChanged,
            this, &AppController::configChanged);
    connect(m_config, &ConfigManager::groupReset,
            this, &AppController::configGroupReset);
    connect(m_locations, &CaptureLocations::locationsChanged,
            this, &AppController::captureLocationsChanged);

    // Qt's screen topology signals do not fire when Windows' per-display HDR
    // toggle changes. Poll only the lightweight DXGI/DisplayConfig snapshot;
    // the slow HEVC encoder probe remains a startup/manual-refresh operation.
    m_hdrPollTimer = new QTimer(this);
    m_hdrPollTimer->setInterval(1000);
    m_hdrPollTimer->setTimerType(Qt::CoarseTimer);
    connect(m_hdrPollTimer, &QTimer::timeout, this, &AppController::pollHdrStatus);
    m_hdrPollTimer->start();
}

AppController::~AppController() = default;

QString AppController::releaseNotesTitle() const
{
    //% "Release notes"
    return qtTrId("gamehq.release_notes.title");
}

void AppController::retranslate()
{
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

bool AppController::portableMode() const
{
    return Paths::isPortable();
}

QString AppController::dataRoot() const
{
    return QDir::toNativeSeparators(Paths::dataDir());
}

QString AppController::logsRoot() const
{
    return QDir::toNativeSeparators(Paths::logsDir());
}

void AppController::openDataFolder()
{
    ShellActions::openFile(Paths::dataDir());
}

void AppController::openLogsFolder()
{
    ShellActions::openFile(Paths::logsDir());
}

QString AppController::beginPortableImport(const QUrl& folderUrl)
{
    if (portableMode())
        return QStringLiteral("Portable profiles can only be imported by an installed copy of GameHQ.");
    if (!folderUrl.isLocalFile())
        return QStringLiteral("Select a local GameHQ portable folder.");
    const QString source = QDir::cleanPath(folderUrl.toLocalFile());
    if (!QFileInfo(source + QStringLiteral("/portable.flag")).isFile()
        || !QFileInfo(source + QStringLiteral("/GameHQ.exe")).isFile()
        || !QFileInfo(source + QStringLiteral("/gamehq-data")).isDir())
        return QStringLiteral("The selected folder is not a GameHQ portable package.");

    // Identify this process by more than its id: the importer refuses to start
    // until it can prove *this* instance has exited, and a bare id can name a
    // different program once Windows recycles it.
    const QString identity = ProcessIdentity::currentToken();
    if (identity.isEmpty())
        return QStringLiteral("GameHQ could not identify its own process for the import.");
    const QStringList arguments {
        QStringLiteral("--import-portable"), source,
        QStringLiteral("--wait-for-pid"), identity
    };
    if (!QProcess::startDetached(QCoreApplication::applicationFilePath(), arguments))
        return QStringLiteral("GameHQ could not start the portable import process.");
    QCoreApplication::quit();
    return {};
}

void AppController::quitApplication()
{
    QCoreApplication::quit();
}

void AppController::copyDiagnosticSummary() const
{
    // Whether the log is actually being written is the first thing worth
    // knowing when a report arrives without one attached.
    const QString summary = QStringLiteral(
        "GameHQ %1 (%2)\n"
        "Data folder: %3\n"
        "Logs folder: %4 (%5)\n"
        "Screenshots folder: %6\n"
        "Clips folder: %7\n"
        "%8\n"
        "%9")
        .arg(version(), portableMode() ? QStringLiteral("portable") : QStringLiteral("installed"),
             dataRoot(), logsRoot(),
             Logger::writingToFile() ? QStringLiteral("writing")
                                     : QStringLiteral("NOT writable — logging to stderr"),
             screenshotsRoot(), clipsRoot(),
             m_hdr.summaryLines().join(QLatin1Char('\n')),
             InputDiagnostics::instance().exportText());
    QGuiApplication::clipboard()->setText(summary);
}

void AppController::refreshHdrStatus()
{
    const bool experimentalHdrEnabled =
        m_config->value(ConfigKeys::InternalCaptureExperimentalHdr,
                        ConfigKeys::InternalCaptureExperimentalHdrDefault).toBool();
    const capture::HdrReport refreshed =
        capture::HdrCapabilities::query(experimentalHdrEnabled);
    const bool displayChanged =
        m_hdrProbed && !capture::HdrCapabilities::sameDisplayState(m_hdr, refreshed);
    m_hdr = refreshed;
    m_hdrProbed = true;
    capture::HdrCapabilities::logReport(m_hdr);
    emit hdrStatusChanged();
    if (displayChanged)
        emit hdrDisplayConfigurationChanged();
}

void AppController::pollHdrStatus()
{
    if (!m_hdrProbed)
        return;

    const bool experimentalHdrEnabled =
        m_config->value(ConfigKeys::InternalCaptureExperimentalHdr,
                        ConfigKeys::InternalCaptureExperimentalHdrDefault).toBool();
    capture::HdrReport refreshed =
        capture::HdrCapabilities::refreshDisplayState(m_hdr, experimentalHdrEnabled);
    if (capture::HdrCapabilities::sameDisplayState(m_hdr, refreshed))
        return;

    m_hdr = std::move(refreshed);
    capture::HdrCapabilities::logReport(m_hdr);
    emit hdrStatusChanged();
    emit hdrDisplayConfigurationChanged();
}

bool AppController::hdrDisplayActive() const
{
    return m_hdr.anyHdrActive;
}

QString AppController::hdrStatusText() const
{
    if (!m_hdrProbed)
        return QStringLiteral("Not checked yet");
    if (m_hdr.outputs.isEmpty())
        return QStringLiteral("No displays reported by the graphics driver");
    return m_hdr.anyHdrActive ? QStringLiteral("Windows HDR is active")
                              : QStringLiteral("Windows HDR is inactive");
}

QString AppController::hdrDetailText() const
{
    if (!m_hdrProbed)
        return QStringLiteral("Check the current HDR state of every display.");
    return m_hdr.summaryLines().join(QLatin1Char('\n'));
}

QString AppController::setCaptureRoot(const QString& kindValue, const QUrl& folderUrl)
{
    CaptureLocations::Kind kind;
    if (!parseCaptureKind(kindValue, kind))
        return QStringLiteral("The capture type is invalid.");
    if (!folderUrl.isLocalFile())
        return QStringLiteral("Choose a local folder.");

    QString error;
    if (!m_locations->setBaseRoot(kind, folderUrl.toLocalFile(), &error))
        return error;
    rescan();
    return {};
}

QString AppController::resetCaptureRoot(const QString& kindValue)
{
    CaptureLocations::Kind kind;
    if (!parseCaptureKind(kindValue, kind))
        return QStringLiteral("The capture type is invalid.");

    QString error;
    if (!m_locations->resetBaseRoot(kind, &error))
        return error;
    rescan();
    return {};
}

void AppController::removeWatchedFolder(const QString& path)
{
    m_db->removeWatchedFolder(path);
    emit foldersChanged();
}

QVariant AppController::config(const QString& key, const QVariant& fallback) const
{
    return m_config->value(key, fallback);
}

void AppController::setConfig(const QString& key, const QVariant& value)
{
    switch (m_settings->change(key, value)) {
    case SettingsRouter::Outcome::Rejected:
        emit configChanged(key, m_config->value(key, false));
        return;
    case SettingsRouter::Outcome::Rescan:
        rescan();
        return;
    case SettingsRouter::Outcome::Consumed:
        return;
    case SettingsRouter::Outcome::Plain:
        break;
    }
    m_config->setValue(key, value);
    m_config->save();
    // A recording buffer armed with the old fps/resolution/length must pick
    // up the change — App connects this to FramePumpService::restartBuffer.
    if (isReplayBufferParamKey(key))
        emit replaySettingsChanged();
}

QVariant AppController::configDefault(const QString& key, const QVariant& fallback) const
{
    return m_config->defaultValue(key, fallback);
}

bool AppController::configIsDefault(const QString& key) const
{
    return m_config->isDefault(key);
}

void AppController::resetConfig(const QString& key)
{
    switch (m_settings->reset(key)) {
    case SettingsRouter::Outcome::Rejected:
        emit configChanged(key, m_config->value(key, false));
        return;
    case SettingsRouter::Outcome::Rescan:
        rescan();
        return;
    case SettingsRouter::Outcome::Consumed:
        return;
    case SettingsRouter::Outcome::Plain:
        break;
    }
    if (!m_config->resetValue(key))
        return;
    m_config->save();
    if (isReplayBufferParamKey(key))
        emit replaySettingsChanged();
}

void AppController::resetConfigGroup(const QString& prefix)
{
    if (prefix == QStringLiteral("startup") || prefix.isEmpty())
        m_startup->setEnabled(false);
    if (prefix == QStringLiteral("storage") || prefix.isEmpty())
        m_locations->preserveCurrentRoots();
    if (!m_config->resetGroup(prefix))
        return;
    m_config->save();
    if (prefix == QStringLiteral("replay") || prefix == QStringLiteral("audio")
        || prefix.isEmpty())
        emit replaySettingsChanged();
    if (prefix == QStringLiteral("storage") || prefix.isEmpty())
        rescan();
}

void AppController::resetAllConfig()
{
    m_startup->setEnabled(false);
    m_locations->preserveCurrentRoots();
    if (!m_config->resetAll())
        return;
    m_config->save();
    emit replaySettingsChanged();
    rescan();
}

void AppController::resetCategory(const QString& category)
{
    for (const QString& prefix : SettingsCategories::groups().value(category))
        resetConfigGroup(prefix);
    for (const QString& key : SettingsCategories::keys().value(category))
        resetConfig(key);
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

int AppController::overlayGameId() const
{
    return m_currentGame->currentGameId();
}

QVariantList AppController::games() const
{
    QVariantList out;
    const auto entries = m_db->listGames();
    for (const GameEntry& g : entries) {
        QVariantMap item;
        item.insert(QStringLiteral("id"), g.id);
        item.insert(QStringLiteral("name"), g.name);
        item.insert(QStringLiteral("iconPath"), g.iconPath);
        out.append(item);
    }
    return out;
}

void AppController::setCategory(const QString& category)
{
    m_category = category;
    m_gameId = -1;
    m_gallery->setFilter(m_category, m_gameId);
    emit filterChanged();
}

void AppController::setGame(int gameId)
{
    m_category = QStringLiteral("all");
    m_gameId = gameId;
    m_gallery->setFilter(m_category, m_gameId);
    emit filterChanged();
}

void AppController::setGameCategory(const QString& category, int gameId)
{
    m_category = category;
    m_gameId = gameId;
    m_gallery->setFilter(m_category, m_gameId);
    emit filterChanged();
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
        const int gameId = m_currentGame->currentGameAvailable() ? m_currentGame->currentGameId() : -1;
        m_overlayGallery->setFilter(QStringLiteral("all"), gameId);
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

void AppController::updateForegroundGame(const QString& gameName, const QString& executablePath)
{
    const bool stateChanged = m_currentGame->update(gameName, executablePath);
    if (stateChanged) {
        emit currentGameChanged();
        emit overlayGameChanged();
    }

    if (m_currentGame->lastUpdateChangedGameMetadata())
        emit gamesChanged();
}

void AppController::updateReplayBufferState(bool active, const QString& gameName)
{
    if (m_replayBufferActive == active && m_replayBufferGame == gameName)
        return;
    m_replayBufferActive = active;
    m_replayBufferGame = gameName;
    emit replayBufferStateChanged();
}

void AppController::rescan()
{
    m_lastScanAdded = m_scanner->scanAll();
    m_gallery->refresh();
    emit gamesChanged();
    emit lastScanChanged();
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
    if (!m_captureLibrary->deleteCaptures(m_gallery, rows))
        return;
    emit gamesChanged();
}

void AppController::deleteCaptureFrom(GalleryModel* model, int row)
{
    if (m_captureLibrary->deleteCapture(model, row))
        emit gamesChanged();
}

void AppController::openCaptureFrom(GalleryModel* model, int row)
{
    m_captureLibrary->openCapture(model, row);
}

void AppController::showInFolderFrom(GalleryModel* model, int row)
{
    m_captureLibrary->showInFolder(model, row);
}

void AppController::commitCapture(const QString& filePath, const QString& type,
                                  const QString& gameName, const QString& executablePath)
{
    m_captureLibrary->commitCapture(filePath, type, gameName, executablePath);
    const bool stateChanged = m_currentGame->update(gameName, executablePath);
    if (stateChanged) {
        emit currentGameChanged();
        emit overlayGameChanged();
    }
    emit gamesChanged();
}

void AppController::commitClip(const QString& filePath, const QString& gameName,
                              const QString& thumbnailPath, const QString& executablePath)
{
    m_captureLibrary->commitClip(filePath, gameName, thumbnailPath, executablePath);
    const bool stateChanged = m_currentGame->update(gameName, executablePath);
    if (stateChanged) {
        emit currentGameChanged();
        emit overlayGameChanged();
    }
    emit gamesChanged();
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
