# First automated test target (0.5.71) — pure logic only: no database, no GUI,
# no game process. See docs/dev-setup.md for the run command.
#
# GameHQ is an executable, not a library, so there is nothing to link against.
# Each test compiles the handful of sources it actually needs straight into its
# own exe. That keeps the tests honest about their dependencies: if a unit needs
# half the app to build, it is not pure logic and does not belong here.
find_package(Qt6 REQUIRED COMPONENTS Test)
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

function(gamehq_add_test name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE
        "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_BINARY_DIR}/generated")
    target_link_libraries(${name} PRIVATE Qt6::Test)
    # Console subsystem, explicitly: the app sets WIN32_EXECUTABLE project-wide,
    # and a GUI-subsystem test exe still passes but writes its results to a
    # console it does not have — ctest then reports a pass with no output.
    set_target_properties(${name} PROPERTIES WIN32_EXECUTABLE FALSE)
    add_test(NAME ${name} COMMAND ${name})
    # The test exes are not deployed with windeployqt, so a plain `ctest` run
    # cannot resolve Qt6Core.dll and Windows answers with one modal "DLL was
    # not found" box per test — ctest then hangs until every box is dismissed.
    # Handing each test the project-local Qt bin directory keeps `ctest` a
    # non-interactive command.
    # Every test is bounded. Without this, one hung test consumes the whole CI
    # job budget and reports nothing useful; the slowest test here finishes in
    # about seven seconds, so two minutes is generous while still failing fast.
    # Tests that legitimately need longer raise it explicitly below.
    set_tests_properties(${name} PROPERTIES
        TIMEOUT 120
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endfunction()

gamehq_add_test(tst_gameidentity
    tst_gameidentity.cpp
    "${CMAKE_SOURCE_DIR}/src/core/GameIdentity.cpp")

# p7-1: every built-in key is explicitly live or restart-required. The policy
# stays pure; group tests prove mixed replay resets request one rebuild only.
gamehq_add_test(tst_settingsapplypolicy
    tst_settingsapplypolicy.cpp
    "${CMAKE_SOURCE_DIR}/src/config/SettingsApplyPolicy.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/ConfigManager.cpp")

gamehq_add_test(tst_controllerarbitration
    tst_controllerarbitration.cpp)

gamehq_add_test(tst_controlleridentity
    tst_controlleridentity.cpp
    "${CMAKE_SOURCE_DIR}/src/input/ControllerIdentity.cpp")

gamehq_add_test(tst_xinputidentity
    tst_xinputidentity.cpp
    "${CMAKE_SOURCE_DIR}/src/input/Gamepad.h"
    "${CMAKE_SOURCE_DIR}/src/input/XInputDevice.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/ControllerIdentity.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/InputDiagnostics.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/PerfTrace.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/ControlId.cpp")

gamehq_add_test(tst_sonyreportlayout
    tst_sonyreportlayout.cpp)

# cpo-c01/cpo-c02: one wired DualSense reaches GameHQ through the Sony HID,
# XInput and WinMM backends at once. Each backend normalizes raw buttons
# independently, so this drives the three pure mapping tables directly and
# asserts the same physical button always yields the same canonical control.
# Without it, gamepad.button.0 silently meant L2 on one API and the left stick
# click on another, and a saved binding matched only half the time.
gamehq_add_test(tst_providercontrolmap
    tst_providercontrolmap.cpp
    # Gamepad.h has no .cpp of its own, so AUTOMOC only sees its Q_OBJECT when
    # the header is listed here explicitly.
    "${CMAKE_SOURCE_DIR}/src/input/Gamepad.h"
    "${CMAKE_SOURCE_DIR}/src/input/WinMMDevice.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/XInputDevice.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/DualSenseDevice.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/InputDiagnostics.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/InputRateMonitor.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/RawInputApi.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/HidCloakMonitor.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/SelectiveRawHidFallback.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/PerfTrace.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/ControllerIdentity.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/ControlId.cpp")
target_link_libraries(tst_providercontrolmap PRIVATE user32 cfgmgr32 hid winmm)

# Pure policy test: no database, no Qt GUI. Covers the frozen decision order in
# BindingRelation that both the editor and the runtime depend on.
gamehq_add_test(tst_bindingrelation
    tst_bindingrelation.cpp
    "${CMAKE_SOURCE_DIR}/src/input/ActionCatalog.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/BindingPattern.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/BindingRelation.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/ContextOverrideCatalog.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/ControlId.cpp")

# Pure model test: the trigger/gesture specs, their canonical serialization and
# the validation that guards the parse boundary. No database, no device.
gamehq_add_test(tst_bindingpattern
    tst_bindingpattern.cpp
    "${CMAKE_SOURCE_DIR}/src/input/BindingPattern.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/ControlId.cpp")

# The recognizer's timing state machine, driven through a fake facts provider:
# no database, no device, no binding table.
gamehq_add_test(tst_inputpattern
    tst_inputpattern.cpp
    "${CMAKE_SOURCE_DIR}/src/input/ActionCatalog.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/BindingPattern.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/ControlId.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/InputPatternRecognizer.cpp")

# Clamping and the one-release migration behind the gesture timing settings.
gamehq_add_test(tst_gesturetiming
    tst_gesturetiming.cpp
    "${CMAKE_SOURCE_DIR}/src/input/ActionCatalog.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/BindingPattern.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/ControlId.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/GestureTiming.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/InputPatternRecognizer.cpp")

gamehq_add_test(tst_inputratemonitor
    tst_inputratemonitor.cpp
    "${CMAKE_SOURCE_DIR}/src/input/InputRateMonitor.cpp")

# GameInput callback lifetime, bounded queue and Qt-thread handoff. Production
# loading is compilation-covered by GameHQ; this target drives the same wrapper
# through the deterministic fake API and never touches machine hardware.
gamehq_add_test(tst_gameinputwrapper
    tst_gameinputwrapper.cpp
    "${CMAKE_SOURCE_DIR}/src/gameinput/FakeGameInputApi.cpp"
    "${CMAKE_SOURCE_DIR}/src/gameinput/GameInputEventQueue.cpp"
    "${CMAKE_SOURCE_DIR}/src/gameinput/GameInputQtDispatcher.cpp"
    "${CMAKE_SOURCE_DIR}/src/gameinput/GameInputQtDispatcher.h"
    "${CMAKE_SOURCE_DIR}/src/gameinput/GameInputWrapper.cpp"
    "${CMAKE_SOURCE_DIR}/src/gameinput/GameInputWrapper.h")

gamehq_add_test(tst_gameinputrouter
    tst_gameinputrouter.cpp
    "${CMAKE_SOURCE_DIR}/src/gameinput/FakeGameInputApi.cpp"
    "${CMAKE_SOURCE_DIR}/src/gameinput/GameInputEventQueue.cpp"
    "${CMAKE_SOURCE_DIR}/src/gameinput/GameInputQtDispatcher.cpp"
    "${CMAKE_SOURCE_DIR}/src/gameinput/GameInputQtDispatcher.h"
    "${CMAKE_SOURCE_DIR}/src/gameinput/GameInputWrapper.cpp"
    "${CMAKE_SOURCE_DIR}/src/gameinput/GameInputWrapper.h"
    "${CMAKE_SOURCE_DIR}/src/gameinput/GameInputRouter.cpp"
    "${CMAKE_SOURCE_DIR}/src/gameinput/GameInputRouter.h"
    "${CMAKE_SOURCE_DIR}/src/input/PhysicalControllerRegistry.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/CapabilityEventRouter.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/ProviderIntegration.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/ExtraButtonCatalog.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/ActionCatalog.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/BindingPattern.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/InputDiagnostics.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/ControlId.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/CaptureDatabase.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/CaptureQueries.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/GameRowRepair.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/GameMetadataBackfill.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/GameIconCache.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/Paths.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/LegacyMigration.cpp"
    "${CMAKE_SOURCE_DIR}/src/core/GameIdentity.cpp")
target_link_libraries(tst_gameinputrouter PRIVATE Qt6::Sql Qt6::Gui Qt6::Widgets)

gamehq_add_test(tst_gamepadcontrols
    tst_gamepadcontrols.cpp
    "${CMAKE_SOURCE_DIR}/src/input/Gamepad.h"
    "${CMAKE_SOURCE_DIR}/src/input/ControlId.cpp")

gamehq_add_test(tst_physicalcontrollerregistry
    tst_physicalcontrollerregistry.cpp
    "${CMAKE_SOURCE_DIR}/src/input/PhysicalControllerRegistry.cpp")

# cpo-c03b investigation slice: the PnP container seam is the only new
# production unit. It has no callers yet by design (no correlation wiring until
# container equality with the GameInput side is proven), so it is compiled and
# tested on its own; the live half of the evidence comes from
# tools/spikes/pnp_container_probe.cpp on real hardware.
gamehq_add_test(tst_pnpdeviceapi
    tst_pnpdeviceapi.cpp
    "${CMAKE_SOURCE_DIR}/src/input/PnpDeviceApi.cpp")
target_link_libraries(tst_pnpdeviceapi PRIVATE cfgmgr32)

gamehq_add_test(tst_capabilityeventrouter
    tst_capabilityeventrouter.cpp
    "${CMAKE_SOURCE_DIR}/src/input/CapabilityEventRouter.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/PhysicalControllerRegistry.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/ControlId.cpp")

# t25 cross-provider integration: shared registry + capability router across
# Sony Raw / GameInput / XInput / WinMM / Raw HID, exactly-one-edge dedup,
# identity upgrade, and the complete v3 standard-control map.
gamehq_add_test(tst_providerintegration
    tst_providerintegration.cpp
    "${CMAKE_SOURCE_DIR}/src/input/ProviderIntegration.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/CapabilityEventRouter.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/PhysicalControllerRegistry.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/ControlId.cpp")
target_include_directories(tst_providerintegration PRIVATE
    "${CMAKE_SOURCE_DIR}/third_party/gameinput/include")

gamehq_add_test(tst_controlleracceptance
    tst_controlleracceptance.cpp
    "${CMAKE_SOURCE_DIR}/src/input/ActionCatalog.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/BindingPattern.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/BindingRuntime.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/BindingResolver.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/DefaultBindings.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/OverlayInputPolicy.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/BindingRelation.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/ContextOverrideCatalog.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/InputPatternRecognizer.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/InputDiagnostics.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/ControlId.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/ProviderIntegration.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/CapabilityEventRouter.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/PhysicalControllerRegistry.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/CaptureDatabase.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/CaptureQueries.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/GameRowRepair.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/GameMetadataBackfill.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/GameIconCache.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/Paths.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/LegacyMigration.cpp"
    "${CMAKE_SOURCE_DIR}/src/core/GameIdentity.cpp")
target_link_libraries(tst_controlleracceptance PRIVATE Qt6::Sql Qt6::Gui Qt6::Widgets)

target_include_directories(tst_gameinputrouter PRIVATE
    "${CMAKE_SOURCE_DIR}/third_party/gameinput/include")

gamehq_add_test(tst_selectiverawhid
    tst_selectiverawhid.cpp
    "${CMAKE_SOURCE_DIR}/src/input/SelectiveRawHidFallback.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/ControlId.cpp")

# Pure logic: bounded rings, probe window, privacy redaction, export layout.
gamehq_add_test(tst_inputdiagnostics
    tst_inputdiagnostics.cpp
    "${CMAKE_SOURCE_DIR}/src/input/InputDiagnostics.cpp")

# Verify + bounded-retry policy behind the overlay's foreground grab, driven
# through a fake ForegroundApi that can refuse deterministically.
gamehq_add_test(tst_foregroundacquirer
    tst_foregroundacquirer.cpp
    "${CMAKE_SOURCE_DIR}/src/overlay/ForegroundAcquirer.cpp"
    "${CMAKE_SOURCE_DIR}/src/overlay/ForegroundApi.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/InputDiagnostics.cpp")
target_link_libraries(tst_foregroundacquirer PRIVATE user32)

# cpo-o02: the single non-activating overlay presentation path. The fake seam
# proves the flag/ordering contract (including an HWND swap no machine
# reproduces on demand); the last case drives the production QWindow adapter
# against real Windows and skips only on a session that cannot hold focus.
gamehq_add_test(tst_overlaypresenter
    tst_overlaypresenter.cpp
    "${CMAKE_SOURCE_DIR}/src/overlay/OverlayPresenter.cpp"
    "${CMAKE_SOURCE_DIR}/src/overlay/ForegroundApi.cpp")
target_link_libraries(tst_overlaypresenter PRIVATE Qt6::Gui user32)

# cpo-o03: the overlay's lifetime/focus rules are pure — every Win32 query the
# manager performs is expressed as a fact, so the decisions (keep, rebind to a
# replaced game window, hide) are testable without a game, a desktop session or
# a QML window. The last case audits that the overlay's popups are still
# in-window items rather than separate native windows.
gamehq_add_test(tst_overlaylifetime
    tst_overlaylifetime.cpp
    "${CMAKE_SOURCE_DIR}/src/overlay/OverlayLifetimePolicy.cpp")
target_compile_definitions(tst_overlaylifetime PRIVATE
    GAMEHQ_SOURCE_DIR="${CMAKE_SOURCE_DIR}/src")

# cpo-o04: the overlay's input contract — what GameHQ fires while the overlay
# is visible, the escape routes, and the audit that no shipped trigger+gesture
# delivers two actions. Drives the same arbitration the runtime uses
# (OverlayInput::select, which BindingResolver::matching delegates to) against
# the shipped default table: no database, no Win32, no device.
gamehq_add_test(tst_overlayinput
    tst_overlayinput.cpp
    "${CMAKE_SOURCE_DIR}/src/input/DefaultBindings.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/OverlayInputPolicy.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/ActionCatalog.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/BindingPattern.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/ContextOverrideCatalog.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/ControlId.cpp")

# The shell contract lives in the real QML, so this suite reads it
# from the source tree (left/right is seek-only; L1/R1 switches).
target_compile_definitions(tst_overlayinput PRIVATE
    GAMEHQ_SOURCE_DIR="${CMAKE_SOURCE_DIR}/src")

# Drives the real DualSenseDevice through a fake RawInputApi. It links the
# Win32 implementation only because the production constructor references it;
# the fake overrides every call, so no test here touches the machine's devices.
gamehq_add_test(tst_rawinputflood
    tst_rawinputflood.cpp
    # Gamepad.h has no .cpp of its own, so AUTOMOC only sees its Q_OBJECT when
    # the header is listed here explicitly.
    "${CMAKE_SOURCE_DIR}/src/input/Gamepad.h"
    "${CMAKE_SOURCE_DIR}/src/input/DualSenseDevice.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/InputDiagnostics.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/InputRateMonitor.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/RawInputApi.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/HidCloakMonitor.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/SelectiveRawHidFallback.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/ControlId.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/ControllerIdentity.cpp")
target_link_libraries(tst_rawinputflood PRIVATE user32 cfgmgr32 hid)

# WM_HOTKEY dispatch reentrancy: synthesized messages through the real native
# event filter. Registers real (exotic) chords, so it skips if the OS refuses.
gamehq_add_test(tst_hotkeyreentrancy
    tst_hotkeyreentrancy.cpp
    "${CMAKE_SOURCE_DIR}/src/input/HotkeyManager.cpp")
target_link_libraries(tst_hotkeyreentrancy PRIVATE user32)

# Lazy mouse-hook activation (GitHub stutter report): hook only while a mouse
# binding/capture needs it, idempotent lifecycle on a dedicated thread, held
# buttons released on stop. Installs a real WH_MOUSE_LL, skips if refused.
gamehq_add_test(tst_mousehooklazy
    tst_mousehooklazy.cpp
    "${CMAKE_SOURCE_DIR}/src/input/MouseHookDevice.cpp")
target_link_libraries(tst_mousehooklazy PRIVATE user32)

# Owning-engine teardown, with real mouse parsing/hook lifecycle and isolated DB.
gamehq_add_test(tst_inputengineshutdown
    tst_inputengineshutdown.cpp
    "${CMAKE_SOURCE_DIR}/src/input/Gamepad.h")
foreach(unit IN ITEMS
    InputEngine MouseHookDevice BindingRuntime BindingEditorModel BindingResolver
    MappingPresetMaterializer MappingAssignmentResolver
    BindingPattern BindingRelation ContextOverrideCatalog InputPatternRecognizer
    ActionCatalog ControlId ControllerIdentity GestureTiming HotkeyManager
    DualSenseDevice XInputDevice WinMMDevice RawInputApi HidCloakMonitor
    SelectiveRawHidFallback RawHidBindingCatalog ProviderIntegration
    PhysicalControllerRegistry CapabilityEventRouter ExtraButtonCatalog
    InputDiagnostics InputRateMonitor PerfTrace EventLoopStallMonitor
    DefaultBindings OverlayInputPolicy)
    target_sources(tst_inputengineshutdown PRIVATE "${CMAKE_SOURCE_DIR}/src/input/${unit}.cpp")
endforeach()
foreach(unit IN ITEMS GameInputRouter ProductionGameInputApi GameInputWrapper
    GameInputEventQueue GameInputQtDispatcher)
    target_sources(tst_inputengineshutdown PRIVATE "${CMAKE_SOURCE_DIR}/src/gameinput/${unit}.cpp")
endforeach()
target_sources(tst_inputengineshutdown PRIVATE
    "${CMAKE_SOURCE_DIR}/src/capture/CaptureRequest.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/ConfigManager.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/Paths.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/LegacyMigration.cpp"
    "${CMAKE_SOURCE_DIR}/src/core/GameIdentity.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/CaptureDatabase.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/CaptureQueries.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/GameRowRepair.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/GameMetadataBackfill.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/GameIconCache.cpp")
target_include_directories(tst_inputengineshutdown PRIVATE
    "${CMAKE_SOURCE_DIR}/third_party/gameinput/include")
target_link_libraries(tst_inputengineshutdown PRIVATE
    Qt6::Sql Qt6::Gui Qt6::Widgets user32 winmm cfgmgr32 hid)

gamehq_add_test(tst_configmanager
    tst_configmanager.cpp
    "${CMAKE_SOURCE_DIR}/src/config/ConfigManager.cpp")

gamehq_add_test(tst_navigationstate
    tst_navigationstate.cpp
    "${CMAKE_SOURCE_DIR}/src/ui/NavigationState.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/ConfigManager.cpp")

gamehq_add_test(tst_windowplacement
    tst_windowplacement.cpp
    "${CMAKE_SOURCE_DIR}/src/core/WindowPlacement.cpp")

gamehq_add_test(tst_portableprofileimporter
    tst_portableprofileimporter.cpp
    "${CMAKE_SOURCE_DIR}/src/config/PortableProfileImporter.cpp")
target_link_libraries(tst_portableprofileimporter PRIVATE Qt6::Sql)

gamehq_add_test(tst_gamerowrepair
    tst_gamerowrepair.cpp
    "${CMAKE_SOURCE_DIR}/src/storage/GameRowRepair.cpp"
    "${CMAKE_SOURCE_DIR}/src/core/GameIdentity.cpp")
# GameRowRepair.cpp's other half (normalizeDuplicateNames) talks to QSqlDatabase,
# so the translation unit needs Qt::Sql to link even though the tested function
# does not touch it.
target_link_libraries(tst_gamerowrepair PRIVATE Qt6::Sql)

gamehq_add_test(tst_versionnumber
    tst_versionnumber.cpp
    "${CMAKE_SOURCE_DIR}/src/updates/VersionNumber.cpp")

gamehq_add_test(tst_remotereleasenotes
    tst_remotereleasenotes.cpp
    "${CMAKE_SOURCE_DIR}/src/updates/RemoteReleaseNotes.cpp"
    "${CMAKE_SOURCE_DIR}/src/updates/ReleaseCatalog.cpp"
    "${CMAKE_SOURCE_DIR}/src/updates/VersionNumber.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/ReleaseNotes.cpp"
    "${CMAKE_SOURCE_DIR}/src/localization/LocaleRegistry.cpp")
target_link_libraries(tst_remotereleasenotes PRIVATE Qt6::Network)

gamehq_add_test(tst_releasenotes
    tst_releasenotes.cpp
    "${CMAKE_SOURCE_DIR}/src/app/ReleaseNotes.cpp"
    "${CMAKE_SOURCE_DIR}/src/localization/LocaleRegistry.cpp")
target_compile_definitions(tst_releasenotes PRIVATE
    GAMEHQ_RELEASE_NOTES_GENERATED_DIR="${CMAKE_SOURCE_DIR}/assets/release-notes/generated"
    GAMEHQ_RELEASE_NOTES_SOURCE_DIR="${CMAKE_SOURCE_DIR}/assets/release-notes"
    GAMEHQ_LOCALE_MANIFEST_FILE="${CMAKE_SOURCE_DIR}/i18n/locales.json")

gamehq_add_test(tst_releasetrust tst_releasetrust.cpp)
target_link_libraries(tst_releasetrust PRIVATE gamehq_release_trust)
target_compile_definitions(tst_releasetrust PRIVATE
    UPDATER_EXE="$<TARGET_FILE:GameHQUpdater>"
    GAMEHQ_RELEASE_VECTOR="${CMAKE_SOURCE_DIR}/tests/fixtures/release-trust/gamehq-test-vector.json")
add_dependencies(tst_releasetrust GameHQUpdater)

# TEST ONLY signing helper. Kept out of gamehq_release_trust so no shipped
# binary can ever produce a release signature. It pulls in the TEST-ONLY trust
# variant (src/CMakeLists.txt), which is what lets a shipping-configured build
# still verify these synthetic signatures: the trusted RFC vector key is linked
# into the test executable, never into GameHQ.exe or GameHQUpdater.exe.
add_library(gamehq_test_release_signer STATIC support/TestReleaseSigner.cpp)
target_include_directories(gamehq_test_release_signer
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/support"
    PRIVATE "${monocypher_SOURCE_DIR}/src" "${monocypher_SOURCE_DIR}/src/optional")
target_link_libraries(gamehq_test_release_signer PUBLIC gamehq_release_trust_testkeys)

gamehq_add_test(tst_releasemanifest tst_releasemanifest.cpp)
target_link_libraries(tst_releasemanifest PRIVATE gamehq_release_trust_testkeys)
target_compile_definitions(tst_releasemanifest PRIVATE
    GAMEHQ_RELEASE_VECTOR="${CMAKE_SOURCE_DIR}/tests/fixtures/release-trust/gamehq-test-vector.json")

gamehq_add_test(tst_updatemaintenance
    tst_updatemaintenance.cpp
    "${CMAKE_SOURCE_DIR}/src/core/UpdateMaintenance.cpp")

gamehq_add_test(tst_applicationmutex
    tst_applicationmutex.cpp
    "${CMAKE_SOURCE_DIR}/src/core/ApplicationMutex.cpp")

gamehq_add_test(tst_integrationprotocol
    tst_integrationprotocol.cpp
    "${CMAKE_SOURCE_DIR}/src/integration/IntegrationProtocol.cpp"
    "${CMAKE_SOURCE_DIR}/src/integration/LocalIntegrationServer.cpp"
    "${CMAKE_SOURCE_DIR}/src/integration/LocalIntegrationServer.h")
target_link_libraries(tst_integrationprotocol PRIVATE Qt6::Network)

gamehq_add_test(tst_integrationservice
    tst_integrationservice.cpp
    "${CMAKE_SOURCE_DIR}/src/integration/IntegrationProtocol.cpp"
    "${CMAKE_SOURCE_DIR}/src/integration/LocalIntegrationServer.cpp"
    "${CMAKE_SOURCE_DIR}/src/integration/LocalIntegrationServer.h"
    "${CMAKE_SOURCE_DIR}/src/integration/ExternalGameContext.cpp"
    "${CMAKE_SOURCE_DIR}/src/integration/ExternalGameContext.h"
    "${CMAKE_SOURCE_DIR}/src/integration/IntegrationService.cpp"
    "${CMAKE_SOURCE_DIR}/src/integration/IntegrationService.h")
target_link_libraries(tst_integrationservice PRIVATE Qt6::Network)

add_executable(integration_server_fixture
    integration_server_fixture.cpp
    "${CMAKE_SOURCE_DIR}/src/integration/IntegrationProtocol.cpp"
    "${CMAKE_SOURCE_DIR}/src/integration/LocalIntegrationServer.cpp"
    "${CMAKE_SOURCE_DIR}/src/integration/LocalIntegrationServer.h"
    "${CMAKE_SOURCE_DIR}/src/integration/ExternalGameContext.cpp"
    "${CMAKE_SOURCE_DIR}/src/integration/ExternalGameContext.h"
    "${CMAKE_SOURCE_DIR}/src/integration/IntegrationService.cpp"
    "${CMAKE_SOURCE_DIR}/src/integration/IntegrationService.h")
target_include_directories(integration_server_fixture PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(integration_server_fixture PRIVATE Qt6::Core Qt6::Network)
set_target_properties(integration_server_fixture PROPERTIES WIN32_EXECUTABLE FALSE)

gamehq_add_test(tst_integrationclient
    tst_integrationclient.cpp
    "${CMAKE_SOURCE_DIR}/src/integration/IntegrationProtocol.cpp"
    "${CMAKE_SOURCE_DIR}/src/integration/IntegrationClient.cpp")
target_link_libraries(tst_integrationclient PRIVATE Qt6::Network)
target_compile_definitions(tst_integrationclient PRIVATE
    INTEGRATION_SERVER_FIXTURE="$<TARGET_FILE:integration_server_fixture>")
add_dependencies(tst_integrationclient integration_server_fixture)

gamehq_add_test(tst_gamemetadatabackfill
    tst_gamemetadatabackfill.cpp
    "${CMAKE_SOURCE_DIR}/src/storage/GameMetadataBackfill.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/GameIconCache.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/Paths.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/LegacyMigration.cpp"
    "${CMAKE_SOURCE_DIR}/src/core/GameIdentity.cpp")
target_link_libraries(tst_gamemetadatabackfill PRIVATE Qt6::Sql Qt6::Gui Qt6::Widgets)

gamehq_add_test(tst_gameiconcache
    tst_gameiconcache.cpp
    "${CMAKE_SOURCE_DIR}/src/storage/GameIconCache.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/Paths.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/LegacyMigration.cpp")
target_link_libraries(tst_gameiconcache PRIVATE Qt6::Gui Qt6::Widgets)

gamehq_add_test(tst_captureiconrefresh
    tst_captureiconrefresh.cpp
    # CaptureDatabase validates preset rows through the shared binding grammar.
    "${CMAKE_SOURCE_DIR}/src/input/BindingPattern.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/ControlId.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/CaptureDatabase.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/CaptureQueries.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/GameRowRepair.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/GameMetadataBackfill.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/GameIconCache.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/Paths.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/LegacyMigration.cpp"
    "${CMAKE_SOURCE_DIR}/src/core/GameIdentity.cpp")
target_link_libraries(tst_captureiconrefresh PRIVATE Qt6::Sql Qt6::Gui Qt6::Widgets)

# Capture deletion: file/DB failure paths with an injected filesystem seam.
gamehq_add_test(tst_capturelibrarydeletion
    tst_capturelibrarydeletion.cpp
    # CaptureDatabase validates preset rows through the shared binding grammar.
    "${CMAKE_SOURCE_DIR}/src/input/BindingPattern.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/ControlId.cpp"
    "${CMAKE_SOURCE_DIR}/src/ui/CaptureLibraryService.cpp"
    "${CMAKE_SOURCE_DIR}/src/ui/GalleryModel.cpp"
    "${CMAKE_SOURCE_DIR}/src/ui/ShellActions.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/ThumbnailService.cpp"
    "${CMAKE_SOURCE_DIR}/src/capture/ReplayExporter.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/CaptureDatabase.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/CaptureQueries.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/GameRowRepair.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/GameMetadataBackfill.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/GameIconCache.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/Paths.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/LegacyMigration.cpp"
    "${CMAKE_SOURCE_DIR}/src/core/GameIdentity.cpp")
# ThumbnailService pulls ReplayExporter in for video thumbnails, which needs
# the Media Foundation import libraries even though no test decodes a clip.
target_link_libraries(tst_capturelibrarydeletion PRIVATE
    Qt6::Sql Qt6::Gui Qt6::Widgets ole32 mfplat mfreadwrite mfuuid mf)

# Capture commit: what the library actually did with a finished capture.
gamehq_add_test(tst_capturecommitoutcome
    tst_capturecommitoutcome.cpp
    # CaptureDatabase validates preset rows through the shared binding grammar.
    "${CMAKE_SOURCE_DIR}/src/input/BindingPattern.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/ControlId.cpp"
    "${CMAKE_SOURCE_DIR}/src/ui/CaptureLibraryService.cpp"
    "${CMAKE_SOURCE_DIR}/src/ui/GalleryModel.cpp"
    "${CMAKE_SOURCE_DIR}/src/ui/ShellActions.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/ThumbnailService.cpp"
    "${CMAKE_SOURCE_DIR}/src/capture/ReplayExporter.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/CaptureDatabase.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/CaptureQueries.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/GameRowRepair.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/GameMetadataBackfill.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/GameIconCache.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/Paths.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/LegacyMigration.cpp"
    "${CMAKE_SOURCE_DIR}/src/core/GameIdentity.cpp")
target_link_libraries(tst_capturecommitoutcome PRIVATE
    Qt6::Sql Qt6::Gui Qt6::Widgets ole32 mfplat mfreadwrite mfuuid mf)

gamehq_add_test(tst_capturedatabaserepair
    tst_capturedatabaserepair.cpp
    # CaptureDatabase validates preset rows through the shared binding grammar.
    "${CMAKE_SOURCE_DIR}/src/input/BindingPattern.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/ControlId.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/CaptureDatabase.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/CaptureQueries.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/GameRowRepair.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/GameMetadataBackfill.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/GameIconCache.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/Paths.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/LegacyMigration.cpp"
    "${CMAKE_SOURCE_DIR}/src/core/GameIdentity.cpp")
target_link_libraries(tst_capturedatabaserepair PRIVATE Qt6::Sql Qt6::Gui Qt6::Widgets)

gamehq_add_test(tst_bindingeditor
    tst_bindingeditor.cpp
    "${CMAKE_SOURCE_DIR}/src/input/ActionCatalog.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/InputPatternRecognizer.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/BindingEditorModel.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/BindingPattern.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/InputDiagnostics.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/BindingResolver.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/DefaultBindings.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/OverlayInputPolicy.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/BindingRelation.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/BindingRuntime.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/ContextOverrideCatalog.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/ControlId.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/ControllerIdentity.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/RawHidBindingCatalog.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/SelectiveRawHidFallback.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/CaptureDatabase.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/CaptureQueries.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/GameRowRepair.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/GameMetadataBackfill.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/GameIconCache.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/Paths.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/LegacyMigration.cpp"
    "${CMAKE_SOURCE_DIR}/src/core/GameIdentity.cpp")
target_link_libraries(tst_bindingeditor PRIVATE Qt6::Sql Qt6::Gui Qt6::Widgets)
qt_add_translations(tst_bindingeditor
    TS_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/i18n/gamehq_manager_en_US.ts"
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/i18n/gamehq_manager_pl_PL.ts"
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/i18n/gamehq_manager_zh_Hans.ts"
    RESOURCE_PREFIX "/i18n"
    LUPDATE_OPTIONS -no-obsolete
    LRELEASE_OPTIONS -idbased
)

# Catalog/default-binding invariants: unique ids, no duplicate default slots,
# every default resolves to a real action and parses as a valid pattern.
# BindingResolver::defaultBindings() is static, but its translation unit talks
# to the database, hence the storage sources and Qt::Sql below.
gamehq_add_test(tst_actioncatalog
    tst_actioncatalog.cpp
    "${CMAKE_SOURCE_DIR}/src/input/ActionCatalog.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/BindingPattern.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/BindingResolver.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/DefaultBindings.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/OverlayInputPolicy.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/ContextOverrideCatalog.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/ControlId.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/InputDiagnostics.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/CaptureDatabase.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/CaptureQueries.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/GameRowRepair.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/GameMetadataBackfill.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/GameIconCache.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/Paths.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/LegacyMigration.cpp"
    "${CMAKE_SOURCE_DIR}/src/core/GameIdentity.cpp")
target_link_libraries(tst_actioncatalog PRIVATE Qt6::Sql Qt6::Gui Qt6::Widgets)

# Schema v3 -> v4 compatibility: every stored activation keeps its meaning.
gamehq_add_test(tst_bindingmigration
    tst_bindingmigration.cpp
    "${CMAKE_SOURCE_DIR}/src/input/ActionCatalog.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/BindingPattern.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/BindingResolver.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/DefaultBindings.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/OverlayInputPolicy.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/ContextOverrideCatalog.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/ControlId.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/ExtraButtonCatalog.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/InputDiagnostics.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/CaptureDatabase.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/CaptureQueries.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/GameRowRepair.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/GameMetadataBackfill.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/GameIconCache.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/Paths.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/LegacyMigration.cpp"
    "${CMAKE_SOURCE_DIR}/src/core/GameIdentity.cpp")
target_link_libraries(tst_bindingmigration PRIVATE Qt6::Sql Qt6::Gui Qt6::Widgets)

# Mapping-preset storage (schema v8): transactional CRUD, typed targets and the
# rollback guarantees a later migration has to trust (docs/mapping-presets.md).
gamehq_add_test(tst_mappingpresetstorage
    tst_mappingpresetstorage.cpp
    "${CMAKE_SOURCE_DIR}/src/input/ActionCatalog.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/BindingPattern.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/BindingResolver.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/DefaultBindings.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/OverlayInputPolicy.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/ContextOverrideCatalog.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/ControlId.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/ExtraButtonCatalog.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/InputDiagnostics.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/CaptureDatabase.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/CaptureQueries.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/GameRowRepair.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/GameMetadataBackfill.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/GameIconCache.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/Paths.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/LegacyMigration.cpp"
    "${CMAKE_SOURCE_DIR}/src/core/GameIdentity.cpp")
target_link_libraries(tst_mappingpresetstorage PRIVATE Qt6::Sql Qt6::Gui Qt6::Widgets)

# Runtime materialization of the mapping migration (cpo-p03): the bounded
# compatibility phase that folds a live alias chain, proves the persisted
# content equal to legacy resolution and only then promotes the chain.
gamehq_add_test(tst_presetmaterializer
    tst_presetmaterializer.cpp
    "${CMAKE_SOURCE_DIR}/src/input/ActionCatalog.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/BindingPattern.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/BindingResolver.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/DefaultBindings.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/OverlayInputPolicy.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/ContextOverrideCatalog.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/ControlId.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/ExtraButtonCatalog.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/InputDiagnostics.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/MappingPresetMaterializer.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/CaptureDatabase.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/CaptureQueries.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/GameRowRepair.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/GameMetadataBackfill.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/GameIconCache.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/Paths.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/LegacyMigration.cpp"
    "${CMAKE_SOURCE_DIR}/src/core/GameIdentity.cpp")
target_link_libraries(tst_presetmaterializer PRIVATE Qt6::Sql Qt6::Gui Qt6::Widgets)

# Assignment resolution (cpo-p04): the winner rule game -> controller ->
# group_default -> builtin, with the source named and broken assignments
# stepping down the chain (docs/mapping-presets.md sections 4 and 7).
gamehq_add_test(tst_mappingassignmentresolver
    tst_mappingassignmentresolver.cpp
    "${CMAKE_SOURCE_DIR}/src/input/MappingAssignmentResolver.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/ActionCatalog.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/BindingPattern.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/BindingResolver.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/DefaultBindings.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/OverlayInputPolicy.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/ContextOverrideCatalog.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/ControlId.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/ExtraButtonCatalog.cpp"
    "${CMAKE_SOURCE_DIR}/src/input/InputDiagnostics.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/CaptureDatabase.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/CaptureQueries.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/GameRowRepair.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/GameMetadataBackfill.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/GameIconCache.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/Paths.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/LegacyMigration.cpp"
    "${CMAKE_SOURCE_DIR}/src/core/GameIdentity.cpp")
target_link_libraries(tst_mappingassignmentresolver PRIVATE Qt6::Sql Qt6::Gui Qt6::Widgets)

# Stand-in for app\GameHQ.exe: it reports the arguments Windows actually handed
# it, so the launcher's pass-through can be checked against a real process.
gamehq_add_test(tst_updateschedule
    tst_updateschedule.cpp
    "${CMAKE_SOURCE_DIR}/src/updates/UpdateSchedule.cpp")

gamehq_add_test(tst_releasecatalog
    tst_releasecatalog.cpp
    "${CMAKE_SOURCE_DIR}/src/updates/ReleaseCatalog.cpp"
    "${CMAKE_SOURCE_DIR}/src/updates/VersionNumber.cpp")

gamehq_add_test(tst_logrotation
    tst_logrotation.cpp
    "${CMAKE_SOURCE_DIR}/src/diagnostics/Logger.cpp")

add_executable(process_identity_fixture process_identity_fixture.cpp)
target_link_options(process_identity_fixture PRIVATE -static -municode)
set_target_properties(process_identity_fixture PROPERTIES WIN32_EXECUTABLE FALSE)

gamehq_add_test(tst_processidentity
    tst_processidentity.cpp
    "${CMAKE_SOURCE_DIR}/src/core/ProcessIdentity.cpp")
target_compile_definitions(tst_processidentity PRIVATE
    PROCESS_IDENTITY_FIXTURE="$<TARGET_FILE:process_identity_fixture>")
add_dependencies(tst_processidentity process_identity_fixture)

gamehq_add_test(tst_storedpaths
    tst_storedpaths.cpp
    "${CMAKE_SOURCE_DIR}/src/config/Paths.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/LegacyMigration.cpp")

gamehq_add_test(tst_capturepublisher
    tst_capturepublisher.cpp
    "${CMAKE_SOURCE_DIR}/src/capture/CapturePublisher.cpp")

# Actual recorder restore/trim and export-task lifetime with file fixtures;
# no capture device, encoder or muxer execution.
gamehq_add_test(tst_segmentrecorder
    tst_segmentrecorder.cpp
    "${CMAKE_SOURCE_DIR}/src/capture/SegmentRecorder.cpp"
    "${CMAKE_SOURCE_DIR}/src/capture/SegmentLease.cpp")
target_link_libraries(tst_segmentrecorder PRIVATE
    d3d11 dxguid ole32 mfplat mfreadwrite mfuuid)

gamehq_add_test(tst_framepumpservice
    tst_framepumpservice.cpp
    "${CMAKE_SOURCE_DIR}/src/capture/ReplayBufferState.cpp"
    "${CMAKE_SOURCE_DIR}/src/capture/ReplayBufferState.h")

# Capture-border verdict (p4-2): pure derivation, no WinRT and no capture device.
gamehq_add_test(tst_captureborderstate
    tst_captureborderstate.cpp
    "${CMAKE_SOURCE_DIR}/src/config/ConfigManager.cpp"
    "${CMAKE_SOURCE_DIR}/src/capture/CaptureBorderState.cpp"
    "${CMAKE_SOURCE_DIR}/src/capture/CaptureBorderState.h")

# p1-2: the identity that ties one capture press to its outcome in the log.
gamehq_add_test(tst_capturerequest
    tst_capturerequest.cpp
    "${CMAKE_SOURCE_DIR}/src/capture/CaptureRequest.cpp"
    "${CMAKE_SOURCE_DIR}/src/capture/CaptureRequest.h")

# p6-2/p6-3: which level an event answers to, how a slider percentage becomes an
# amplitude, and what the engine reports while its effects load. No audio device
# and no asynchronous loading to wait on.
gamehq_add_test(tst_soundengine
    tst_soundengine.cpp
    "${CMAKE_SOURCE_DIR}/src/sound/SoundLoadTracker.cpp"
    "${CMAKE_SOURCE_DIR}/src/sound/SoundLoadTracker.h"
    "${CMAKE_SOURCE_DIR}/src/sound/SoundLevels.cpp"
    "${CMAKE_SOURCE_DIR}/src/sound/SoundLevels.h"
    "${CMAKE_SOURCE_DIR}/src/config/ConfigManager.cpp")
# Qt Multimedia is linked for QtAudio::convertVolume only: it is a plain
# conversion function and opens no audio device.
target_link_libraries(tst_soundengine PRIVATE Qt6::Multimedia)

add_executable(launcher_argecho_fixture launcher_argecho_fixture.cpp)
target_link_options(launcher_argecho_fixture PRIVATE -static -municode)
target_link_libraries(launcher_argecho_fixture PRIVATE shell32)
set_target_properties(launcher_argecho_fixture PROPERTIES WIN32_EXECUTABLE FALSE)

gamehq_add_test(tst_launchercommandline
    tst_launchercommandline.cpp
    "${CMAKE_SOURCE_DIR}/src/launcher/LauncherCommandLine.cpp")
target_compile_definitions(tst_launchercommandline PRIVATE
    LAUNCHER_ARGECHO_FIXTURE="$<TARGET_FILE:launcher_argecho_fixture>")
add_dependencies(tst_launchercommandline launcher_argecho_fixture)

gamehq_add_test(tst_launcherlocalization
    tst_launcherlocalization.cpp
    "${CMAKE_SOURCE_DIR}/src/launcher/LauncherLocalization.cpp"
    "${CMAKE_SOURCE_DIR}/src/launcher/LauncherStrings.rc")
target_compile_definitions(tst_launcherlocalization PRIVATE
    GAMEHQ_LOCALES_JSON="${CMAKE_SOURCE_DIR}/i18n/locales.json"
    LAUNCHER_MAIN_SOURCE="${CMAKE_SOURCE_DIR}/src/launcher/LauncherMain.cpp"
    LAUNCHER_RC_SOURCE="${CMAKE_SOURCE_DIR}/src/launcher/LauncherStrings.rc"
    LAUNCHER_CMAKE_SOURCE="${CMAKE_SOURCE_DIR}/src/CMakeLists.txt"
    LAUNCHER_PACKAGE_SOURCE="${CMAKE_SOURCE_DIR}/packaging/assemble-package.ps1")

gamehq_add_test(tst_updatedownloader
    tst_updatedownloader.cpp
    "${CMAKE_SOURCE_DIR}/src/updates/UpdateDownloader.cpp")
target_link_libraries(tst_updatedownloader PRIVATE Qt6::Network gamehq_test_release_signer)

gamehq_add_test(tst_updatepreflight
    tst_updatepreflight.cpp
    "${CMAKE_SOURCE_DIR}/src/updates/UpdatePreflight.cpp")

gamehq_add_test(tst_hdrtonemap
    tst_hdrtonemap.cpp
    "${CMAKE_SOURCE_DIR}/src/capture/hdr/HdrToneMapMath.cpp")

gamehq_add_test(tst_hdrcapabilities
    tst_hdrcapabilities.cpp
    "${CMAKE_SOURCE_DIR}/src/capture/HdrCapabilities.cpp")
target_link_libraries(tst_hdrcapabilities PRIVATE
    user32 dxgi ole32 mfplat mfuuid mf)

# GPU smoke test (t24): needs a real D3D11 device, so it is not "pure logic"
# like the tests above — it self-skips via QSKIP when no D3D11 hardware
# device or no FP16 support is available (see the test file header comment).
gamehq_add_test(tst_hdrgputonemap
    tst_hdrgputonemap.cpp
    "${CMAKE_SOURCE_DIR}/src/capture/hdr/HdrToneMapMath.cpp"
    "${CMAKE_SOURCE_DIR}/src/capture/hdr/GpuToneMapper.cpp")
target_link_libraries(tst_hdrgputonemap PRIVATE d3d11 d3dcompiler)

gamehq_add_test(tst_updateinstaller
    tst_updateinstaller.cpp
    "${CMAKE_SOURCE_DIR}/src/updates/UpdateInstaller.cpp"
    "${CMAKE_SOURCE_DIR}/src/updates/UpdateDownloader.cpp"
    "${CMAKE_SOURCE_DIR}/src/updater/UpdaterTransaction.cpp"
    "${CMAKE_SOURCE_DIR}/src/core/UpdaterHandshake.cpp")
target_link_libraries(tst_updateinstaller PRIVATE Qt6::Network gamehq_test_release_signer)

gamehq_add_test(tst_updatertransaction
    tst_updatertransaction.cpp
    "${CMAKE_SOURCE_DIR}/src/updater/UpdaterDataSnapshot.cpp"
    "${CMAKE_SOURCE_DIR}/src/updater/UpdaterSwap.cpp"
    "${CMAKE_SOURCE_DIR}/src/updater/UpdaterHealth.cpp"
    "${CMAKE_SOURCE_DIR}/src/updater/UpdaterRecovery.cpp"
    "${CMAKE_SOURCE_DIR}/src/core/UpdateMaintenance.cpp"
    "${CMAKE_SOURCE_DIR}/src/updater/UpdaterStaging.cpp"
    "${CMAKE_SOURCE_DIR}/src/updater/UpdaterTransaction.cpp"
    "${CMAKE_SOURCE_DIR}/src/launcher/UpdaterPromotion.cpp")
add_executable(updater_health_fixture updater_health_fixture.cpp)
target_link_options(updater_health_fixture PRIVATE -static -municode)
# UPDATER_EXE drives the transaction behaviour over synthetic signed releases,
# so it is the TEST-ONLY trust twin. PRODUCTION_UPDATER_EXE is the helper the
# release actually ships: the suite asserts that it refuses exactly the same
# material, which is what proves the test key never reaches a shipped binary.
target_compile_definitions(tst_updatertransaction PRIVATE
    UPDATER_EXE="$<TARGET_FILE:updater_test_trust_fixture>"
    PRODUCTION_UPDATER_EXE="$<TARGET_FILE:GameHQUpdater>"
    HEALTH_FIXTURE_EXE="$<TARGET_FILE:updater_health_fixture>"
    GAMEHQ_UPDATER_VERSION="${GAMEHQ_VERSION_FULL}")
add_dependencies(tst_updatertransaction GameHQUpdater updater_test_trust_fixture
                 updater_health_fixture)
target_link_libraries(tst_updatertransaction PRIVATE miniz bcrypt Qt6::Sql gamehq_test_release_signer)

# The end-to-end updater tests drive real child processes through swap,
# rollback and recovery, so they need more than the default bound - especially
# on a shared CI runner.
set_tests_properties(tst_updatertransaction tst_updateinstaller PROPERTIES TIMEOUT 600)
set_tests_properties(tst_integrationservice tst_integrationclient
                     tst_portableprofileimporter PROPERTIES TIMEOUT 300)

# Synthetic IDs live only in this test target. They exercise the same ID-based
# lrelease/resource path as production without polluting the en-US app catalog.
gamehq_add_test(tst_localizationcatalog
    tst_localizationcatalog.cpp)
target_compile_definitions(tst_localizationcatalog PRIVATE
    GAMEHQ_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
qt_add_translations(tst_localizationcatalog
    TS_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/i18n/gamehq_test_en_US.ts"
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/i18n/gamehq_test_pl_PL.ts"
        "${CMAKE_SOURCE_DIR}/i18n/app/gamehq_en_US.ts"
        "${CMAKE_SOURCE_DIR}/i18n/app/gamehq_th_TH.ts"
        "${CMAKE_SOURCE_DIR}/i18n/app/gamehq_es_419.ts"
        "${CMAKE_SOURCE_DIR}/i18n/app/gamehq_uk_UA.ts"
        "${CMAKE_SOURCE_DIR}/i18n/app/gamehq_it_IT.ts"
    RESOURCE_PREFIX "/i18n"
    LUPDATE_OPTIONS -no-obsolete
    LRELEASE_OPTIONS -idbased
)

# p4-7: inventory independently packaged runtimes and developer tools, then
# prove each one has an explicit native localization boundary.
add_test(NAME tst_auxiliarysurfaceaudit
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tools/i18n/test_auxiliary_surfaces.py")
set_tests_properties(tst_auxiliarysurfaceaudit PROPERTIES TIMEOUT 30)

# t4 / LANG-04J: the production registry, synchronized fallback catalogs,
# generated state, resources, aliases, and pseudo-locale boundary stay aligned.
add_test(NAME tst_launchportfolio
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tools/i18n/test_launch_portfolio.py")
set_tests_properties(tst_launchportfolio PROPERTIES TIMEOUT 30)

add_test(NAME tst_innolanguages
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tools/i18n/test_inno_languages.py")
set_tests_properties(tst_innolanguages PROPERTIES TIMEOUT 30)

gamehq_add_test(tst_languagemanager
    tst_languagemanager.cpp
    "${CMAKE_SOURCE_DIR}/src/localization/LocaleRegistry.cpp"
    "${CMAKE_SOURCE_DIR}/src/localization/LanguageManager.cpp"
    "${CMAKE_SOURCE_DIR}/src/localization/StartupLocalization.cpp")
qt_add_translations(tst_languagemanager
    TS_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/i18n/gamehq_manager_en_US.ts"
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/i18n/gamehq_manager_de_DE.ts"
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/i18n/gamehq_manager_pl_PL.ts"
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/i18n/gamehq_manager_zh_Hans.ts"
        "${CMAKE_SOURCE_DIR}/i18n/app/gamehq_en_US.ts"
        "${CMAKE_SOURCE_DIR}/i18n/app/gamehq_th_TH.ts"
        "${CMAKE_SOURCE_DIR}/i18n/app/gamehq_es_419.ts"
        "${CMAKE_SOURCE_DIR}/i18n/app/gamehq_uk_UA.ts"
        "${CMAKE_SOURCE_DIR}/i18n/app/gamehq_it_IT.ts"
    RESOURCE_PREFIX "/i18n"
    LUPDATE_OPTIONS -no-obsolete
    LRELEASE_OPTIONS -idbased
)
qt_add_resources(tst_languagemanager "language_manager_fixtures"
    PREFIX "/i18n"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/i18n"
    FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/i18n/locales-test.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/i18n/corrupt.qm"
)
qt_add_resources(tst_languagemanager "production_locale_manifest"
    PREFIX "/i18n"
    BASE "${CMAKE_SOURCE_DIR}/i18n"
    FILES "${CMAKE_SOURCE_DIR}/i18n/locales.json"
)

gamehq_add_test(tst_pseudolocales
    tst_pseudolocales.cpp
    "${CMAKE_SOURCE_DIR}/src/localization/LocaleRegistry.cpp"
    "${CMAKE_SOURCE_DIR}/src/localization/LanguageManager.cpp")
target_link_libraries(tst_pseudolocales PRIVATE Qt6::Gui Qt6::Quick)
target_compile_definitions(tst_pseudolocales PRIVATE
    GAMEHQ_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
qt_add_translations(tst_pseudolocales
    TS_FILES
        "${CMAKE_SOURCE_DIR}/i18n/app/gamehq_en_US.ts"
        "${GAMEHQ_PSEUDO_I18N_DIR}/gamehq_en_XA.ts"
        "${GAMEHQ_PSEUDO_I18N_DIR}/gamehq_ar_XB.ts"
    RESOURCE_PREFIX "/i18n"
    LUPDATE_OPTIONS -no-obsolete
    LRELEASE_OPTIONS -idbased)
qt_add_resources(tst_pseudolocales "pseudo_locale_manifest"
    PREFIX "/pseudo-i18n"
    BASE "${GAMEHQ_PSEUDO_I18N_DIR}"
    FILES "${GAMEHQ_PSEUDO_MANIFEST}")

gamehq_add_test(tst_languagepreference
    tst_languagepreference.cpp
    "${CMAKE_SOURCE_DIR}/src/config/ConfigManager.cpp"
    "${CMAKE_SOURCE_DIR}/src/localization/LocaleRegistry.cpp"
    "${CMAKE_SOURCE_DIR}/src/localization/LanguageManager.cpp"
    "${CMAKE_SOURCE_DIR}/src/localization/LanguagePreference.cpp")
qt_add_translations(tst_languagepreference
    TS_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/i18n/gamehq_manager_en_US.ts"
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/i18n/gamehq_manager_pl_PL.ts"
    RESOURCE_PREFIX "/i18n"
    LUPDATE_OPTIONS -no-obsolete
    LRELEASE_OPTIONS -idbased
)
qt_add_resources(tst_languagepreference "language_preference_fixtures"
    PREFIX "/i18n"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/i18n"
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/i18n/locales-test.json"
)

gamehq_add_test(tst_liveretranslation
    tst_liveretranslation.cpp
    "${CMAKE_SOURCE_DIR}/src/input/ActionCatalog.cpp"
    "${CMAKE_SOURCE_DIR}/src/localization/LocaleRegistry.cpp"
    "${CMAKE_SOURCE_DIR}/src/localization/LanguageManager.cpp"
    "${CMAKE_SOURCE_DIR}/src/tray/TrayIcon.cpp")
target_link_libraries(tst_liveretranslation PRIVATE Qt6::Quick Qt6::Widgets)
target_compile_definitions(tst_liveretranslation PRIVATE
    GAMEHQ_VERSION="${GAMEHQ_VERSION_FULL}")
qt_add_translations(tst_liveretranslation
    TS_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/i18n/gamehq_manager_en_US.ts"
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/i18n/gamehq_manager_pl_PL.ts"
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/i18n/gamehq_manager_zh_Hans.ts"
    RESOURCE_PREFIX "/i18n"
    LUPDATE_OPTIONS -no-obsolete
    LRELEASE_OPTIONS -idbased
)
qt_add_resources(tst_liveretranslation "live_retranslation_fixtures"
    PREFIX "/i18n"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/i18n"
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/i18n/locales-test.json"
)
set_tests_properties(tst_liveretranslation PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# The catalog synchronizer is an explicit developer tool. This fixture test
# proves stable extraction/catalog bytes and check-mode failure without touching
# production source or translation catalogs.
add_test(NAME tst_i18nsync
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tools/i18n/test_sync.py"
        --lupdate "$<TARGET_FILE:Qt6::lupdate>")
set_tests_properties(tst_i18nsync PROPERTIES TIMEOUT 120)

add_test(NAME tst_i18npseudo
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tools/i18n/test_pseudo.py")
set_tests_properties(tst_i18npseudo PROPERTIES TIMEOUT 120)

add_test(NAME tst_i18nverify
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tools/i18n/test_verify.py")
set_tests_properties(tst_i18nverify PROPERTIES TIMEOUT 120)

# p7-1: the checked-in CI wiring remains offline and a stale generated locale
# fails with an artifact-specific diagnostic without rewriting the fixture.
add_test(NAME tst_i18nci
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tools/i18n/test_ci.py")
set_tests_properties(tst_i18nci PROPERTIES TIMEOUT 60)

add_test(NAME tst_package_localization
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tools/i18n/test_package_localization.py")
set_tests_properties(tst_package_localization PROPERTIES TIMEOUT 60)

# p8-2: deterministic merger for the per-locale runtime/package/installer
# evidence. The full artifact matrix is driven by the PowerShell entry point.
add_test(NAME tst_multilingualacceptancereport
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tools/i18n/test_multilingual_acceptance_report.py")
set_tests_properties(tst_multilingualacceptancereport PROPERTIES TIMEOUT 60)

add_test(NAME tst_i18nprotocol
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tools/i18n/test_protocol.py")
set_tests_properties(tst_i18nprotocol PROPERTIES TIMEOUT 120)

add_test(NAME tst_i18ndiff
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tools/i18n/test_check_diff.py")
set_tests_properties(tst_i18ndiff PROPERTIES TIMEOUT 120)

add_test(NAME tst_i18nselective
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tools/i18n/test_selective_translation.py"
        --lupdate "$<TARGET_FILE:Qt6::lupdate>")
set_tests_properties(tst_i18nselective PROPERTIES TIMEOUT 120)

# p6-1: the versioned release-note source contract and its generated bundles.
add_test(NAME tst_i18nreleasenotes
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tools/i18n/test_release_notes_generation.py")
set_tests_properties(tst_i18nreleasenotes PROPERTIES TIMEOUT 120)

# p6-4: publication documents are deterministic outputs of the same source and
# never an authoring source or an update-trust input.
add_test(NAME tst_i18npublication
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tools/i18n/test_release_publication.py")
set_tests_properties(tst_i18npublication PROPERTIES TIMEOUT 120)

# p6-5: one release-facing command verifies sources, generated bundles,
# publication output, UTF-8 and integrity metadata without network access.
add_test(NAME tst_releasenoteassets
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/packaging/test-release-note-assets.ps1"
        -PythonExecutable "${Python3_EXECUTABLE}")
set_tests_properties(tst_releasenoteassets PROPERTIES TIMEOUT 180)

# p7-3: the one-command locale lifecycle and its contributor guide. Every
# mutating case runs against an isolated temporary root with a fixture locale.
add_test(NAME tst_i18nlocalelifecycle
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tools/i18n/test_locale_lifecycle.py")
set_tests_properties(tst_i18nlocalelifecycle PROPERTIES TIMEOUT 180)

# Real service entry points, deterministic update gate, no capture hardware.
gamehq_add_test(tst_captureacknowledgement
    tst_captureacknowledgement.cpp
    "${CMAKE_SOURCE_DIR}/src/capture/FramePumpService.cpp"
    "${CMAKE_SOURCE_DIR}/src/capture/ScreenshotService.cpp"
    "${CMAKE_SOURCE_DIR}/src/capture/CaptureRequest.cpp"
    "${CMAKE_SOURCE_DIR}/src/capture/CaptureBorderState.cpp"
    "${CMAKE_SOURCE_DIR}/src/capture/CapturePublisher.cpp"
    "${CMAKE_SOURCE_DIR}/src/capture/ReplayBufferState.cpp"
    "${CMAKE_SOURCE_DIR}/src/capture/HdrCapabilities.cpp"
    "${CMAKE_SOURCE_DIR}/src/capture/hdr/GpuToneMapper.cpp"
    "${CMAKE_SOURCE_DIR}/src/capture/hdr/HdrToneMapMath.cpp"
    "${CMAKE_SOURCE_DIR}/src/capture/AudioCapture.cpp"
    "${CMAKE_SOURCE_DIR}/src/capture/SegmentRecorder.cpp"
    "${CMAKE_SOURCE_DIR}/src/capture/SegmentLease.cpp"
    "${CMAKE_SOURCE_DIR}/src/capture/ReplayExporter.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/ConfigManager.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/CaptureLocations.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/Paths.cpp"
    "${CMAKE_SOURCE_DIR}/src/config/LegacyMigration.cpp"
    "${CMAKE_SOURCE_DIR}/src/core/GameIdentity.cpp"
    "${CMAKE_SOURCE_DIR}/src/games/GameDetector.cpp"
    "${CMAKE_SOURCE_DIR}/src/integration/ExternalGameContext.cpp"
    "${CMAKE_SOURCE_DIR}/src/storage/ThumbnailService.cpp"
)
target_compile_definitions(tst_captureacknowledgement PRIVATE UNICODE _UNICODE)
target_link_libraries(tst_captureacknowledgement PRIVATE
    Qt6::Gui Qt6::Widgets Qt6::Network
    user32 gdi32 version winmm windowsapp runtimeobject ole32 oleaut32
    d3d11 dxgi dxguid uuid d3dcompiler mfplat mfreadwrite mfuuid mf avrt ksuser mmdevapi)
# Worker startup sweeps its replay cache. Keep that cache isolated from user data.
set_target_properties(tst_captureacknowledgement PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/captureacknowledgement")
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/tests/captureacknowledgement/portable.flag" CONTENT "")

# p1-5: real input dispatch and replay receipts, with synthetic device/state facts.
get_target_property(CONTROLLER_CLIP_INPUT_SOURCES tst_inputengineshutdown SOURCES)
list(REMOVE_ITEM CONTROLLER_CLIP_INPUT_SOURCES tst_inputengineshutdown.cpp)
get_target_property(CONTROLLER_CLIP_CAPTURE_SOURCES tst_captureacknowledgement SOURCES)
list(REMOVE_ITEM CONTROLLER_CLIP_CAPTURE_SOURCES tst_captureacknowledgement.cpp)
set(CONTROLLER_CLIP_SOURCES ${CONTROLLER_CLIP_INPUT_SOURCES} ${CONTROLLER_CLIP_CAPTURE_SOURCES})
list(REMOVE_DUPLICATES CONTROLLER_CLIP_SOURCES)
gamehq_add_test(tst_controllerclip_e2e tst_controllerclip_e2e.cpp ${CONTROLLER_CLIP_SOURCES})
get_target_property(CONTROLLER_CLIP_INPUT_LIBS tst_inputengineshutdown LINK_LIBRARIES)
get_target_property(CONTROLLER_CLIP_CAPTURE_LIBS tst_captureacknowledgement LINK_LIBRARIES)
target_link_libraries(tst_controllerclip_e2e PRIVATE ${CONTROLLER_CLIP_INPUT_LIBS} ${CONTROLLER_CLIP_CAPTURE_LIBS})
target_include_directories(tst_controllerclip_e2e PRIVATE "${CMAKE_SOURCE_DIR}/third_party/gameinput/include")
target_compile_definitions(tst_controllerclip_e2e PRIVATE UNICODE _UNICODE)
set_target_properties(tst_controllerclip_e2e PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/controllerclip")
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/tests/controllerclip/portable.flag" CONTENT "")

# cpo-p05: preset switching at safe input boundaries. Same synthetic backends and
# the same real InputEngine/BindingRuntime the controller clip E2E drives.
get_target_property(PRESET_SWITCH_SOURCES tst_inputengineshutdown SOURCES)
list(REMOVE_ITEM PRESET_SWITCH_SOURCES tst_inputengineshutdown.cpp)
gamehq_add_test(tst_presetswitch tst_presetswitch.cpp ${PRESET_SWITCH_SOURCES})
get_target_property(PRESET_SWITCH_LIBS tst_inputengineshutdown LINK_LIBRARIES)
target_link_libraries(tst_presetswitch PRIVATE ${PRESET_SWITCH_LIBS})
target_include_directories(tst_presetswitch PRIVATE "${CMAKE_SOURCE_DIR}/third_party/gameinput/include")
target_compile_definitions(tst_presetswitch PRIVATE UNICODE _UNICODE)

# p3-4: real Media Foundation decode/export plus worker handoff and gallery identity.
get_target_property(GAMEHQ_CAPTURE_TEST_SOURCES tst_captureacknowledgement SOURCES)
list(REMOVE_ITEM GAMEHQ_CAPTURE_TEST_SOURCES tst_captureacknowledgement.cpp)
gamehq_add_test(tst_replaythumbnail tst_replaythumbnail.cpp ${GAMEHQ_CAPTURE_TEST_SOURCES})
get_target_property(GAMEHQ_CAPTURE_TEST_LIBRARIES tst_captureacknowledgement LINK_LIBRARIES)
target_link_libraries(tst_replaythumbnail PRIVATE ${GAMEHQ_CAPTURE_TEST_LIBRARIES})
target_compile_definitions(tst_replaythumbnail PRIVATE UNICODE _UNICODE
    REPLAY_THUMB_FIXTURE="${CMAKE_SOURCE_DIR}/tests/fixtures/replay-thumbnail.mp4")
set_target_properties(tst_replaythumbnail PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/replaythumbnail")
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/tests/replaythumbnail/portable.flag" CONTENT "")

gamehq_add_test(tst_notificationcenter tst_notificationcenter.cpp
    "${CMAKE_SOURCE_DIR}/src/notify/NotificationCenter.cpp"
    "${CMAKE_SOURCE_DIR}/src/notify/ToastModel.cpp")
target_link_libraries(tst_notificationcenter PRIVATE Qt6::Quick user32)
target_compile_definitions(tst_notificationcenter PRIVATE
    GAMEHQ_APP_SOURCE="${CMAKE_SOURCE_DIR}/src/app/App.cpp")

# cpo-o05: native overlay acceptance. The overlay window, manager and presenter
# are the shipped sources (OverlayManager, OverlayPresenter, OverlayLifetimePolicy,
# ForegroundApi) and the QML is embedded from src/ui/qml unchanged; the other
# process in the test is a borderless Win32 fixture window driven through a
# registered message (tests/overlay_target_fixture.cpp).
add_executable(overlay_target_fixture overlay_target_fixture.cpp)
target_link_options(overlay_target_fixture PRIVATE -static -municode)
target_link_libraries(overlay_target_fixture PRIVATE user32)
set_target_properties(overlay_target_fixture PROPERTIES WIN32_EXECUTABLE TRUE)

gamehq_add_test(tst_overlaynative
    tst_overlaynative.cpp
    "${CMAKE_SOURCE_DIR}/src/overlay/ForegroundApi.cpp"
    "${CMAKE_SOURCE_DIR}/src/overlay/OverlayLifetimePolicy.cpp"
    "${CMAKE_SOURCE_DIR}/src/overlay/OverlayManager.cpp"
    "${CMAKE_SOURCE_DIR}/src/overlay/OverlayPresenter.cpp")
target_link_libraries(tst_overlaynative PRIVATE Qt6::Gui Qt6::Quick Qt6::Qml Qt6::Multimedia user32)
target_compile_definitions(tst_overlaynative PRIVATE
    GAMEHQ_TARGET_FIXTURE="$<TARGET_FILE:overlay_target_fixture>")
add_dependencies(tst_overlaynative overlay_target_fixture)
# Ten native window scenarios, each of which builds a QML engine, spawns or
# drives a second process and waits on real foreground transitions.
set_tests_properties(tst_overlaynative PROPERTIES TIMEOUT 600)

# The harness must drive the shipped overlay, not a copy of it: the real QML
# files are embedded under the same module URI the app uses ("GameHQ") through
# an explicit resource tree, with the qmldir listing the module's singletons.
file(GENERATE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/overlay_qml_module/qmldir"
    CONTENT "module GameHQ\nsingleton Theme 1.0 Theme.qml\nsingleton Brand 1.0 Brand.qml\nOverlayWindow 1.0 OverlayWindow.qml\n")
qt_add_resources(tst_overlaynative "overlay_qml_module"
    PREFIX "/qt/qml/GameHQ"
    BASE "${CMAKE_CURRENT_BINARY_DIR}/overlay_qml_module"
    FILES "${CMAKE_CURRENT_BINARY_DIR}/overlay_qml_module/qmldir")
qt_add_resources(tst_overlaynative "overlay_qml"
    PREFIX "/qt/qml/GameHQ"
    BASE "${CMAKE_SOURCE_DIR}/src/ui/qml"
    FILES
        "${CMAKE_SOURCE_DIR}/src/ui/qml/OverlayWindow.qml"
        "${CMAKE_SOURCE_DIR}/src/ui/qml/Theme.qml"
        "${CMAKE_SOURCE_DIR}/src/ui/qml/Brand.qml"
        "${CMAKE_SOURCE_DIR}/src/ui/qml/components/ScaledSurface.qml"
        "${CMAKE_SOURCE_DIR}/src/ui/qml/components/SidebarItem.qml"
        "${CMAKE_SOURCE_DIR}/src/ui/qml/components/OverlaySidebar.qml"
        "${CMAKE_SOURCE_DIR}/src/ui/qml/components/CaptureTile.qml"
        "${CMAKE_SOURCE_DIR}/src/ui/qml/components/VideoBadge.qml"
        "${CMAKE_SOURCE_DIR}/src/ui/qml/components/TileIconButton.qml"
        "${CMAKE_SOURCE_DIR}/src/ui/qml/components/OverlayCaptureStrip.qml"
        "${CMAKE_SOURCE_DIR}/src/ui/qml/components/AppScrollBar.qml"
        "${CMAKE_SOURCE_DIR}/src/ui/qml/components/SettingsPageHeader.qml"
        "${CMAKE_SOURCE_DIR}/src/ui/qml/components/SettingsPage.qml"
        "${CMAKE_SOURCE_DIR}/src/ui/qml/components/AccentButton.qml"
        "${CMAKE_SOURCE_DIR}/src/ui/qml/components/Lightbox.qml"
        "${CMAKE_SOURCE_DIR}/src/ui/qml/components/MediaStage.qml"
        "${CMAKE_SOURCE_DIR}/src/ui/qml/components/PlayerControls.qml"
        "${CMAKE_SOURCE_DIR}/src/ui/qml/components/OverlayPreview.qml"
        "${CMAKE_SOURCE_DIR}/src/ui/qml/components/ConfirmDialog.qml"
        "${CMAKE_SOURCE_DIR}/src/ui/qml/components/OverlayFooter.qml"
        "${CMAKE_SOURCE_DIR}/src/ui/qml/components/OverlayActionMenu.qml"
        "${CMAKE_SOURCE_DIR}/src/ui/qml/helpers/SidebarCategories.js"
        "${CMAKE_SOURCE_DIR}/src/ui/qml/helpers/PadNav.js"
        "${CMAKE_SOURCE_DIR}/src/ui/qml/themes/Skin.qml"
        "${CMAKE_SOURCE_DIR}/src/ui/qml/themes/DarkSkin.qml"
        "${CMAKE_SOURCE_DIR}/src/ui/qml/themes/LightSkin.qml"
        "${CMAKE_SOURCE_DIR}/src/ui/qml/themes/HighContrastSkin.qml"
        "${CMAKE_SOURCE_DIR}/src/ui/qml/themes/MidnightSkin.qml"
        "${CMAKE_SOURCE_DIR}/src/ui/qml/themes/EmeraldSkin.qml"
        "${CMAKE_SOURCE_DIR}/src/ui/qml/themes/HarborSkin.qml"
        "${CMAKE_SOURCE_DIR}/src/ui/qml/themes/CarbonSkin.qml"
        "${CMAKE_SOURCE_DIR}/src/ui/qml/themes/CobaltSkin.qml"
        "${CMAKE_SOURCE_DIR}/src/ui/qml/themes/SynthwaveSkin.qml"
        "${CMAKE_SOURCE_DIR}/src/ui/qml/themes/NordSkin.qml"
        "${CMAKE_SOURCE_DIR}/src/ui/qml/themes/DraculaSkin.qml"
        "${CMAKE_SOURCE_DIR}/src/ui/qml/themes/GruvboxSkin.qml"
        "${CMAKE_SOURCE_DIR}/src/ui/qml/themes/ObsidianSkin.qml")
