qt_add_executable(GameHQ
    main.cpp
    app/App.cpp            app/App.h
    app/ReleaseNotes.cpp   app/ReleaseNotes.h
    app/StartupManager.cpp app/StartupManager.h
    capture/CapturePublisher.cpp capture/CapturePublisher.h
    capture/CaptureUtil.h
      capture/HdrCapabilities.cpp capture/HdrCapabilities.h capture/hdr/HdrStateResolver.h
    capture/ScreenshotService.cpp capture/ScreenshotService.h
    capture/FramePumpService.cpp capture/FramePumpService.h capture/wgc_shims.h
    capture/hdr/HdrToneMapMath.cpp capture/hdr/HdrToneMapMath.h
    capture/hdr/GpuToneMapper.cpp capture/hdr/GpuToneMapper.h
    capture/AudioCapture.cpp capture/AudioCapture.h capture/wasapi_shims.h
    capture/SegmentRecorder.cpp capture/SegmentRecorder.h
    capture/ReplayExporter.cpp capture/ReplayExporter.h
    config/Paths.cpp       config/Paths.h
    config/LegacyMigration.cpp config/LegacyMigration.h
    config/PortableProfileImporter.cpp config/PortableProfileImporter.h
    config/ConfigManager.cpp config/ConfigManager.h
    config/CaptureLocations.cpp config/CaptureLocations.h
    core/ApplicationMutex.cpp core/ApplicationMutex.h
    core/ProcessIdentity.cpp core/ProcessIdentity.h
    core/GameIdentity.cpp core/GameIdentity.h
    core/UpdateMaintenance.cpp core/UpdateMaintenance.h
    core/UpdaterHandshake.cpp core/UpdaterHandshake.h
    diagnostics/Logger.cpp diagnostics/Logger.h
    gameinput/GameInputEvent.h
    gameinput/IGameInputApi.h
    gameinput/GameInputEventQueue.cpp gameinput/GameInputEventQueue.h
    gameinput/GameInputQtDispatcher.cpp gameinput/GameInputQtDispatcher.h
    gameinput/GameInputWrapper.cpp gameinput/GameInputWrapper.h
    gameinput/GameInputRouter.cpp gameinput/GameInputRouter.h
    gameinput/ProductionGameInputApi.cpp gameinput/ProductionGameInputApi.h
    gameinput/GameInputLabelMap.h
    gameinput/StandardControlMap.h
    games/GameDetector.cpp games/GameDetector.h
    input/HotkeyManager.cpp input/HotkeyManager.h
    input/Gamepad.h
    input/StickNav.h
    input/SonyReportLayout.h
    input/ActionCatalog.cpp input/ActionCatalog.h
    input/BindingPattern.cpp input/BindingPattern.h
    input/BindingRelation.cpp input/BindingRelation.h
    input/BindingResolver.cpp input/BindingResolver.h
    input/ContextOverrideCatalog.cpp input/ContextOverrideCatalog.h
    input/BindingRuntime.cpp input/BindingRuntime.h
    input/InputPatternRecognizer.cpp input/InputPatternRecognizer.h
    input/GestureTiming.cpp input/GestureTiming.h
    input/BindingEditorModel.cpp input/BindingEditorModel.h
    input/ControlId.cpp input/ControlId.h
    input/ControllerIdentity.cpp input/ControllerIdentity.h
    input/PhysicalControllerRegistry.cpp input/PhysicalControllerRegistry.h
    input/ExtraButtonCatalog.cpp input/ExtraButtonCatalog.h
    input/CapabilityEventRouter.cpp input/CapabilityEventRouter.h
    input/ProviderIntegration.cpp input/ProviderIntegration.h
      input/SelectiveRawHidFallback.cpp input/SelectiveRawHidFallback.h
      input/RawHidBindingCatalog.cpp input/RawHidBindingCatalog.h
    input/InputDiagnostics.cpp input/InputDiagnostics.h
    input/InputRateMonitor.cpp input/InputRateMonitor.h
    input/RawInputApi.cpp input/RawInputApi.h
    input/DualSenseDevice.cpp input/DualSenseDevice.h
    input/HidCloakMonitor.cpp input/HidCloakMonitor.h
    input/XInputDevice.cpp input/XInputDevice.h
    input/WinMMDevice.cpp input/WinMMDevice.h
    integration/IntegrationMessage.h
    integration/IntegrationProtocol.cpp integration/IntegrationProtocol.h
    integration/LocalIntegrationServer.cpp integration/LocalIntegrationServer.h
    integration/ExternalGameContext.cpp integration/ExternalGameContext.h
    integration/IntegrationService.cpp integration/IntegrationService.h
    integration/IntegrationClient.cpp integration/IntegrationClient.h
    input/MouseHookDevice.cpp input/MouseHookDevice.h
    input/MouseMonitorPolicy.h
    input/PerfTrace.cpp input/PerfTrace.h
    input/EventLoopStallMonitor.cpp input/EventLoopStallMonitor.h
    input/InputEngine.cpp input/InputEngine.h
    notify/NotificationCenter.cpp notify/NotificationCenter.h
    overlay/ForegroundAcquirer.cpp overlay/ForegroundAcquirer.h
    overlay/ForegroundApi.cpp overlay/ForegroundApi.h
    overlay/OverlayManager.cpp overlay/OverlayManager.h
    sound/SoundEngine.cpp  sound/SoundEngine.h
    storage/CaptureDatabase.cpp storage/CaptureDatabase.h
    storage/CaptureQueries.cpp storage/CaptureQueries.h
    storage/CaptureScanner.cpp  storage/CaptureScanner.h
    storage/GameIconCache.cpp storage/GameIconCache.h
    storage/GameMetadataBackfill.cpp storage/GameMetadataBackfill.h
    storage/GameRowRepair.cpp storage/GameRowRepair.h
    storage/ThumbnailService.cpp storage/ThumbnailService.h
    tray/TrayIcon.cpp      tray/TrayIcon.h
    ui/AppController.cpp   ui/AppController.h
    ui/CaptureLibraryService.cpp ui/CaptureLibraryService.h
    ui/SettingsRouter.cpp ui/SettingsRouter.h
    ui/CurrentGameService.cpp ui/CurrentGameService.h
    ui/GalleryModel.cpp    ui/GalleryModel.h
    ui/ShellActions.cpp    ui/ShellActions.h
    updates/VersionNumber.cpp updates/VersionNumber.h
    updates/ReleaseInfo.h
    updates/UpdateDownloader.cpp updates/UpdateDownloader.h
    updates/UpdatePreflight.cpp updates/UpdatePreflight.h
    updates/UpdateInstaller.cpp updates/UpdateInstaller.h
    updates/GitHubReleaseSource.cpp updates/GitHubReleaseSource.h
    updates/ReleaseCatalog.cpp updates/ReleaseCatalog.h
    updates/UpdateSchedule.cpp updates/UpdateSchedule.h
    updates/UpdateService.cpp updates/UpdateService.h
    # Win32 resource: embeds the multi-size app icon into GameHQ.exe.
    ${CMAKE_SOURCE_DIR}/assets/icons/gamehq.rc
)

set_source_files_properties(ui/qml/Theme.qml ui/qml/Brand.qml PROPERTIES
    QT_QML_SINGLETON_TYPE TRUE
)

qt_add_qml_module(GameHQ
    URI GameHQ
    VERSION 1.0
    QML_FILES
        ui/qml/Main.qml
        ui/qml/OverlayWindow.qml
        ui/qml/ToastWindow.qml
        ui/qml/SettingsView.qml
        ui/qml/HelpView.qml
        ui/qml/Theme.qml
        ui/qml/Brand.qml
        ui/qml/helpers/SidebarCategories.js
        ui/qml/helpers/PadNav.js
        ui/qml/themes/Skin.qml
        ui/qml/themes/DarkSkin.qml
        ui/qml/themes/LightSkin.qml
        ui/qml/themes/HighContrastSkin.qml
        ui/qml/themes/MidnightSkin.qml
        ui/qml/themes/EmeraldSkin.qml
        ui/qml/themes/HarborSkin.qml
        ui/qml/themes/CarbonSkin.qml
        ui/qml/themes/CobaltSkin.qml
        ui/qml/themes/SynthwaveSkin.qml
        ui/qml/themes/NordSkin.qml
        ui/qml/themes/DraculaSkin.qml
        ui/qml/themes/GruvboxSkin.qml
        ui/qml/themes/ObsidianSkin.qml
        ui/qml/components/ThemeBackdrop.qml
        ui/qml/components/BulkSelection.qml
        ui/qml/components/SidebarItem.qml
        ui/qml/components/DesktopSidebar.qml
        ui/qml/components/DesktopGalleryHeader.qml
        ui/qml/components/DesktopGalleryFooter.qml
        ui/qml/components/DesktopGalleryGrid.qml
        ui/qml/components/DesktopEmptyState.qml
        ui/qml/components/OverlaySidebar.qml
        ui/qml/components/OverlayCaptureStrip.qml
        ui/qml/components/OverlayPreview.qml
        ui/qml/components/OverlayFooter.qml
        ui/qml/components/OverlayActionMenu.qml
        ui/qml/components/CaptureTile.qml
        ui/qml/components/AccentButton.qml
        ui/qml/components/AppScrollBar.qml
        ui/qml/components/TileIconButton.qml
        ui/qml/components/VideoBadge.qml
        ui/qml/components/SettingsPathField.qml
        ui/qml/components/BindingCard.qml
        ui/qml/components/BindingSegmentedControl.qml
        ui/qml/components/BindingAssignmentDialog.qml
        ui/qml/components/BindingConflictDialog.qml
        ui/qml/components/BindingCompatibilityDialog.qml
        ui/qml/components/ConfirmDialog.qml
        ui/qml/components/Toast.qml
        ui/qml/components/UpdateBanner.qml
        ui/qml/components/AboutWhatsNewDialog.qml
        ui/qml/components/HelpDialog.qml
        ui/qml/components/DialogCloseButton.qml
        ui/qml/components/TextLink.qml
        ui/qml/components/Lightbox.qml
        ui/qml/components/MediaStage.qml
        ui/qml/components/PlayerControls.qml
        ui/qml/components/NavRepeat.qml
        ui/qml/components/SettingsCombo.qml
        ui/qml/components/SettingsPage.qml
        ui/qml/components/SettingsPageHeader.qml
        ui/qml/components/SettingsSection.qml
        ui/qml/components/SettingsRow.qml
        ui/qml/components/SettingsToggle.qml
        ui/qml/components/SettingsSlider.qml
        ui/qml/components/SettingsCategoryButton.qml
        ui/qml/components/SettingsSegmentedControl.qml
        ui/qml/components/SettingsPathRow.qml
        ui/qml/components/SettingsLinkRow.qml
        ui/qml/components/SettingsStatusStrip.qml
        ui/qml/components/SettingsActionTile.qml
        ui/qml/components/SettingsEmptyState.qml
        ui/qml/components/SettingsDisclosure.qml
        ui/qml/settings/GeneralSettingsPage.qml
        ui/qml/settings/CaptureSettingsPage.qml
        ui/qml/settings/ReplaySettingsPage.qml
        ui/qml/settings/InputSettingsPage.qml
        ui/qml/settings/LibrarySettingsPage.qml
        ui/qml/settings/FeedbackSettingsPage.qml
        ui/qml/settings/AdvancedSettingsPage.qml
        ui/qml/settings/AboutSettingsPage.qml
    )

qt_add_resources(GameHQ "icons"
    PREFIX "/icons"
    BASE "${CMAKE_SOURCE_DIR}/assets/icons"
    FILES
        "${CMAKE_SOURCE_DIR}/assets/icons/gamehq.svg"
        "${CMAKE_SOURCE_DIR}/assets/icons/gamehq.ico"
)

qt_add_resources(GameHQ "sounds"
    PREFIX "/sounds"
    BASE "${CMAKE_SOURCE_DIR}/assets/sounds"
    FILES
        "${CMAKE_SOURCE_DIR}/assets/sounds/nav_tick.wav"
        "${CMAKE_SOURCE_DIR}/assets/sounds/overlay_open.wav"
        "${CMAKE_SOURCE_DIR}/assets/sounds/overlay_close.wav"
        "${CMAKE_SOURCE_DIR}/assets/sounds/favorite.wav"
        "${CMAKE_SOURCE_DIR}/assets/sounds/confirm.wav"
        "${CMAKE_SOURCE_DIR}/assets/sounds/error.wav"
        "${CMAKE_SOURCE_DIR}/assets/sounds/screenshot.wav"
        "${CMAKE_SOURCE_DIR}/assets/sounds/replay_saved.wav"
)

qt_add_resources(GameHQ "release_notes"
    PREFIX "/release-notes"
    BASE "${CMAKE_SOURCE_DIR}/assets"
    FILES "${CMAKE_SOURCE_DIR}/assets/release-notes.json"
)

target_include_directories(GameHQ PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}/generated
    ${CMAKE_SOURCE_DIR}/third_party/gameinput/include
)

target_compile_definitions(GameHQ PRIVATE
    GAMEHQ_VERSION="${GAMEHQ_VERSION_FULL}"
    UNICODE _UNICODE
)

target_link_libraries(GameHQ PRIVATE
    gamehq_release_trust
    Qt6::Core Qt6::Gui Qt6::Quick Qt6::Sql Qt6::Widgets Qt6::Svg
    Qt6::Multimedia Qt6::Network
    # `hid` backs the selective Raw HID fallback's report parsing
    # (RIDI_PREPARSEDDATA + HidP_GetUsages in RawInputApi.cpp).
    user32 gdi32 version winmm cfgmgr32 hid
    # 0.5 Windows Graphics Capture frame pump (WinRT raw-ABI path, no cppwinrt).
    windowsapp runtimeobject ole32 oleaut32 d3d11 dxgi dxguid uuid
    # t24 experimental HDR tone-map shader (runtime HLSL compile via D3DCompile).
    d3dcompiler
    # 0.5 Step 4 H.264 encode via Media Foundation SinkWriter (fragmented MP4).
    mfplat mfreadwrite mfuuid mf avrt ksuser mmdevapi
)

set_target_properties(GameHQ PROPERTIES
    WIN32_EXECUTABLE TRUE
)

# Tiny static Win32 launcher for clean package roots (docs/packaging.md).
# The packaging scripts copy it to build/GameHQ.exe or dist/GameHQ/GameHQ.exe;
# the real executable and every runtime dependency live below app/.
add_executable(GameHQLauncher WIN32
    launcher/LauncherMain.cpp
    launcher/LauncherCommandLine.cpp launcher/LauncherCommandLine.h
    launcher/UpdaterPromotion.cpp launcher/UpdaterPromotion.h
    core/UpdateMaintenance.cpp core/UpdateMaintenance.h
    ${CMAKE_SOURCE_DIR}/assets/icons/gamehq.rc
)
target_include_directories(GameHQLauncher PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_options(GameHQLauncher PRIVATE -static)

# Static updater helper: it must remain runnable while app/ and Qt DLLs are
# being replaced. Stage 1 exposes only a transaction-safe --dry-run mode.
add_executable(GameHQUpdater
    updater/UpdaterMain.cpp
    updater/UpdaterTransaction.cpp updater/UpdaterTransaction.h
    updater/UpdaterStaging.cpp updater/UpdaterStaging.h
    updater/UpdaterDataSnapshot.cpp updater/UpdaterDataSnapshot.h
    updater/UpdaterSwap.cpp updater/UpdaterSwap.h
    updater/UpdaterHealth.cpp updater/UpdaterHealth.h
    updater/UpdaterRecovery.cpp updater/UpdaterRecovery.h
    core/UpdateMaintenance.cpp core/UpdateMaintenance.h
    core/UpdaterHandshake.cpp core/UpdaterHandshake.h
)
target_include_directories(GameHQUpdater PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(GameHQUpdater PRIVATE miniz bcrypt)
target_link_libraries(GameHQUpdater PRIVATE gamehq_release_trust)
target_link_options(GameHQUpdater PRIVATE -static -municode)
target_compile_definitions(GameHQUpdater PRIVATE
    GAMEHQ_UPDATER_VERSION="${GAMEHQ_VERSION_FULL}")

# Minimal GameInput runtime probe (Phase 5 t17, docs/design/gameinput-spike.md).
# Links the vendored MIT loader from the Microsoft.GameInput package and proves
# app-local side-by-side resolution plus clean fail-soft when the runtime is
# absent. Never shipped as a startup dependency.
add_executable(GameInputRuntimeProbe
    gameinput/GameInputRuntimeProbe.cpp
    ${CMAKE_SOURCE_DIR}/third_party/gameinput/src/GameInput.cpp
)
target_include_directories(GameInputRuntimeProbe PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/gameinput/include)
target_compile_definitions(GameInputRuntimeProbe PRIVATE UNICODE _UNICODE)
target_link_options(GameInputRuntimeProbe PRIVATE -static)
