#include "app/App.h"
#include "app/StartupManager.h"
#include "config/ConfigKeys.h"
#include "capture/FramePumpService.h"
#include "capture/ReplayBufferState.h"
#include "capture/ScreenshotService.h"
#include "capture/CapturePublisher.h"
#include "config/CaptureLocations.h"
#include "config/Paths.h"
#include "config/ConfigManager.h"
#include "diagnostics/Logger.h"
#include "gameinput/GameInputFocusController.h"
#include "input/HotkeyManager.h"
#include "input/ActionCatalog.h"
#include "input/InputDiagnostics.h"
#include "input/InputEngine.h"
#include "input/GameSessionPresetBinder.h"
#include "input/MappingPresetModel.h"
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
#include "share/ShareService.h"
#include "share/TelegramWiring.h"
#include "share/providers/ClipboardShareProvider.h"
#include "share/external/ExternalProviderHost.h"
#include "share/providers/DiscordDesktopProvider.h"
#include "share/providers/DiscordWebhookProvider.h"
#include "share/providers/TelegramDesktopProvider.h"
#include "tray/TrayIcon.h"
#include "ui/AppController.h"
#include "ui/GalleryModel.h"
#include "ui/SteamInputHelper.h"
#include "updates/UpdateSchedule.h"
#include "updates/UpdateService.h"
#include "updates/UpdateInstaller.h"
#include "Brand.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QFile>
#include <QGuiApplication>
#include <QQmlContext>
#include <qqml.h>
#include <QQuickWindow>
#include <QTimer>
#include <QUrl>

App::App(QObject* parent)
    : QObject(parent)
{
}

App::~App()
{
    // Teardown runs in a fixed order. Members are destroyed in reverse
    // declaration order, so the input engine and capture go before the
    // overlay; anything that points across that boundary is detached here.
    // 1. No new events from the game detector's external context.
    GameDetector::setExternalContext(nullptr);
    // 2. Finish the overlay's close handoff while the input engine it
    //    quiesced is still alive, and detach it so ~OverlayManager cannot
    //    reach the destroyed engine.
    if (m_overlay)
        m_overlay->setNeutralHandoffSource(nullptr);
    // 3. Input and capture stop in their own destructors. The session marker
    //    is removed by main() only after all of this has finished.
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
    CapturePublisher::setOwnershipJournal(Paths::dataDir()
                                          + QStringLiteral("/pending-captures.journal"));
    m_startup = std::make_unique<StartupManager>();
    const bool startupEnabled = m_config->value(ConfigKeys::StartupEnabled, false).toBool();
    if (!m_startup->syncOnLaunch(startupEnabled) && startupEnabled) {
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

    wireCaptureFeedback();

    m_tray = std::make_unique<TrayIcon>();
    connect(m_languageManager.get(), &LanguageManager::retranslationRequested,
            m_tray.get(), &TrayIcon::retranslate);
    connect(m_tray.get(), &TrayIcon::openGalleryRequested, this, &App::showWindow);
    connect(m_tray.get(), &TrayIcon::rescanRequested,
            m_controller.get(), &AppController::rescan);
    connect(m_tray.get(), &TrayIcon::screenshotRequested, m_screenshots.get(), [this] {
        m_screenshots->capture(CaptureRequest::create(CaptureRequest::Source::Tray));
    });
    connect(m_tray.get(), &TrayIcon::quitRequested,
            qApp, &QCoreApplication::quit, Qt::QueuedConnection);

    // cpo-o06c: one owner of the process-wide GameInput focus policy, created
    // before anything that may use it (overlay, input stack) so it outlives both.
    m_gameInputFocus = std::make_unique<ModernInput::GameInputFocusController>();
    m_overlay = std::make_unique<OverlayManager>(&m_engine);
    // The overlay may only REQUEST a policy change; the controller owns it.
    m_overlay->setGameInputFocusRequestSink(m_gameInputFocus.get());
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
    wireUpdates();

    m_hotkeys = std::make_unique<HotkeyManager>();
    connect(m_overlay.get(), &OverlayManager::aboutToShow,
            m_controller.get(), &AppController::syncOverlayToForegroundGame);

    m_sounds = std::make_unique<SoundEngine>(m_config.get());
    // Settings previews the capture sound through the controller; the engine
    // only exists this late, so the controller is told about it here.
    m_controller->setSoundEngine(m_sounds.get());
    connect(m_sounds.get(), &SoundEngine::loadFailed, this,
            [this](const QString& failedEvents) {
                // One notice per run, deliberately not gated on
                // "notifications.enabled": the UI has just gone silent and the
                // log line naming the events is the only other evidence.
                qWarning() << "Sounds: unavailable —" << failedEvents;
                m_notify->post(
                    NativeText::get(
                        //: Title of the warning shown once when interface sound
                        //: effects could not be loaded at startup.
                        //% "Some interface sounds are unavailable"
                        QT_TRID_NOOP("gamehq.notification.sounds_unavailable.title"),
                        "Some interface sounds are unavailable"),
                    {}, {}, QStringLiteral("warning"), QDateTime::currentDateTime(), false);
            });
    connect(m_overlay.get(), &OverlayManager::visibleChanged, this, [this] {
        m_sounds->play(m_overlay->isVisible() ? QStringLiteral("overlay_open")
                                              : QStringLiteral("overlay_close"));
    });

    wireInput();

    m_languageManager->setQmlRetranslateCallback([this] { m_engine.retranslate(); });
    qmlRegisterUncreatableType<ReplayBufferState>("GameHQ", 1, 0, "ReplayBufferState",
                                                QStringLiteral("Replay state is owned by the capture service"));
    qmlRegisterUncreatableMetaObject(CaptureBorder::staticMetaObject, "GameHQ", 1, 0,
                                     "CaptureBorder", QStringLiteral("Capture border API states"));
    m_engine.rootContext()->setContextProperty(QStringLiteral("framePump"), m_framePump.get());
    m_engine.rootContext()->setContextProperty(QStringLiteral("app"), m_controller.get());
    wireShare();
    m_engine.rootContext()->setContextProperty(QStringLiteral("shareService"), m_share.get());
    m_engine.rootContext()->setContextProperty(QStringLiteral("overlayGallery"), m_overlayGallery.get());
    m_engine.rootContext()->setContextProperty(QStringLiteral("overlay"), m_overlay.get());
    m_engine.rootContext()->setContextProperty(QStringLiteral("sounds"), m_sounds.get());
    m_engine.rootContext()->setContextProperty(QStringLiteral("input"), m_input.get());
    m_engine.rootContext()->setContextProperty(QStringLiteral("steamInput"), m_steamInput.get());
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
            // The window may have closed (and its HWND been recycled) while
            // the desktop window was up; only hand focus to the same one.
            if (isSameWindow(*m_foregroundApi, m_desktopReturnTarget, m_desktopReturnPid))
                m_desktopFocus->acquire(m_desktopReturnTarget, QStringLiteral("desktop hide"));
            else
                qInfo() << "Desktop summon: return window" << m_desktopReturnTarget
                        << "is gone, leaving focus alone";
            m_desktopReturnTarget = nullptr;
            m_desktopReturnPid = 0;
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
    if (origin) {
        m_desktopReturnTarget = origin;
        m_desktopReturnPid = m_foregroundApi->windowProcessId(origin);
    }
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
    // Explicit "show me the gallery" intent changes the page only: the filter
    // the user last chose is theirs, not something an --open-gallery call gets
    // to reset (docs/product-spec.md "Navigation memory", rule R4).
    QObject *root = m_engine.rootObjects().first();
    root->setProperty("settingsOpen", false);
    root->setProperty("helpOpen", false);
    showWindow();
}
