#include "app/App.h"
#include "app/StartupManager.h"
#include "config/ConfigKeys.h"
#include "capture/FramePumpService.h"
#include "capture/ScreenshotService.h"
#include "config/CaptureLocations.h"
#include "config/Paths.h"
#include "config/ConfigManager.h"
#include "diagnostics/Logger.h"
#include "input/HotkeyManager.h"
#include "input/ActionCatalog.h"
#include "input/InputDiagnostics.h"
#include "input/InputEngine.h"
#include "integration/IntegrationService.h"
#include "localization/LanguageManager.h"
#include "localization/LanguagePreference.h"
#include "localization/LocaleRegistry.h"
#include "localization/NativeText.h"
#include "games/GameDetector.h"
#include "core/UpdateMaintenance.h"
#include "notify/NotificationCenter.h"
#include "overlay/ForegroundAcquirer.h"
#include "overlay/ForegroundApi.h"
#include "overlay/OverlayManager.h"
#include "sound/SoundEngine.h"
#include "storage/CaptureDatabase.h"
#include "storage/CaptureScanner.h"
#include "tray/TrayIcon.h"
#include "ui/AppController.h"
#include "ui/GalleryModel.h"
#include "updates/UpdateSchedule.h"
#include "updates/UpdateService.h"
#include "updates/UpdateInstaller.h"
#include "Brand.h"

#include <QDateTime>
#include <QFile>
#include <QGuiApplication>
#include <QQmlContext>
#include <QQuickWindow>
#include <QTimer>

App::App(QObject* parent)
    : QObject(parent)
{
}

App::~App()
{
    GameDetector::setExternalContext(nullptr);
    // Orderly teardown reached: the next launch should not report a crash.
    QFile::remove(Paths::dataDir() + QStringLiteral("/session.marker"));
}

void App::setPostUpdateValidation(bool enabled)
{
    m_postUpdateValidation = enabled;
}

void App::recordPostUpdateSuccess(const QString& version)
{
    if (!m_postUpdateValidation || !m_controller)
        return;
    m_controller->setConfig(QString(ConfigKeys::InternalUpdatesPendingPostUpdateVersion), version);
}

bool App::init()
{
    const Paths::DirectoryStatus directories = Paths::ensureDirectories();
    Logger::install(Paths::logsDir());
    if (!directories.essentialReady) {
        // Continuing would mean running with no place to keep the database,
        // settings or recordings, and reporting success anyway.
        qCritical() << "Storage: GameHQ cannot create its data locations:"
                    << directories.failedEssential.join(QStringLiteral(", "));
        return false;
    }
    if (!directories.failedOptional.isEmpty()) {
        qWarning() << "Storage: continuing without some cache or log locations:"
                   << directories.failedOptional.join(QStringLiteral(", "));
    }

    // Crash triage without telemetry: a marker that only an orderly shutdown
    // removes. Finding it at startup means the previous session died, which
    // the input diagnostics export reports alongside the backend history.
    {
        const QString marker = Paths::dataDir() + QStringLiteral("/session.marker");
        const bool crashed = QFile::exists(marker);
        if (crashed)
            qWarning() << "App: previous session ended unexpectedly (marker found)";
        InputDiagnostics::instance().setPreviousSessionCrashed(crashed);
        QFile file(marker);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            file.write("running");
    }

    m_integration = std::make_unique<IntegrationService>(QStringLiteral(GAMEHQ_VERSION));
    connect(m_integration.get(), &IntegrationService::activateRequested,
            this, &App::showWindow);
    connect(m_integration.get(), &IntegrationService::openGalleryRequested,
            this, &App::openGallery);
    QString integrationError;
    if (!m_integration->start(integrationError))
        qWarning() << "Integration server unavailable:" << integrationError;
    GameDetector::setExternalContext(m_integration->externalContext());

    qInfo() << Brand::Name << GAMEHQ_VERSION
            << "starting, portable:" << Paths::isPortable()
            << "data:" << Paths::dataDir();

    m_config = std::make_unique<ConfigManager>(Paths::dataDir() + QStringLiteral("/config.json"));
    // An unreadable config used to be ignored, and the first save() then
    // destroyed the only copy of the user's settings. Preserve it first, and
    // refuse to start if even that fails.
    switch (m_config->loadOrQuarantine()) {
    case ConfigManager::LoadResult::Loaded:
    case ConfigManager::LoadResult::Missing:
        break;
    case ConfigManager::LoadResult::Quarantined:
        m_configQuarantinedPath = m_config->quarantinedPath();
        break;
    case ConfigManager::LoadResult::Unrecoverable:
        qCritical() << "Config: refusing to start on defaults because the existing settings file "
                       "could not be read or preserved";
        return false;
    }

    // Localization must be stable before any user-visible model, tray action,
    // notification, or QML component is constructed. LanguageManager is the
    // only owner of installed translators for the process lifetime.
    m_localeRegistry = std::make_unique<LocaleRegistry>();
    QString localeError;
    if (!m_localeRegistry->load(QStringLiteral(":/i18n/locales.json"), &localeError)) {
        qCritical() << "Localization manifest is invalid:" << localeError;
        return false;
    }
    m_languageManager = std::make_unique<LanguageManager>(m_localeRegistry.get());
    m_languagePreference = std::make_unique<LanguagePreference>(m_config.get(),
                                                                m_localeRegistry.get());
    RegistryLanguageBootstrapStore bootstrapStore;
    QString bootstrapWarning;
    const QString requestedLanguage = m_languagePreference->initialLanguage(
        bootstrapStore, Paths::isPortable(), &bootstrapWarning);
    if (!bootstrapWarning.isEmpty())
        qWarning() << "Localization bootstrap:" << bootstrapWarning;
    if (!m_languageManager->initialize(requestedLanguage, {}, &localeError)) {
        qCritical() << "Localization could not initialize:" << localeError;
        return false;
    }
    QGuiApplication::setLayoutDirection(m_languageManager->layoutDirection());
    connect(m_languageManager.get(), &LanguageManager::languageChanged, this, [this] {
        QGuiApplication::setLayoutDirection(m_languageManager->layoutDirection());
    });
    m_languagePreference->bind(m_languageManager.get());
    ActionCatalog::retranslate();
    connect(m_languageManager.get(), &LanguageManager::retranslationRequested,
            this, [] { ActionCatalog::retranslate(); });

    m_locations = std::make_unique<CaptureLocations>(m_config.get());
    m_startup = std::make_unique<StartupManager>();
    const bool startupEnabled = m_config->value(ConfigKeys::StartupEnabled, false).toBool();
    if (!m_startup->setEnabled(startupEnabled) && startupEnabled) {
        m_config->setValue(ConfigKeys::StartupEnabled, false);
        m_config->save();
    }

    m_db = std::make_unique<CaptureDatabase>(Paths::databasePath());
    if (!m_db->open()) {
        qCritical() << "Failed to open database — aborting startup";
        return false;
    }

    m_scanner = std::make_unique<CaptureScanner>(m_db.get(), m_locations.get(),
                                                 Paths::thumbnailsDir());
    m_gallery = std::make_unique<GalleryModel>(m_db.get());
    m_overlayGallery = std::make_unique<GalleryModel>(m_db.get());
    m_controller = std::make_unique<AppController>(m_db.get(), m_scanner.get(),
                                                   m_gallery.get(), m_overlayGallery.get(),
                                                   m_config.get(), m_locations.get(), m_startup.get(),
                                                   m_languageManager.get());
    connect(m_languageManager.get(), &LanguageManager::retranslationRequested,
            m_controller.get(), &AppController::retranslate);
    connect(m_languageManager.get(), &LanguageManager::retranslationRequested,
            m_gallery.get(), &GalleryModel::retranslate);
    connect(m_languageManager.get(), &LanguageManager::retranslationRequested,
            m_overlayGallery.get(), &GalleryModel::retranslate);

    // Ordering contract for everything below: the service lambdas capture `this`
    // and dereference members that are constructed further down (m_sounds at the
    // SoundEngine block, m_notify at the NotificationCenter block). That is safe
    // only because nothing here emits before init() returns — the first capture
    // needs the pad/hotkeys, which m_input->start() arms last. Keep it that way:
    // if a service ever fires a signal from its own constructor, construct
    // m_sounds/m_notify before the connect that uses them.

    // Screenshot capture (0.4): GDI grab of the foreground game → DB/gallery.
    m_screenshots = std::make_unique<ScreenshotService>(m_config.get(),
                                                        m_locations.get());
    // Let the QML bridge save clip frames (Share on a focused clip) through the
    // same screenshot pipeline — same folder, format, sound and toast.
    m_controller->setScreenshotService(m_screenshots.get());
    // Shutter plays the instant the pixels are grabbed (before the async encode),
    // so the feedback is immediate even though the PNG lands a moment later.
    connect(m_screenshots.get(), &ScreenshotService::grabbed, this,
            [this] {
                if (m_config->value(ConfigKeys::CaptureScreenshotSound, true).toBool())
                    m_sounds->play(QStringLiteral("screenshot"));
            });
    connect(m_screenshots.get(), &ScreenshotService::captured, this,
            [this](const QString& path, const QString& game, const QString& exePath) {
                m_controller->commitCapture(path, QStringLiteral("screenshot"), game, exePath);
                if (m_config->value(ConfigKeys::NotificationsEnabled, true).toBool()
                    && m_config->value(ConfigKeys::CaptureScreenshotNotify, true).toBool()) {
                    m_notify->post(
                        NativeText::get(
                            //: Title of the notification shown after a screenshot is saved.
                            //% "Screenshot saved"
                            QT_TRID_NOOP("gamehq.notification.screenshot_saved.title"),
                            "Screenshot saved"),
                        game, path,
                        QStringLiteral("success"), QDateTime::currentDateTime(), false);
                }
            });
    connect(m_screenshots.get(), &ScreenshotService::skipped, this,
            [this](const QString& why) {
                qInfo() << "Screenshot: skipped —" << why;
                m_sounds->play(QStringLiteral("error"));
            });
    connect(m_screenshots.get(), &ScreenshotService::failed, this,
            [this](const QString& why) {
                qWarning() << "Screenshot: failed —" << why;
                m_sounds->play(QStringLiteral("error"));
            });

    // WGC replay buffer (0.5): records continuously while a game is foreground
    // (replay.auto, always-on; Settings → Replay is the master switch). Share-hold
    // (or Ctrl+Shift+E) saves the last N seconds as one clip (below).
    m_framePump = std::make_unique<FramePumpService>(m_config.get(), m_locations.get());
    connect(m_screenshots.get(), &ScreenshotService::hdrCaptureRequested,
            m_framePump.get(), &FramePumpService::captureHdrScreenshot);
    connect(m_framePump.get(), &FramePumpService::hdrScreenshotReady,
            m_screenshots.get(), &ScreenshotService::saveImage);
    connect(m_framePump.get(), &FramePumpService::hdrScreenshotFailed,
            m_screenshots.get(), &ScreenshotService::reportHdrCaptureFailure);
    connect(m_framePump.get(), &FramePumpService::failed, this,
            [this](const QString& why) { qWarning() << "FramePump: failed —" << why; });
    // The ring freezes immediately, but do not say "saved" until the final MP4
    // exists and the gallery commit path runs. Otherwise a remux failure sounds
    // successful while no thumbnail appears.
    connect(m_framePump.get(), &FramePumpService::clipSaving, this,
            [](const QString&, const QString&, const QString&) {
            });
    // Remux finished: file + thumbnail ready → add it to the gallery and notify.
    connect(m_framePump.get(), &FramePumpService::clipSaved, this,
            [this](const QString& path, const QString& game, const QString& thumb,
                   const QString& exePath) {
                m_controller->commitClip(path, game, thumb, exePath);
                if (m_config->value(ConfigKeys::ReplayClipSound, true).toBool())
                    m_sounds->play(QStringLiteral("replay_saved"));
                if (m_config->value(ConfigKeys::NotificationsEnabled, true).toBool()
                    && m_config->value(ConfigKeys::ReplayClipNotify, true).toBool()) {
                    m_notify->post(
                        NativeText::get(
                            //: Title of the notification shown after a replay clip is saved.
                            //% "Replay saved"
                            QT_TRID_NOOP("gamehq.notification.replay_saved.title"),
                            "Replay saved"),
                        game, thumb, QStringLiteral("success"), QDateTime::currentDateTime(), true);
                }
            });
    connect(m_framePump.get(), &FramePumpService::clipFailed, this,
            [this](const QString& game, const QString& reason) {
                m_sounds->play(QStringLiteral("error"));
                if (m_config->value(ConfigKeys::NotificationsEnabled, true).toBool()) {
                    const QString body = reason.isEmpty()
                        ? game
                        : NativeText::get(
                              //: Replay failure detail; %1 is the unchanged technical reason.
                              //% "Reason: %1"
                              QT_TRID_NOOP("gamehq.notification.replay_failed.reason"),
                              "Reason: %1").arg(reason);
                    m_notify->post(
                        NativeText::get(
                            //: Title of the notification shown when saving a replay clip fails.
                            //% "Replay failed"
                            QT_TRID_NOOP("gamehq.notification.replay_failed.title"),
                            "Replay failed"),
                        body, QString(), QStringLiteral("error"), QDateTime::currentDateTime(), false);
                }
            });
    connect(m_framePump.get(), &FramePumpService::foregroundGameDetected,
            m_controller.get(), &AppController::updateForegroundGame);
    connect(m_framePump.get(), &FramePumpService::recordingStateChanged,
            m_controller.get(), &AppController::updateReplayBufferState);
    // Settings changed a replay.* key (fps/resolution/length): re-arm a
    // running buffer so it records with the new parameters immediately.
    connect(m_controller.get(), &AppController::replaySettingsChanged,
            m_framePump.get(), &FramePumpService::restartBuffer);
    // HDR can be toggled while a game is running. Re-arm immediately so the
    // rolling buffer never combines SDR and HDR capture formats.
    connect(m_controller.get(), &AppController::hdrDisplayConfigurationChanged,
            m_framePump.get(), &FramePumpService::restartBuffer);

    m_tray = std::make_unique<TrayIcon>();
    connect(m_languageManager.get(), &LanguageManager::retranslationRequested,
            m_tray.get(), &TrayIcon::retranslate);
    connect(m_tray.get(), &TrayIcon::openGalleryRequested, this, &App::showWindow);
    connect(m_tray.get(), &TrayIcon::rescanRequested,
            m_controller.get(), &AppController::rescan);
    connect(m_tray.get(), &TrayIcon::screenshotRequested,
            m_screenshots.get(), &ScreenshotService::capture);
    connect(m_tray.get(), &TrayIcon::quitRequested,
            qApp, &QCoreApplication::quit, Qt::QueuedConnection);

    m_overlay = std::make_unique<OverlayManager>(&m_engine);
    m_notify = std::make_unique<NotificationCenter>(&m_engine);
    if (!m_configQuarantinedPath.isEmpty()) {
        // One non-blocking notice, not a startup loop: the settings are already
        // back at their defaults and the old file is still on disk.
        m_notify->post(
                       NativeText::get(
                           //: Title of the warning shown after an unreadable settings file is quarantined.
                           //% "Settings could not be read"
                           QT_TRID_NOOP("gamehq.notification.settings_quarantined.title"),
                           "Settings could not be read"),
                       NativeText::get(
                           //: Body of the warning shown after an unreadable settings file is quarantined.
                           //% "GameHQ started with default settings. Your previous settings file was kept so nothing was lost."
                           QT_TRID_NOOP("gamehq.notification.settings_quarantined.body"),
                           "GameHQ started with default settings. Your previous settings file was kept so nothing was lost."),
                       m_configQuarantinedPath, QStringLiteral("warning"),
                       QDateTime::currentDateTime(),
                       false);
    }
    m_updates = std::make_unique<UpdateService>(QStringLiteral("underfusion"), QStringLiteral("GameHQ"),
                                                 QStringLiteral(GAMEHQ_VERSION),
                                                 Paths::packageRoot() + QStringLiteral("/.update/downloads"),
                                                 Paths::dataDir() + QStringLiteral("/release-trust-state.json"),
                                                 nullptr);
    // Update-check policy (docs/updater.md "Discovery"): prime the cached ETag
    // and skip flag from config, persist whatever the service later reports,
    // and gate automatic checks to at most once every 24h. Manual checkNow()
    // from QML always bypasses this cache.
    m_updates->primeCachedEtag(m_config->value(ConfigKeys::InternalUpdatesEtag, QString()).toString());
    m_updates->primeSkippedVersion(m_config->value(ConfigKeys::UpdatesSkippedVersion, QString()).toString());
    m_updates->primeLastChecked(QDateTime::fromString(
        m_config->value(ConfigKeys::InternalUpdatesLastCheckUtc, QString()).toString(),
        Qt::ISODate).toUTC());
    m_updates->primeNextAllowedCheck(QDateTime::fromString(
        m_config->value(ConfigKeys::InternalUpdatesNextAllowedCheckUtc, QString()).toString(),
        Qt::ISODate).toUTC());
    connect(m_updates.get(), &UpdateService::nextAllowedCheckChanged, this,
            [this](const QDateTime& when) {
        m_config->setValue(ConfigKeys::InternalUpdatesNextAllowedCheckUtc,
                            when.isValid() ? when.toUTC().toString(Qt::ISODate) : QString());
        m_config->save();
    });
    connect(m_updates.get(), &UpdateService::etagUpdated, this, [this](const QString& etag) {
        m_config->setValue(ConfigKeys::InternalUpdatesEtag, etag);
        m_config->save();
    });
    connect(m_updates.get(), &UpdateService::versionSkipped, this, [this](const QString& version) {
        m_config->setValue(ConfigKeys::UpdatesSkippedVersion, version);
        m_config->save();
    });
    connect(m_updates.get(), &UpdateService::lastCheckedChanged, this, [this] {
        m_config->setValue(ConfigKeys::InternalUpdatesLastCheckUtc,
                            m_updates->lastChecked().toUTC().toString(Qt::ISODate));
        m_config->save();
    });
    m_updatePreparationTimer = new QTimer(this);
    m_updatePreparationTimer->setSingleShot(true);
    m_updatePreparationTimer->setInterval(30000);
    auto finishUpdatePreparation = [this] {
        if (!m_updateScreenshotsReady || !m_updateReplayReady)
            return;
        m_updatePreparationTimer->stop();
        m_config->save();
        qInfo() << "Update preparation: capture services are quiescent and config is flushed";
        m_updates->markQuiescent();
    };
    connect(m_screenshots.get(), &ScreenshotService::updateReady, this, [this, finishUpdatePreparation] {
        m_updateScreenshotsReady = true;
        finishUpdatePreparation();
    });
    connect(m_framePump.get(), &FramePumpService::updateWaitingForExport, this, [] {
        qInfo() << "Update preparation: waiting for the current replay export to finish";
    });
    connect(m_framePump.get(), &FramePumpService::updateReady, this, [this, finishUpdatePreparation] {
        m_updateReplayReady = true;
        finishUpdatePreparation();
    });
    connect(m_updates.get(), &UpdateService::prepareForUpdateRequested, this, [this] {
        m_updateScreenshotsReady = false;
        m_updateReplayReady = false;
        m_updatePreparationTimer->start();
        m_screenshots->prepareForUpdate();
        m_framePump->prepareForUpdate();
    });
    connect(m_updates.get(), &UpdateService::installApproved, this,
            [this](const VerifiedUpdate &verified) {
        QString transactionPath;
        QString error;
        if (!UpdateInstaller::prepareTransaction(Paths::packageRoot(), Paths::dataDir(),
                                                  verified, transactionPath, error)) {
            m_screenshots->cancelUpdatePreparation();
            m_framePump->cancelUpdatePreparation();
            m_updates->cancelPreparation(error);
            qWarning() << "Update installer handoff failed:" << error;
            return;
        }
        std::string maintenanceError;
        if (!maintenance::begin(Paths::packageRoot().toStdWString(), maintenanceError)) {
            error = QString::fromStdString(maintenanceError);
            m_screenshots->cancelUpdatePreparation();
            m_framePump->cancelUpdatePreparation();
            m_updates->cancelPreparation(
                NativeText::get(
                    //: Update failure detail; %1 is the unchanged maintenance subsystem reason.
                    //% "GameHQ could not begin update maintenance: %1"
                    QT_TRID_NOOP("gamehq.error.update.maintenance_handoff_failed"),
                    "GameHQ could not begin update maintenance: %1").arg(error));
            qWarning() << "Update maintenance handoff failed:" << error;
            return;
        }
        m_integration->broadcastMaintenance(300);
        if (!UpdateInstaller::launchPrepared(Paths::packageRoot(), transactionPath, error)) {
            maintenance::finish(Paths::packageRoot().toStdWString());
            m_integration->cancelMaintenance();
            m_screenshots->cancelUpdatePreparation();
            m_framePump->cancelUpdatePreparation();
            m_updates->cancelPreparation(error);
            qWarning() << "Update installer handoff failed:" << error;
            return;
        }
        m_updates->markInstalling();
        qInfo() << "Updater helper launched for" << verified.version << "; exiting application";
        QTimer::singleShot(0, qApp, &QCoreApplication::quit);
    });
    connect(m_updatePreparationTimer, &QTimer::timeout, this, [this] {
        m_screenshots->cancelUpdatePreparation();
        m_framePump->cancelUpdatePreparation();
        m_updates->cancelPreparation(NativeText::get(
            //: Update installation error shown when active capture work cannot stop safely.
            //% "The update was canceled because capture work did not finish safely in time."
            QT_TRID_NOOP("gamehq.error.update.capture_quiescence_timeout"),
            "The update was canceled because capture work did not finish safely in time."));
        qWarning() << "Update preparation timed out; update cancelled without stopping capture work";
    });
    auto maybeAutoCheckForUpdate = [this] {
        if (!m_config->value(ConfigKeys::UpdatesCheckAutomatically, true).toBool())
            return;
        const QDateTime last = QDateTime::fromString(
            m_config->value(ConfigKeys::InternalUpdatesLastCheckUtc, QString()).toString(), Qt::ISODate);
        // Two limits: GameHQ's own daily interval, and any cooldown GitHub
        // imposed. Automatic checks stay silent during a cooldown - the user
        // only sees a retry time if they ask for a check themselves.
        if (!UpdateSchedule::automaticCheckAllowed(last, m_updates->nextAllowedCheck(),
                                                   QDateTime::currentDateTimeUtc()))
            return;
        m_updates->checkNow();
    };
    QTimer::singleShot(20000, m_updates.get(), maybeAutoCheckForUpdate); // 15-30s after startup
    m_updateCheckTimer = new QTimer(this);
    m_updateCheckTimer->setInterval(60 * 60 * 1000); // hourly wake; the 24h gate above does the real throttling
    connect(m_updateCheckTimer, &QTimer::timeout, this, maybeAutoCheckForUpdate);
    m_updateCheckTimer->start();

    m_hotkeys = std::make_unique<HotkeyManager>();
    connect(m_overlay.get(), &OverlayManager::aboutToShow,
            m_controller.get(), &AppController::syncOverlayToForegroundGame);

    m_sounds = std::make_unique<SoundEngine>(m_config.get());
    connect(m_overlay.get(), &OverlayManager::visibleChanged, this, [this] {
        m_sounds->play(m_overlay->isVisible() ? QStringLiteral("overlay_open")
                                              : QStringLiteral("overlay_close"));
    });

    // Controller input (0.3): DualSense Share tap/hold + PS, keyboard hotkey stays.
    m_input = std::make_unique<InputEngine>(m_config.get(), m_db.get(), m_hotkeys.get());
    connect(m_languageManager.get(), &LanguageManager::retranslationRequested,
            m_input.get(), &InputEngine::retranslate);
    connect(m_input.get(), &InputEngine::overlayToggleRequested,
            m_overlay.get(), &OverlayManager::toggle);
    // Hold PS (2 s): summon/dismiss the desktop window with real OS focus.
    m_foregroundApi.reset(ForegroundApi::createSystem());
    m_desktopFocus = new ForegroundAcquirer(this);
    connect(m_controller.get(), &AppController::desktopFocusRequested,
            this, &App::focusDesktopWindow);
    connect(m_input.get(), &InputEngine::desktopWindowToggleRequested,
            this, &App::toggleDesktopWindow);
    // overlayHideRequested (Circle) is now consumed in OverlayWindow.qml: it
    // pops the action menu / sidebar focus first and only closes the overlay
    // at the root level, calling `overlay.hide()` itself in that case.
    connect(m_input.get(), &InputEngine::screenshotRequested,
            m_screenshots.get(), &ScreenshotService::capture);
    connect(m_input.get(), &InputEngine::replayRequested,
            m_framePump.get(), &FramePumpService::saveReplay);
    connect(m_overlay.get(), &OverlayManager::visibleChanged, this, [this] {
        m_input->setOverlayVisible(m_overlay->isVisible());
    });

    m_languageManager->setQmlRetranslateCallback([this] { m_engine.retranslate(); });
    m_engine.rootContext()->setContextProperty(QStringLiteral("app"), m_controller.get());
    m_engine.rootContext()->setContextProperty(QStringLiteral("overlayGallery"), m_overlayGallery.get());
    m_engine.rootContext()->setContextProperty(QStringLiteral("overlay"), m_overlay.get());
    m_engine.rootContext()->setContextProperty(QStringLiteral("sounds"), m_sounds.get());
    m_engine.rootContext()->setContextProperty(QStringLiteral("input"), m_input.get());
    m_engine.rootContext()->setContextProperty(QStringLiteral("updates"), m_updates.get());
    m_engine.rootContext()->setContextProperty(QStringLiteral("notifications"), m_notify.get());
    m_engine.rootContext()->setContextProperty(QStringLiteral("languageManager"),
                                               m_languageManager.get());
    m_engine.loadFromModule("GameHQ", "Main");
    if (m_engine.rootObjects().isEmpty()) {
        qCritical() << "Failed to load QML root — aborting startup";
        return false;
    }
    if (m_pendingOpenGallery)
        openGallery();
    else if (m_pendingActivation)
        showWindow();

    if (m_postUpdateValidation) {
        // A newly installed build must prove that its database, QML and services
        // stay alive before capture hooks are armed. The helper watches for the
        // token written by main.cpp after the event-loop health window.
        m_screenshots->prepareForUpdate();
        m_framePump->prepareForUpdate();
    } else {
        m_input->start();   // seed default bindings + begin listening for the pad
        m_inputStarted = true;
    }

    // First scan after the window is up so startup feels instant. The HDR probe
    // rides along for the same reason — it activates an encoder MFT, which is
    // far too slow to run before the first frame.
    QTimer::singleShot(0, m_controller.get(), &AppController::rescan);
    QTimer::singleShot(0, m_controller.get(), &AppController::refreshHdrStatus);

    // Topology changes get a full refresh; AppController also performs a
    // lightweight one-second check for Windows' HDR toggle itself.
    connect(qApp, &QGuiApplication::screenAdded, m_controller.get(), &AppController::refreshHdrStatus);
    connect(qApp, &QGuiApplication::screenRemoved, m_controller.get(), &AppController::refreshHdrStatus);
    connect(qApp, &QGuiApplication::primaryScreenChanged, m_controller.get(), &AppController::refreshHdrStatus);
    return true;
}

void App::completePostUpdateValidation()
{
    if (!m_postUpdateValidation)
        return;
    m_postUpdateValidation = false;
    m_screenshots->cancelUpdatePreparation();
    m_framePump->cancelUpdatePreparation();
    if (!m_inputStarted) {
        m_input->start();
        m_inputStarted = true;
    }
}

void App::showWindow()
{
    if (m_engine.rootObjects().isEmpty()) {
        m_pendingActivation = true;
        return;
    }
    m_pendingActivation = false;
    if (auto* window = qobject_cast<QQuickWindow*>(m_engine.rootObjects().first())) {
        window->show();
        window->raise();
        window->requestActivate();
    }
}

void App::focusDesktopWindow()
{
    if (m_engine.rootObjects().isEmpty() || !m_desktopFocus)
        return;
    auto* window = qobject_cast<QQuickWindow*>(m_engine.rootObjects().first());
    if (!window)
        return;

    window->show();
    window->raise();
    window->requestActivate();
    m_desktopFocus->acquire(reinterpret_cast<void*>(window->winId()),
                            QStringLiteral("desktop lightbox close"));
}

// Hold PS (2 s): bring the desktop window over the game with real OS focus;
// hold again while it is focused to hide it and hand the foreground back to
// the window it was taken from. requestActivate() alone is routinely denied
// by the foreground lock while a game owns input, so the acquirer's
// AttachThreadInput bypass (the overlay's own mechanism) does the focus move.
void App::toggleDesktopWindow()
{
    if (m_engine.rootObjects().isEmpty()) {
        m_pendingActivation = true;
        return;
    }
    auto* window = qobject_cast<QQuickWindow*>(m_engine.rootObjects().first());
    if (!window)
        return;

    if (window->isVisible() && window->isActive()) {
        qInfo() << "Desktop summon: hiding the window, returning focus";
        window->hide();
        if (m_desktopReturnTarget) {
            m_desktopFocus->acquire(m_desktopReturnTarget, QStringLiteral("desktop hide"));
            m_desktopReturnTarget = nullptr;
        }
        return;
    }

    // The overlay gets out of the way without restoring focus to the game —
    // this window is about to take it. Its remembered foreground becomes the
    // return address; otherwise remember whatever is foreground right now,
    // as long as that isn't already us.
    void* origin = m_overlay && m_overlay->isVisible()
        ? m_overlay->hideForDesktopHandoff() : nullptr;
    if (!origin) {
        void* current = m_foregroundApi->foregroundWindow();
        if (current && current != reinterpret_cast<void*>(window->winId()))
            origin = current;
    }
    if (origin)
        m_desktopReturnTarget = origin;
    qInfo() << "Desktop summon: showing the window, focus taken from" << m_desktopReturnTarget;
    showWindow();
    m_desktopFocus->acquire(reinterpret_cast<void*>(window->winId()),
                            QStringLiteral("desktop show"));
}

void App::openGallery()
{
    if (m_engine.rootObjects().isEmpty() || !m_controller) {
        m_pendingOpenGallery = true;
        return;
    }
    m_pendingOpenGallery = false;
    m_controller->setCategory(QStringLiteral("all"));
    QObject *root = m_engine.rootObjects().first();
    root->setProperty("settingsOpen", false);
    root->setProperty("helpOpen", false);
    showWindow();
}
