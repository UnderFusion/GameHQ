#pragma once
#include <QObject>
#include <QQmlApplicationEngine>
#include <memory>

class ConfigManager;
class LocaleRegistry;
class LanguageManager;
class LanguagePreference;
class CaptureLocations;
class StartupManager;
class CaptureDatabase;
class CaptureScanner;
class GalleryModel;
class AppController;
class TrayIcon;
class HotkeyManager;
class OverlayManager;
class SoundEngine;
class InputEngine;
class ScreenshotService;
class FramePumpService;
class NotificationCenter;
class UpdateService;
class IntegrationService;
class ForegroundApi;
class ForegroundAcquirer;
class QTimer;

// Owns application lifecycle: paths → logging → config → database →
// scanner/model/controller → tray → QML UI.
class App : public QObject
{
    Q_OBJECT
public:
    explicit App(QObject* parent = nullptr);
    ~App() override;

    // Returns false on fatal init failure (already logged).
    bool init();
    void setPostUpdateValidation(bool enabled);
    void recordPostUpdateSuccess(const QString& version);
    void completePostUpdateValidation();

private:
    void showWindow();
    void focusDesktopWindow();
    void openGallery();
    void toggleDesktopWindow();

    std::unique_ptr<ConfigManager> m_config;
    std::unique_ptr<LocaleRegistry> m_localeRegistry;
    std::unique_ptr<LanguageManager> m_languageManager;
    std::unique_ptr<LanguagePreference> m_languagePreference;
    std::unique_ptr<CaptureLocations> m_locations;
    std::unique_ptr<StartupManager> m_startup;
    std::unique_ptr<CaptureDatabase> m_db;
    std::unique_ptr<CaptureScanner> m_scanner;
    std::unique_ptr<GalleryModel> m_gallery;
    // Separate instance for the overlay (milestone 0.6) so its own category/
    // game filter never clashes with the main window's.
    std::unique_ptr<GalleryModel> m_overlayGallery;
    std::unique_ptr<AppController> m_controller;
    std::unique_ptr<TrayIcon> m_tray;
    std::unique_ptr<HotkeyManager> m_hotkeys;
    std::unique_ptr<OverlayManager> m_overlay;
    std::unique_ptr<SoundEngine> m_sounds;
    std::unique_ptr<InputEngine> m_input;
    std::unique_ptr<ScreenshotService> m_screenshots;
    std::unique_ptr<FramePumpService> m_framePump;
    std::unique_ptr<NotificationCenter> m_notify;
    std::unique_ptr<UpdateService> m_updates;
    std::unique_ptr<IntegrationService> m_integration;
    // Wakes periodically to see if the 24h automatic-check window has passed
    // (see the update-check policy block in init()). Manual checkNow() from
    // QML bypasses this entirely.
    QTimer* m_updateCheckTimer = nullptr;
    QTimer* m_updatePreparationTimer = nullptr;
    bool m_updateScreenshotsReady = false;
    bool m_updateReplayReady = false;
    bool m_postUpdateValidation = false;
    // Set when startup had to preserve an unreadable settings file, so the user
    // can be told once where it went.
    QString m_configQuarantinedPath;
    bool m_inputStarted = false;
    bool m_pendingActivation = false;
    bool m_pendingOpenGallery = false;
    // Hold-PS desktop summon: the foreground reader, the retrying focus
    // acquirer, and the window the foreground was taken from ("return
    // address" for the second hold).
    std::unique_ptr<ForegroundApi> m_foregroundApi;
    ForegroundAcquirer* m_desktopFocus = nullptr;
    void* m_desktopReturnTarget = nullptr;
    QQmlApplicationEngine m_engine;
};
