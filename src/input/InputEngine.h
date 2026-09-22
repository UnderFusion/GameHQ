#pragma once
#include "capture/CaptureRequest.h"
#include "input/ActionCatalog.h"
#include "input/BindingResolver.h"
#include "input/ControlId.h"
#include "input/MappingAssignmentResolver.h"
#include "input/ProviderIntegration.h"
#include <QElapsedTimer>
#include <QHash>
#include <QSet>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <functional>
#include <memory>
#include <vector>

#include "gameinput/NeutralHandoff.h"
#include "input/HeldControlTracker.h"

class ConfigManager;
class CaptureDatabase;
class Gamepad;
class DualSenseDevice;
class XInputDevice;
class WinMMDevice;
class BindingRuntime;
class MappingPresetMaterializer;
class BindingEditorModel;
class MappingPresetModel;
class HotkeyManager;
class MouseHookDevice;
class QTimer;
namespace ModernInput { class GameInputRouter; class GameInputFocusController; }

// Owns the controller backends (Sony Raw Input, XInput, WinMM) + Share
// tap/hold detector and maps buttons onto GameHQ actions
// (docs/controller-input.md). Global triggers (Share tap/hold, PS toggle)
// fire regardless of overlay state; when the overlay is visible the face
// buttons / d-pad drive its navigation instead of leaking out as actions.
//
// Exactly ONE backend is active at a time. Sony > XInput > WinMM provides the
// initial/fallback choice, then real button activity may move the active role
// while a short duplicate window suppresses mirrored API reports. Switching
// or losing the backend cancels any in-flight gesture (nav repeat, Share
// tap/hold), so nothing sticks across a transition. The Raw Input backend's
// device-topology hint triggers XInput/WinMM rescans, so pads appearing in
// those APIs are picked up event-driven instead of by hot polling.
// Exposed to QML as "input" for the Settings input-test screen.
class InputEngine : public QObject, public ModernInput::NeutralHandoffSource
{
    Q_OBJECT
    Q_PROPERTY(QString lastInput READ lastInput NOTIFY lastInputChanged)
    Q_PROPERTY(QString controllerStatus READ controllerStatus NOTIFY controllerStatusChanged)
    // Non-empty when a supported pad is connected to Windows but cloaked from
    // applications by a HID filter driver (HidHide). controllerFixAvailable
    // gates the one-click self-whitelist remedy (UAC prompt).
    Q_PROPERTY(QString controllerWarning READ controllerWarning NOTIFY controllerWarningChanged)
    Q_PROPERTY(bool controllerFixAvailable READ controllerFixAvailable NOTIFY controllerWarningChanged)
    Q_PROPERTY(QObject* bindingEditor READ bindingEditor CONSTANT)
    // The mapping-preset library + assignment facade (cpo-p06): create, select,
    // rename, delete/reassign, follow-fallback vs Built-in defaults, and the
    // explicit "duplicate for this controller" copy-on-write path.
    Q_PROPERTY(QObject* mappingPresets READ mappingPresets CONSTANT)
    // Diagnostics button probe (Settings → Input): startButtonProbe() opens a
    // 3 s window in which raw button changes — including ones GameHQ normally
    // ignores — are summarized into probeStatus and the diagnostics export.
    Q_PROPERTY(QString probeStatus READ probeStatus NOTIFY probeStatusChanged)
    Q_PROPERTY(bool probeRunning READ probeRunning NOTIFY probeStatusChanged)
    Q_PROPERTY(QString modernControllerStatus READ modernControllerStatus NOTIFY modernControllerChanged)
    Q_PROPERTY(QString modernControllerSummary READ modernControllerSummary NOTIFY modernControllerChanged)
    Q_PROPERTY(bool modernLayoutWarning READ modernLayoutWarning NOTIFY modernControllerChanged)
    Q_PROPERTY(QVariantList modernLayoutWarnings READ modernLayoutWarnings NOTIFY modernControllerChanged)
public:
    InputEngine(ConfigManager* config, CaptureDatabase* db, HotkeyManager* hotkeys,
                QObject* parent = nullptr);
    ~InputEngine() override;

    void start();   // seed default bindings + begin listening

    QString lastInput() const { return m_lastInput; }
    QString controllerStatus() const { return m_controllerStatus; }
    QString controllerWarning() const { return m_controllerWarning; }
    bool controllerFixAvailable() const { return m_controllerFixAvailable; }
    QObject* bindingEditor() const;
    QObject* mappingPresets() const;
    // C++ face of the same object, so the app can hand the preset model the
    // session game (cpo-p07) without going through QML.
    MappingPresetModel* mappingPresetsModel() const;

    // One-click HidHide remedy: relaunches GameHQ elevated to add itself to
    // HidHide's application allow-list, then rescans. Progress/outcome is
    // reported through controllerWarning.
    Q_INVOKABLE void fixHiddenController();

    QString probeStatus() const { return m_probeStatus; }
    bool probeRunning() const { return m_probeRunning; }
    Q_INVOKABLE void startButtonProbe();
    QString modernControllerStatus() const;
    QString modernControllerSummary() const;
    bool modernLayoutWarning() const;
    QVariantList modernLayoutWarnings() const;
    Q_INVOKABLE void copyControllerCompatibilityReport() const;
    // "Confirm current layout" in Settings → Input: accepts the observed
    // extra-button layout of every controller carrying a layout warning, so
    // extra buttons resume routing after a firmware/mode change. The user
    // reviews the detected buttons via the 3 s probe / summary first.
    Q_INVOKABLE void confirmModernControllerLayout(const QString& logicalId);

public slots:
    void retranslate();
    void setOverlayVisible(bool visible);
    // cpo-o06c: the one owner of the process-wide GameInput focus policy. The
    // app owns the controller (it outlives both the input stack and the overlay)
    // and installs it here before start(); the engine forwards it to the router,
    // which hands it to the runtime session. Never allows a second owner: the
    // pointer is only forwarded.
    void setGameInputFocusController(ModernInput::GameInputFocusController* controller);

    // ---------------------------------------------------------------- cpo-o06e
    // The release handoff's source (ModernInput::NeutralHandoffSource). What is
    // held right now comes from the raw edges this engine receives, before any
    // routing; setOverlayReleaseActive() stops overlay input from producing
    // further actions while a close waits for neutral, without ever releasing a
    // control synthetically. The overlay only asks.
    ModernInput::NeutralHandoffSample sampleNeutralPadState() const override;
    void setOverlayReleaseActive(bool active) override;
    // Desktop gallery window's OS focus state (Main.qml binds this to
    // window.active). Pad navigation only reaches the desktop window while
    // it's genuinely the foreground window — same "never steal the pad from
    // a game" rule the overlay already follows.
    void setDesktopFocused(bool focused);
    void setPlaybackActive(bool active);
    void reloadBindings();
    Q_INVOKABLE bool handleKeyPressed(int key, int modifiers, bool autoRepeat = false);
    Q_INVOKABLE bool handleKeyReleased(int key, int modifiers);

    // ---------------------------------------------------------------- cpo-p05
    // The running game's executable path or key; empty means no game. The key
    // is canonicalized with MappingAssignmentResolver::canonicalGameKey(), the
    // same normalization storage writes rows with. A change re-resolves the
    // mapping winners at a safe input boundary; an unchanged key is a no-op.
    //
    // cpo-p05 owns this seam only: cpo-p07 connects the real App /
    // CurrentGameService game session to it.
    void setRunningGameKey(const QString& executablePathOrKey);
    QString runningGameKey() const { return m_runningGameKey; }

    // Re-resolve the preset winner for every mapping route this engine tracks
    // (keyboard, mouse, the active controller route and any route a preset was
    // installed for) and apply the change - if any - at a safe input boundary:
    // prepare, compare, snapshot, invalidate the old generation, publish the
    // new tables, then release-gate the controls that were down. Also the seam
    // assignment/content edits (cpo-p06) call after a write.
    void refreshResolvedPreset();

    // Register a controller mapping route the first time this engine observes
    // it, planning and publishing its winner BEFORE the press that revealed it
    // is dispatched. One shared seam: the legacy pad path, GameInput and
    // selective Raw-HID all call it, so no backend can execute an unplanned
    // table. Cheap after the first registration (an already tracked route does
    // no database work), which is what keeps the per-event path free of
    // assignment and preset resolution. Returns true when a refresh ran.
    bool ensureMappingRoute(const QString& logicalProfile);

    // Read-only resolution state for diagnostics and later leaves.
    QString mappingEffectiveSource(const QString& deviceGroup,
                                   const QString& deviceProfile = {}) const;
    QString mappingInstalledPresetId(const QString& deviceGroup,
                                     const QString& deviceProfile = {}) const;
    // "group\x1fprofile\x1fcontrol" entries currently gated until release.
    QStringList mappingArmedReleaseGates() const;

signals:
    // Global actions (0.4/0.5 wire these to real capture; for now sound + log).
    // Both carry the request that started them, so the log chain from press to
    // saved/failed shares one id (docs/capture-engine.md).
    void screenshotRequested(const CaptureRequest& request);
    void replayRequested(const CaptureRequest& request);
    void overlayToggleRequested();
    // Hold PS (2 s): summon the desktop window over the game with focus, or
    // dismiss it again. Consumed by App, which owns the window + foreground.
    void desktopWindowToggleRequested();
    // Circle: consumed entirely in QML now (OverlayWindow.qml) — pops the
    // action menu / sidebar focus first, only closes the overlay at the
    // root level. Kept named "HideRequested" since Esc still means "close".
    void overlayHideRequested();
    // Overlay navigation (consumed by OverlayWindow.qml while it is open).
    void overlayNavigate(int direction);           // D-pad left/right: -1/+1
    void overlayNavigateVertical(int direction);   // D-pad up/down: -1/+1
    void overlayConfirm();
    void overlayFavorite();
    void overlayMenu();             // Square: open/close the per-capture action menu
    void overlaySidebarToggle();    // Options: enter/exit the sidebar
    void overlayGameStep(int direction);   // L1/R1: quick game switch, -1/+1

    // Desktop gallery navigation (consumed by Main.qml while it is the
    // foreground window and the overlay is closed).
    void desktopNavigate(int direction);           // D-pad left/right: -1/+1
    void desktopNavigateVertical(int direction);   // D-pad up/down: -1/+1
    void desktopConfirm();          // Cross: open the lightbox
    void desktopBack();             // Circle: close menu, else close lightbox
    void desktopFavorite();         // Triangle
    void desktopMenu();             // Square: open/close the per-capture action menu
    void desktopTabStep(int direction);   // L1/R1: switch panel focus (sidebar ↔ grid), -1/+1
    void desktopSettings();               // Options: open Settings
    void desktopZoom(int direction);      // L2/R2: thumbnail size, -1/+1
    void desktopScroll(int direction);    // right stick: wheel-like scroll, -1/+1
    void desktopBulkToggle();             // Cross held: enter/leave bulk selection
    void playbackPlayPause();
    void playbackSeek(int direction);
    // Share while a clip is focused: grab the frame on screen and save it as a
    // screenshot. Consumed in QML (OverlayWindow.qml / Main.qml) because the
    // live video frame lives in the QML video surface, not in C++.
    void frameGrabRequested();

    void lastInputChanged();
    void controllerStatusChanged();
    void controllerWarningChanged();
    void probeStatusChanged();
    void modernControllerChanged();

private:
    friend class InputEngineShutdownTest;
    friend class InputReleaseHandoffTest;
    friend class ControllerClipE2ETest;
    friend class PresetSwitchTest;
    friend class GameSessionPresetTest;
    void shutdown();
    void migrateLegacyHoldSetting();
    void applyGestureTiming();
    // Lazy mouse monitoring: the global WH_MOUSE_LL hook is installed only
    // while a mouse binding or an active mouse capture can consume it (see
    // MouseMonitorPolicy). Synced on start, on every binding reload and on
    // every binding-editor state change.
    bool needsMouseMonitoring() const;
    void syncMouseMonitoring();
    QString m_lastTimingDescription;

    void onControlPressed(const QString& controlId, int family,
                          const QString& backend, const QString& fingerprint,
                          const QString& displayName);
    void onControlReleased(const QString& controlId, int family,
                           const QString& backend, const QString& fingerprint,
                           const QString& displayName);
    void attachGamepad(std::unique_ptr<Gamepad> pad, const QString& displayName);
    // t25 provider-integration bookkeeping: which shared-registry provider a
    // legacy backend reports as, and (re-)observing its attachment when it
    // connects, disconnects, or changes fingerprint.
    ModernInput::ControllerProvider providerFor(const Gamepad* pad) const;
    void observeLegacyBackend(Gamepad* pad, const ControlId::DeviceProfile& profile);
    void removeLegacyBackend(Gamepad* pad, const QString& providerDeviceId = {});
    QString canonicalProfile(Gamepad* pad, const QString& providerDeviceId) const;
    void configureLogicalProfile(const QString& logicalId,
                                 const QStringList& migrationAliases = {});
    // Routes a legacy Capture/Guide edge through the shared capability router
    // so the same physical press arriving via GameInput cannot double-fire.
    // Returns false when the edge is a cross-provider duplicate.
    bool routeLegacySystemEdge(Gamepad* source, const QString& providerDeviceId,
                               const QString& controlId, bool pressed);
    bool hasExplicitViewBinding(const QString& logicalProfile) const;
    void updateActiveBackend();
    void activateBackend(Gamepad* pad, const QString& reason);
    // Pending-candidate confirmation: true once `source` has carried input on
    // its own long enough to take the active role before the silence threshold.
    bool confirmCandidate(Gamepad* source, qint64 now, qint64 activeLastControlMs);
    // Hold / drop / deliver the first press of an unconfirmed candidate.
    void holdCandidatePress(Gamepad* source, const QString& controlId, int family,
                            const QString& fingerprint, qint64 pressedMs);
    void clearPendingCandidate();
    void resolvePendingCandidate(int generation);
    struct PendingPress;
    void replayPendingPress(const PendingPress& press);
    void deliverPress(Gamepad* source, const QString& controlId, int family,
                      const QString& fingerprint);
    bool backendConnected(const Gamepad* pad) const;
    QString backendDisplayName(const Gamepad* pad) const;
    int backendPriority(const Gamepad* pad) const;   // Sony 3 > XInput 2 > WinMM 1
    // Correlates the XInput pad with the IG_ devices Raw Input sees and, when
    // unambiguous, keys its bindings on real hardware identity instead of
    // the slot number (see ControllerIdentity).
    void updateXInputIdentity();
    void setLastInput(const QString& text);
    void setControllerStatus(const QString& text);
    bool desktopCanReceiveInput() const;
    bool anyBackendConnected() const;
    ActionCatalog::Scope primaryScope() const;
    ActionCatalog::Scope fallbackScope() const;
    void dispatchAction(const QString& actionId, const QString& triggerCode = {},
                        const QString& deviceGroup = {}, const QString& deviceProfile = {});

    // Dispatch table handlers — one per entry in the static table inside
    // InputEngine.cpp. Each is a thin wrapper around the signal emission +
    // nav-repeat booking that the old if/else chain performed inline.
    using Self = InputEngine;
    // The device group of the action currently being dispatched. Valid only
    // inside dispatchAction().
    CaptureRequest::Source m_dispatchSource = CaptureRequest::Source::Unknown;
    // The mapping route of the action currently being dispatched, for the same
    // reason and under the same contract: a handler that starts something
    // lasting (the navigation repeat) has to record which route's table it
    // belongs to. Keyboard/mouse have no profile layer, so it is empty there.
    QString m_dispatchGroup;
    QString m_dispatchProfile;
    void handleScreenshot(const QString&)
    {
        emit screenshotRequested(CaptureRequest::create(m_dispatchSource));
    }
    void handleSaveReplay(const QString&)
    {
        emit replayRequested(CaptureRequest::create(m_dispatchSource));
    }
    void handleToggleOverlay(const QString&)          { emit overlayToggleRequested(); }
    void handleToggleDesktop(const QString&)          { emit desktopWindowToggleRequested(); }
    void handleOverlayNavigateLeft(const QString& tc)  { startNavRepeat(tc, -1, [this](int d) { emit overlayNavigate(d); }); }
    void handleOverlayNavigateRight(const QString& tc) { startNavRepeat(tc,  1, [this](int d) { emit overlayNavigate(d); }); }
    void handleOverlayNavigateUp(const QString& tc)    { startNavRepeat(tc, -1, [this](int d) { emit overlayNavigateVertical(d); }); }
    void handleOverlayNavigateDown(const QString& tc)  { startNavRepeat(tc,  1, [this](int d) { emit overlayNavigateVertical(d); }); }
    void handleOverlayConfirm(const QString&)          { emit overlayConfirm(); }
    void handleOverlayBack(const QString&)             { emit overlayHideRequested(); }
    void handleOverlayFavorite(const QString&)         { emit overlayFavorite(); }
    void handleOverlayMenu(const QString&)             { emit overlayMenu(); }
    void handleOverlaySidebarToggle(const QString&)    { emit overlaySidebarToggle(); }
    void handleOverlayGamePrev(const QString& tc)      { startNavRepeat(tc, -1, [this](int d) { emit overlayGameStep(d); }); }
    void handleOverlayGameNext(const QString& tc)      { startNavRepeat(tc,  1, [this](int d) { emit overlayGameStep(d); }); }
    void handleDesktopNavigateLeft(const QString& tc)  { startNavRepeat(tc, -1, [this](int d) { emit desktopNavigate(d); }); }
    void handleDesktopNavigateRight(const QString& tc) { startNavRepeat(tc,  1, [this](int d) { emit desktopNavigate(d); }); }
    void handleDesktopNavigateUp(const QString& tc)    { startNavRepeat(tc, -1, [this](int d) { emit desktopNavigateVertical(d); }); }
    void handleDesktopNavigateDown(const QString& tc)  { startNavRepeat(tc,  1, [this](int d) { emit desktopNavigateVertical(d); }); }
    void handleDesktopConfirm(const QString&)          { emit desktopConfirm(); }
    void handleDesktopBack(const QString&)             { emit desktopBack(); }
    void handleDesktopFavorite(const QString&)         { emit desktopFavorite(); }
    void handleDesktopMenu(const QString&)             { emit desktopMenu(); }
    void handleDesktopTabPrev(const QString&)          { emit desktopTabStep(-1); }
    void handleDesktopTabNext(const QString&)          { emit desktopTabStep(1); }
    void handleDesktopSettings(const QString&)         { emit desktopSettings(); }
    // Zoom repeats while the trigger is held, like the nav buttons — holding
    // R2 should sweep the thumbnails up, not step once per pull.
    void handleDesktopZoomOut(const QString& tc)       { startNavRepeat(tc, -1, [this](int d) { emit desktopZoom(d); }); }
    void handleDesktopZoomIn(const QString& tc)        { startNavRepeat(tc,  1, [this](int d) { emit desktopZoom(d); }); }
    void handleDesktopScrollUp(const QString& tc)      { startNavRepeat(tc, -1, [this](int d) { emit desktopScroll(d); }); }
    void handleDesktopScrollDown(const QString& tc)    { startNavRepeat(tc,  1, [this](int d) { emit desktopScroll(d); }); }
    void handleDesktopBulkToggle(const QString&)       { emit desktopBulkToggle(); }
    void handlePlaybackPlayPause(const QString&)       { emit playbackPlayPause(); }
    void handlePlaybackSeekBack(const QString& tc)     { startNavRepeat(tc, -1, [this](int d) { emit playbackSeek(d); }); }
    void handlePlaybackSeekForward(const QString& tc)  { startNavRepeat(tc,  1, [this](int d) { emit playbackSeek(d); }); }
    void handleFrameGrab(const QString&)               { emit frameGrabRequested(); }

    // Hold-to-repeat for navigation buttons (D-pad ↑↓←→, L1, R1). The pad
    // only delivers press edges to QML, so the repeat lives here: the signal
    // fires once on press, then — if the button stays held — again 220 ms
    // later (no initial delay), then accelerating until release. Same UX as
    // keyboard auto-repeat in QML (see components/NavRepeat.qml).
    //   startInterval: 220 ms between the first auto-repeats (also the
    //                  tap-release guard — release before this fires once)
    //   acceleration: each tick multiplies the interval by this (0.77 = 0.88²,
    //                 2x the shrink-per-tick of the original 0.88)
    //   minInterval: floor at 70 ms (≈14 steps/sec)
    void startNavRepeat(const QString& triggerCode, int direction,
                        std::function<void(int)> emitter);
    void stopNavRepeat();

    ConfigManager* m_config;
    CaptureDatabase* m_db;
    HotkeyManager* m_hotkeys;
    std::unique_ptr<BindingRuntime> m_runtime;
    // cpo-p03: proves and materializes a controller's chain from the migration
    // presets once the live identity is durable; weak pads never reach storage.
    std::unique_ptr<MappingPresetMaterializer> m_presetMaterializer;
    std::unique_ptr<BindingEditorModel> m_bindingEditor;
    // cpo-p06: the preset library/assignment facade QML drives. It never
    // resolves a winner itself; its one runtime seam is refreshResolvedPreset().
    std::unique_ptr<MappingPresetModel> m_mappingPresets;
    std::unique_ptr<MouseHookDevice> m_mouse;
    // Shared t25 integration: one PhysicalControllerRegistry and one
    // CapabilityEventRouter for every provider (Sony Raw, GameInput, XInput,
    // WinMM, selective Raw HID). Declared before m_gameInput, which holds a
    // pointer into it.
    ModernInput::ProviderIntegration m_providers;
    QHash<Gamepad*, QSet<QString>> m_legacyObservedIds;
    QHash<QString, QStringList> m_profileMigrationAliases;
    QSet<QString> m_legacyViewFallbackHeld;
    // cpo-o06e: which controls are held right now, fed from the raw edges before
    // any routing; the release handoff's whole evidence base.
    HeldControlTracker m_held;
    // True for the duration of a close's release handoff: presses stop
    // producing actions while the wait observes the pad (releases keep flowing,
    // they only ever close things).
    bool m_overlayReleaseActive = false;
    std::unique_ptr<ModernInput::GameInputRouter> m_gameInput;
    // cpo-o06c: non-owning. The controller is owned by App so that it outlives
    // both the input stack and the overlay that requests transitions from it.
    ModernInput::GameInputFocusController* m_gameInputFocus = nullptr;
    std::vector<std::unique_ptr<Gamepad>> m_pads;
    DualSenseDevice* m_sonyPad = nullptr;
    XInputDevice* m_xinputPad = nullptr;
    WinMMDevice* m_winmmPad = nullptr;
    Gamepad* m_activeBackend = nullptr;   // the one backend whose events route
    QElapsedTimer m_controllerClock;
    QHash<Gamepad*, qint64> m_backendLastControlMs;
    QHash<Gamepad*, QHash<QString, qint64>> m_backendLastReleaseMs;
    // First event of each non-active backend's current pending run; cleared on
    // takeover and on disconnect, restarted whenever the active backend speaks.
    QHash<Gamepad*, qint64> m_backendCandidateFirstMs;
    // The one press held while a candidate backend is being confirmed. Empty
    // source = nothing held. `released` records that the user already let go,
    // so the replay stays a tap.
    struct PendingPress {
        Gamepad* source = nullptr;
        QString controlId;
        QString fingerprint;
        int family = 0;
        qint64 pressedMs = 0;
        bool released = false;
    };
    PendingPress m_pending;
    int m_pendingGeneration = 0;   // cancels a superseded confirmation timer

    // ---------------------------------------------------------------- cpo-p05
    // What serves one mapping route right now (deviceGroup + canonical logical
    // profile): the installed preset table, or the resolver's own inherited
    // table with an honest source label. The fingerprint is what makes the
    // no-op check exact: same winner AND same content, or a real switch.
    struct MappingChainState {
        QString group;
        QString profile;
        QString source;
        // The switch layer serves an explicit table for this chain. Ownership
        // can NEVER be inferred from a non-empty preset id: `builtin` is an
        // owned, explicitly prepared shipped-defaults table with no preset.
        bool owned = false;
        QString presetId;     // empty when the serving table has no preset (builtin)
        QString fingerprint;
    };
    // One chain's next resolved state, prepared without touching live state.
    struct PlannedChain {
        QString group;
        QString profile;
        bool install = false;             // the switch layer serves an explicit table?
        QString presetId;                 // set only when the table is a named preset
        QString source;                   // effective source that will serve
        QVector<BindingResolver::Binding> table;
        QString fingerprint;
    };

    QString mappingChainKey(const QString& group, const QString& profile) const;
    // The typed controller identity chain for one route: the durable identity
    // first when the registry proves it, then the legacy provider/alias keys.
    // A weak or session-local identity is only ever presented as legacy_slot.
    QVector<MappingAssignmentResolver::IdentityCandidate> controllerIdentityChain(
        const QString& profile) const;
    PlannedChain planMappingChain(const QString& group, const QString& profile) const;

    QHash<QString, MappingChainState> m_mappingChains;
    std::unique_ptr<MappingAssignmentResolver> m_assignmentResolver;
    QString m_runningGameKey;         // canonical; empty = no game
    QString m_lastControllerRoute;    // canonical logical profile of the last pad press
    bool m_mappingSwitchRunning = false;

    // ---------------------------------------------------------------- cpo-x01
    // Routing evidence for the one-click export: which provider serves the
    // logical controller, what a candidate run is holding, and what the
    // arbitration dropped. Counters are process-lifetime arithmetic; the push
    // itself happens only at routing transitions, and from the press path at
    // most once per kRoutingPushThrottleMs - never per event.
    void publishControllerRouting();
    void traceProviderLifecycle(const QString& event);
    void publishMappingDiagnostics();
    int m_mirrorWindowDrops = 0;       // cross-provider events dropped as duplicates
    int m_candidateRuns = 0;           // candidate presses held for confirmation
    qint64 m_lastRoutingPushMs = -1;   // throttle for pushes from the press path
    QString m_lastBackendSwitchReason;
    bool m_started = false;        // gates mouse monitoring until start()
    bool m_shuttingDown = false;
    bool m_sonyConnected = false;
    bool m_xinputConnected = false;
    bool m_winmmConnected = false;
    bool m_overlayVisible = false;
    bool m_desktopFocused = false;
    bool m_playbackActive = false;
    QString m_lastInput;
    QString m_controllerStatus;
    QString m_controllerWarning;
    QString m_probeStatus;
    bool m_probeRunning = false;
    bool m_controllerFixAvailable = false;
    void setControllerWarning(const QString& text, bool fixAvailable);
    QTimer* m_fixWatch = nullptr;        // polls the elevated helper process
    void* m_fixProcess = nullptr;        // HANDLE of the elevated helper (or null)

    QTimer* m_repeatTick = nullptr;      // accelerating repeat timer
    QString m_repeatTrigger;             // canonical control currently held
    // The mapping route the running repeat started from (device group +
    // canonical logical profile). A cpo-p05 switch ends a repeat whose own
    // table was replaced and leaves a repeat belonging to any other route
    // ticking: the repeat is a press of one table, not of the runtime.
    QString m_repeatRouteGroup;
    QString m_repeatRouteProfile;
    int m_repeatDirection = 0;
    std::function<void(int)> m_repeatEmitter;
};
class Gamepad;
