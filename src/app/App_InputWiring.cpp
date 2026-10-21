#include "app/App.h"
#include "capture/FramePumpService.h"
#include "capture/ScreenshotService.h"
#include "config/ConfigKeys.h"
#include "config/ConfigManager.h"
#include "gameinput/GameInputFocusController.h"
#include "input/GameSessionPresetBinder.h"
#include "input/InputEngine.h"
#include "input/MappingPresetModel.h"
#include "localization/LanguageManager.h"
#include "notify/NotificationCenter.h"
#include "overlay/ForegroundAcquirer.h"
#include "overlay/ForegroundApi.h"
#include "overlay/OverlayManager.h"
#include "ui/AppController.h"
#include "ui/SteamInputHelper.h"

#include <QDesktopServices>
#include <QUrl>

// Part of App::init(): the controller input engine, the game session
// mapping context and Steam Input guidance.
void App::wireInput()
{
    // Controller input (0.3): DualSense Share tap/hold + PS, keyboard hotkey stays.
    m_input = std::make_unique<InputEngine>(m_config.get(), m_db.get(), m_hotkeys.get());
    // cpo-o06c: the input stack attaches the runtime to that same controller, so
    // there is exactly one place that changes GameInput's focus policy.
    m_input->setGameInputFocusController(m_gameInputFocus.get());
    // cpo-o06e: the input layer is the neutral-state source a close waits on
    // before it hands the controller back. The overlay only asks; the physical
    // state stays the input layer's to read.
    m_overlay->setNeutralHandoffSource(m_input.get());
    connect(m_languageManager.get(), &LanguageManager::retranslationRequested,
            m_input.get(), &InputEngine::retranslate);
    connect(m_input.get(), &InputEngine::overlayToggleRequested,
            m_overlay.get(), &OverlayManager::toggleFromInput);
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
    connect(m_input.get(), &InputEngine::screenshotRequested, m_screenshots.get(),
            qOverload<const CaptureRequest&>(&ScreenshotService::capture));
    connect(m_input.get(), &InputEngine::replayRequested, m_framePump.get(),
            qOverload<const CaptureRequest&>(&FramePumpService::saveReplay));
    connect(m_overlay.get(), &OverlayManager::visibleChanged, this, [this] {
        m_input->setOverlayVisible(m_overlay->isVisible());
    });

    // cpo-p07: the game session decides the mapping game context. One direction:
    // the session's canonical key (CurrentGameService, which already survives an
    // open overlay and a still-running game) is pushed into the engine, and the
    // engine re-resolves the winners at the cpo-p05 safe switch boundary. Opening
    // or browsing the overlay is not a game-session transition, so nothing here
    // can retarget the mappings while the user is in the overlay.
    m_gameSessionPresets = std::make_unique<GameSessionPresetBinder>(
        m_input.get(), [this] { return m_controller->currentGameExecutableKey(); });
    // Settings' game row reads the same session: no second source of truth.
    m_input->mappingPresetsModel()->setGameTargetProvider([this] {
        MappingPresetModel::GameTarget target;
        target.key = m_controller->currentGameExecutableKey();
        target.label = m_controller->currentGameName();
        target.rowId = m_controller->currentGameId();
        return target;
    });
    // One shared wiring, so the app and tst_gamesessionpresets drive the same
    // connections: a session transition AND a metadata-only change of the same
    // session game (its stored executable path moving under a stable game id)
    // both re-sync the engine and the Settings game row.
    GameSessionPresetBinder::wireGameSessionContext(m_controller.get(),
                                                    m_gameSessionPresets.get(),
                                                    m_input->mappingPresetsModel());
    m_input->mappingPresetsModel()->refreshGameTarget();
    m_gameSessionPresets->syncFromSession();

    // Steam Input conflict help: guidance only. It reads the session game from
    // the same CurrentGameService as the mapping context, opens Steam's own
    // screens only from an explicit click, and posts at most one passive notice
    // per game and run.
    SteamInputHelper::Providers steamProviders;
    steamProviders.gameExecutable = [this] { return m_controller->currentGameExecutableKey(); };
    steamProviders.gameName = [this] { return m_controller->currentGameName(); };
    steamProviders.boundControls = [this] { return m_input->steamAdvisedBoundControls(); };
    steamProviders.openUrl = [](const QString& url) {
        return QDesktopServices::openUrl(QUrl(url));
    };
    steamProviders.notify = [this](const QString& title, const QString& body) {
        if (m_config->value(ConfigKeys::NotificationsEnabled, true).toBool()
            && m_config->value(ConfigKeys::NotificationsSteamInput, true).toBool())
            m_notify->post(title, body, {}, QStringLiteral("warning"));
    };
    m_steamInput = std::make_unique<SteamInputHelper>(m_config.get(), std::move(steamProviders));
    connect(m_controller.get(), &AppController::currentGameChanged,
            m_steamInput.get(), &SteamInputHelper::refresh, Qt::QueuedConnection);
    connect(m_input.get(), &InputEngine::mappingTablesChanged,
            m_steamInput.get(), &SteamInputHelper::refresh, Qt::QueuedConnection);
}
