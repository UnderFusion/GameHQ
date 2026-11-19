#include "input/InputEngine.h"
#include "input/SteamInputAdvice.h"

#include "config/ConfigKeys.h"
#include "config/ConfigManager.h"
#include "input/BindingRuntime.h"
#include "input/ControllerArbitration.h"
#include "input/ControllerIdentity.h"
#include "input/BindingEditorModel.h"
#include "input/BindingPattern.h"
#include "input/DualSenseDevice.h"
#include "input/GestureTiming.h"
#include "input/Gamepad.h"
#include "input/HotkeyManager.h"
#include "input/EventLoopStallMonitor.h"
#include "input/InputDiagnostics.h"
#include "input/MappingPresetMaterializer.h"
#include "input/MappingPresetModel.h"
#include "input/MouseHookDevice.h"
#include "input/MouseMonitorPolicy.h"
#include "input/OverlayInputPolicy.h"
#include "input/WinMMDevice.h"
#include "localization/NativeText.h"
#include "input/XInputDevice.h"
#include "input/SelectiveRawHidFallback.h"
#include "input/RawHidBindingCatalog.h"
#include "gameinput/GameInputFocusController.h"
#include "gameinput/GameInputRouter.h"
#include "gameinput/ProductionGameInputApi.h"

namespace {
// cpo-o06e: engine-internal device identities for the held-control tracker. The
// values are never printed — the receipts name canonical control ids only — they
// exist so a legacy backend and a GameInput logical controller can be told apart
// when the device that owns a held control goes away.
QString legacyDeviceKey(const Gamepad* pad)
{
    return QStringLiteral("pad:0x%1").arg(reinterpret_cast<quintptr>(pad), 0, 16);
}

QString gameInputDeviceKey(const QString& logicalId)
{
    return QStringLiteral("gameinput:") + logicalId;
}
}  // namespace
#include "storage/CaptureDatabase.h"

#include <QDebug>
#include <QClipboard>
#include <QGuiApplication>
#include <QKeySequence>
#include <QSet>
#include <QTimer>
#include <QVariantMap>

#include <windows.h>

namespace {
QString keyboardTrigger(int key, int modifiers)
{
    if (key == Qt::Key_Enter)
        return QStringLiteral("Enter");
    return QKeySequence(key | modifiers).toString(QKeySequence::PortableText);
}
}

InputEngine::InputEngine(ConfigManager* config, CaptureDatabase* db,
                         HotkeyManager* hotkeys, QObject* parent)
    : QObject(parent)
    , m_config(config)
    , m_db(db)
    , m_hotkeys(hotkeys)
    , m_runtime(std::make_unique<BindingRuntime>(db))
    , m_bindingEditor(std::make_unique<BindingEditorModel>(
          db, m_runtime.get(), [this] { reloadBindings(); }))
    , m_mouse(std::make_unique<MouseHookDevice>())
    , m_lastInput(QStringLiteral("Connect a controller and press a button..."))
    , m_controllerStatus(QStringLiteral("No controller detected"))
{
    m_presetMaterializer = std::make_unique<MappingPresetMaterializer>(db, &m_runtime->resolver());
    // cpo-p05 winner resolution: decision only, driven by the game key and the
    // live controller identity chain. Applying a result is a switch boundary
    // (refreshResolvedPreset), never a per-event decision.
    m_assignmentResolver = std::make_unique<MappingAssignmentResolver>(db);

    // cpo-p06: the preset library/assignment facade QML drives. It writes
    // storage and then asks THIS engine to re-resolve at a safe boundary; it
    // never resolves a winner itself, and library-only operations (create,
    // rename, an unused duplicate) never touch the runtime.
    m_mappingPresets = std::make_unique<MappingPresetModel>(db);
    m_mappingPresets->setRuntimeRefresh([this] { refreshResolvedPreset(); });
    m_mappingPresets->setEffectiveTableProvider(
        [this](const QString& group, const QString& profile) {
            return m_runtime->effectiveBindings(group, profile);
        });
    m_mappingPresets->setPinnedProfileProvider(
        [this] { return m_bindingEditor->pinnedProfile(); });
    m_mappingPresets->setPendingEditProvider([this] { return m_bindingEditor->pendingEdit(); });
    m_mappingPresets->setTargetProvider([this](const QString& group, const QString& pinned) {
        // Provenance, never key syntax (cpo-p04/p05 reused): durable identity ->
        // `controller`; a weak one may only persist through a real slot alias.
        MappingPresetModel::Provenance provenance;
        const auto* logical =
            pinned.isEmpty() ? nullptr : m_providers.registry().controller(pinned);
        provenance.known = logical != nullptr;
        if (logical) {
            provenance.durable = logical->confidence == ModernInput::IdentityConfidence::Strong;
            provenance.displayName = logical->displayName;
            for (const auto& attachment : logical->providers) {
                if (!attachment.providerDeviceId.isEmpty())
                    provenance.slotAliases.append(attachment.providerDeviceId);
            }
            provenance.slotAliases.append(m_runtime->resolver().aliasesFor(pinned));
        }
        return MappingPresetModel::targetFromProvenance(group, pinned, provenance);
    });
    // cpo-p06 phase C: the editor's write path lands in a named preset from
    // here on. The editor keeps its draft/conflict/hotkey logic; the model owns
    // which preset a target edit belongs to (adopt-and-mask included).
    m_bindingEditor->setPresetSink(
        [this](const QVector<BindingEditorModel::PresetEdit>& edits) {
            QVector<MappingPresetModel::ContentEdit> batch;
            batch.reserve(edits.size());
            for (const BindingEditorModel::PresetEdit& edit : edits) {
                MappingPresetModel::ContentEdit out;
                out.actionId = edit.actionId;
                out.slot = edit.slot;
                out.remove = edit.remove;
                out.row.actionId = edit.actionId;
                out.row.slot = edit.slot;
                out.row.triggerCode = edit.triggerCode;
                out.row.activation = edit.activation;
                out.row.holdMs = edit.holdMs;
                out.row.unbound = edit.unbound;
                out.row.tapCount = edit.tapCount;
                batch.append(out);
            }
            return m_mappingPresets->applyContentEdits(batch);
        });
    // The editor shows and checks the preset open in "Editing preset" - the one
    // its writes land in (editingPresetId() is the shared rule) - and follows
    // that selection as it changes.
    m_bindingEditor->setEditSourceProvider(
        [this](const QString& group, const QString& profile) {
            BindingEditorModel::EditSource source;
            if (group != m_mappingPresets->deviceGroup())
                return source;
            source.presetId = m_mappingPresets->editingPresetId();
            if (source.presetId.isEmpty())
                return source;
            source.rows = m_db->mappingPresetRows(source.presetId);
            source.live = m_runtime->resolver().presetTableId(group, profile) == source.presetId;
            return source;
        });
    connect(m_mappingPresets.get(), &MappingPresetModel::selectionChanged, this,
            [this] { m_bindingEditor->refreshRows(); });
    // OS half of the binding transaction. The editor calls this *before* it
    // writes anything, so a chord Windows refuses can never be persisted and
    // shown as a working shortcut.
    m_bindingEditor->setHotkeyApply(
        [this](const QString& actionId, int slot, const QString& chord, QString* reason) {
            if (chord.isEmpty()) {
                // Empty chord means "release the slot" — used by the rollback
                // path when the action had no previous keyboard binding.
                m_hotkeys->clearBindingSlot(actionId, slot);
                return true;
            }
            const auto parsed = HotkeyManager::parseChord(chord);
            if (!parsed.valid) {
                if (reason)
                    *reason = parsed.rejectionReason;
                return false;
            }
            if (m_hotkeys->applyBindingSlot(actionId, slot, parsed.modifiers, parsed.vk))
                return true;
            if (reason) {
                *reason = QStringLiteral("This shortcut is already used by Windows or "
                                         "another application.");
            }
            return false;
        });

    m_controllerClock.start();
    m_viewBackFallbackPolicies = m_config->value(ConfigKeys::InputViewBackCaptureFallback).toMap();
    connect(m_bindingEditor.get(), &BindingEditorModel::controllerProfileChanged,
            this, &InputEngine::viewBackFallbackChanged);
    connect(m_bindingEditor.get(), &BindingEditorModel::controllerSpecificChanged,
            this, &InputEngine::viewBackFallbackChanged);
    migrateLegacyHoldSetting();
    applyGestureTiming();
    connect(m_config, &ConfigManager::valueChanged, this,
            [this](const QString& key, const QVariant&) {
                if (key == ConfigKeys::InputViewBackCaptureFallback) {
                    applyViewBackFallbackPolicies();
                    return;
                }
                if (key == ConfigKeys::InputModernControllerSupport) {
                    const bool off = m_config->value(key).toString() == QLatin1String("off");
                    m_gameInput->setMode(off
                        ? ModernInput::GameInputRouter::SupportMode::Off
                        : ModernInput::GameInputRouter::SupportMode::Auto);
                    return;
                }
                if (key != ConfigKeys::InputDefaultHoldMs
                    && key != ConfigKeys::InputMultiTapIntervalMs
                    && key != ConfigKeys::InputChordWindowMs)
                    return;
                applyGestureTiming();
                reloadBindings();
            });
    // A group or all reset drops the overrides without naming a key.
    connect(m_config, &ConfigManager::groupReset, this, [this](const QString& prefix) {
        if (!prefix.isEmpty() && !QLatin1String("input.").startsWith(prefix)
            && !prefix.startsWith(QLatin1String("input")))
            return;
        applyViewBackFallbackPolicies();
        applyGestureTiming();
        reloadBindings();
    });

    connect(m_runtime.get(), &BindingRuntime::actionTriggered,
            this, &InputEngine::dispatchAction);
    if (m_hotkeys) {
        connect(m_hotkeys, &HotkeyManager::hotkeyTriggered, this,
                [this](const QString& actionId) {
                    dispatchAction(actionId, {}, QStringLiteral("keyboard"), {});
                });
    }
    connect(m_mouse.get(), &MouseHookDevice::buttonPressed, this,
            [this](const QString& code, int generation) {
                // A press queued from the hook thread can arrive after the
                // hook was stopped — or stopped AND restarted, which an
                // isRunning() check would wrongly accept. Anything from a
                // previous hook lifetime is dropped: its release was never
                // captured, so acting on it would arm a gesture no release
                // will ever end.
                if (m_shuttingDown || generation != m_mouse->generation())
                    return;
                QString label = code;
                if (code == MouseHookDevice::ButtonBack) label = QStringLiteral("Mouse Back");
                else if (code == MouseHookDevice::ButtonForward) label = QStringLiteral("Mouse Forward");
                else if (code == MouseHookDevice::ButtonMiddle) label = QStringLiteral("Middle Mouse");
                beginInput(label);
                if (m_bindingEditor->captureInput(QStringLiteral("mouse"), code, label))
                    return;
                m_runtime->press(QStringLiteral("mouse"), {}, code,
                                 primaryScope(), fallbackScope());
            });
    connect(m_mouse.get(), &MouseHookDevice::buttonReleased, this,
            [this](const QString& code, int) {
                if (m_shuttingDown)
                    return;
                // Releases process regardless of generation: releasing an
                // unpressed control is a safe no-op, dropping a real one
                // risks a stuck control.
                m_runtime->release(QStringLiteral("mouse"), {}, code);
                stopNavRepeatOnRelease(code);
            });
    // Capturing a first mouse binding must see the buttons before any mouse
    // binding exists, so the hook follows the editor's capture state too.
    connect(m_bindingEditor.get(), &BindingEditorModel::captureChanged,
            this, &InputEngine::syncMouseMonitoring);
    connect(m_bindingEditor.get(), &BindingEditorModel::editorChanged,
            this, &InputEngine::syncMouseMonitoring);
    connect(m_bindingEditor.get(), &BindingEditorModel::deviceGroupChanged,
            this, &InputEngine::syncMouseMonitoring);

    auto sonyPad = std::make_unique<DualSenseDevice>();
    m_sonyPad = sonyPad.get();
    attachGamepad(std::move(sonyPad), QStringLiteral("Sony controller"));

    auto xinputPad = std::make_unique<XInputDevice>();
    m_xinputPad = xinputPad.get();
    attachGamepad(std::move(xinputPad), QStringLiteral("XInput controller"));
    connect(m_xinputPad, &XInputDevice::slotConnectionChanged, this,
            [this](int slot, bool connected) {
                const auto profile = m_xinputPad->profileForSlot(slot);
                if (connected)
                    observeLegacyBackend(m_xinputPad, profile);
                else
                    removeLegacyBackend(m_xinputPad, profile.fingerprint);
            });

    auto winmmPad = std::make_unique<WinMMDevice>();
    m_winmmPad = winmmPad.get();
    attachGamepad(std::move(winmmPad), QStringLiteral("WinMM joystick"));

    const bool modernOff = m_config->value(ConfigKeys::InputModernControllerSupport).toString()
        == QLatin1String("off");
    m_gameInput = std::make_unique<ModernInput::GameInputRouter>(
        std::make_unique<ModernInput::ProductionGameInputApi>(),
        modernOff ? ModernInput::GameInputRouter::SupportMode::Off
                  : ModernInput::GameInputRouter::SupportMode::Auto,
        m_db);
    // One registry + one capability router across GameInput AND the legacy
    // backends: the physical Share/Guide dedup (t25) lives in m_providers.
    m_gameInput->setProviderIntegration(&m_providers);
    // Every provider executes under the canonical logical-controller profile.
    // Legacy fingerprints and slot names are compatibility aliases only.
    const auto gameInputProfile = [this](const QString& logicalId) {
        configureLogicalProfile(logicalId);
        return logicalId;
    };
    connect(m_gameInput.get(), &ModernInput::GameInputRouter::systemControlPressed,
            this, [this, gameInputProfile](const QString& control, const QString& logicalId,
                         const QString& displayName) {
                // cpo-o06e: the raw edge, before any capture or routing — what is
                // held right now is what the release handoff waits for.
                m_held.notePressed(gameInputDeviceKey(logicalId), control);
                if (m_overlayReleaseActive)
                    return;   // a close is waiting for neutral: no actions fire
                if (control == ControlId::Capture || control == ControlId::Guide)
                    m_systemControlOrigins.insert(logicalId + QChar(0x1f) + control,
                        {ModernInput::ControllerProvider::GameInput,
                         {ModernInput::ControllerProvider::GameInput}});
                InputDiagnostics::instance().noteControl(control, QStringLiteral("GameInput"));
                m_bindingEditor->noteObservedControl(control);
                const auto family = ControlId::ControllerFamily::Generic;
                const auto* logical = m_providers.registry().controller(logicalId);
                m_bindingEditor->setControllerProfile({
                    QStringLiteral("GameInput"), logicalId, family,
                    displayName, logical ? logical->modelFingerprint : QString()});
                beginInput((displayName.isEmpty() ? QStringLiteral("Controller") : displayName)
                           + QStringLiteral(": ") + ControlId::label(control, family));
                if (m_bindingEditor->captureInput(QStringLiteral("controller"), control,
                                                  ControlId::label(control, family)))
                    return;
                // cpo-p05: the same shared route seam the legacy path uses - an
                // assigned winner is planned and published before this first
                // press can act under an inherited table.
                const QString logicalProfile = gameInputProfile(logicalId);
                ensureMappingRoute(logicalProfile);
                m_runtime->press(QStringLiteral("controller"), logicalProfile,
                                 control, primaryScope(), fallbackScope(),
                                 control == ControlId::Capture || control == ControlId::Guide);
            });
    connect(m_gameInput.get(), &ModernInput::GameInputRouter::systemControlReleased,
            this, [this, gameInputProfile](const QString& control, const QString& logicalId,
                         const QString&) {
                // cpo-o06e: the release is tracked even while a close waits, so
                // the handoff can see the pad become neutral. Releases never fire
                // actions that a suppression would have to swallow.
                m_held.noteReleased(gameInputDeviceKey(logicalId), control);
                noteSystemControlReleased(logicalId, control,
                                          ModernInput::ControllerProvider::GameInput);
                m_runtime->release(QStringLiteral("controller"), gameInputProfile(logicalId),
                                   control);
                stopNavRepeatOnRelease(control);
            });
    connect(m_gameInput.get(), &ModernInput::GameInputRouter::sessionFallback,
            this, [this](const QString& reason) {
                traceProviderLifecycle(QStringLiteral("fallback: ") + reason);
                stopNavRepeat();
                m_runtime->cancelAll();
                m_legacyViewFallbackHeld.clear();
            });
    connect(m_gameInput.get(), &ModernInput::GameInputRouter::lifecycleReset,
            this, [this](const QString& logicalId, const QString& reason) {
                traceProviderLifecycle(QStringLiteral("reset-before: ") + reason);
                // cpo-o06e: the device is gone, so whatever it was holding went
                // with it — a handoff must not wait for a controller that cannot
                // deliver the release.
                m_held.noteDeviceGone(gameInputDeviceKey(logicalId));
                for (const QString& control : {ControlId::Capture, ControlId::Guide}) {
                    const QString key = logicalId + QChar(0x1f) + control;
                    m_systemControlOrigins.remove(key);
                    m_systemControlReleases.remove(key);
                }
                traceProviderLifecycle(QStringLiteral("reset-after: ") + reason);
                stopNavRepeat();
                m_runtime->cancelAll();
                m_legacyViewFallbackHeld.clear();
            });
    connect(m_gameInput.get(), &ModernInput::GameInputRouter::logicalControllerRekeyed,
            this, [this](const QString& previous, const QString& logicalId) {
                m_held.noteDeviceGone(gameInputDeviceKey(previous));
                m_held.noteDevicePresent(gameInputDeviceKey(logicalId));
                configureLogicalProfile(logicalId, {previous});
                m_bindingEditor->rekeyController(previous, logicalId);
                traceProviderLifecycle(QStringLiteral("rekey"));
            });
    connect(m_gameInput.get(), &ModernInput::GameInputRouter::statusChanged,
            this, &InputEngine::modernControllerChanged);
    connect(m_gameInput.get(), &ModernInput::GameInputRouter::statusChanged,
            this, [this] { traceProviderLifecycle(QStringLiteral("runtime-status")); });
    connect(m_gameInput.get(), &ModernInput::GameInputRouter::deviceConnected,
            this, [this](const QString& logicalId, bool) {
                m_held.noteDevicePresent(gameInputDeviceKey(logicalId));
                traceProviderLifecycle(QStringLiteral("GameInput-attached"));
                emit modernControllerChanged();
            });
    connect(m_gameInput.get(), &ModernInput::GameInputRouter::deviceDisconnected,
            this, [this](const QString& logicalId) {
                m_held.noteDeviceGone(gameInputDeviceKey(logicalId));
                traceProviderLifecycle(QStringLiteral("GameInput-detached"));
                emit modernControllerChanged();
            });

    // The Raw Input backend sees every HID arrival/removal (debounced),
    // including XInput and DirectInput devices. Use it as the hot-plug
    // trigger for the polling backends so they detect new pads immediately
    // without continuously probing empty slots.
    connect(m_sonyPad, &DualSenseDevice::deviceTopologyChanged, this, [this] {
        qInfo() << "Input: device topology changed — rescanning fallback backends";
        m_xinputPad->rescan();
        m_winmmPad->rescanAfterTopologyChange();
        updateXInputIdentity();
    });

    // Production selective Raw HID path (t26): a bound button usage on a
    // gamepad-class HID device no backend drives (WM_INPUT → usage transition
    // → canonical raw control) routes through the shared capability router
    // and then into the binding runtime like any other control.
    connect(m_sonyPad, &DualSenseDevice::rawHidControl, this,
            [this](const QString& identity, const QString& control, bool pressed) {
                const auto result = m_providers.routeRawHidEdge(
                    identity, control, pressed, quint64(m_controllerClock.elapsed()));
                const QString logicalId = m_providers.registry().logicalIdFor(
                    ModernInput::ControllerProvider::RawHid, identity);
                configureLogicalProfile(logicalId);
                // cpo-p05: selective Raw-HID is a mapping route like any other;
                // the same shared seam plans its winner before the first press.
                if (!logicalId.isEmpty())
                    ensureMappingRoute(logicalId);
                for (const QString& release : result.safeReleases)
                    m_runtime->release(QStringLiteral("controller"), logicalId, release);
                if (!result.accepted)
                    return;
                const auto family = ControlId::ControllerFamily::Generic;
                if (pressed) {
                    InputDiagnostics::instance().noteControl(
                        control, QStringLiteral("Raw HID"));
                    m_bindingEditor->noteObservedControl(control);
                    m_bindingEditor->setControllerProfile({
                        QStringLiteral("Raw HID"), logicalId, family,
                        QStringLiteral("Raw HID controller")});
                    beginInput(ControlId::label(control, family));
                    if (m_bindingEditor->captureInput(QStringLiteral("controller"),
                                                      control,
                                                      ControlId::label(control, family)))
                        return;
                    m_runtime->press(QStringLiteral("controller"), logicalId, control,
                                     primaryScope(), fallbackScope());
                } else {
                    m_runtime->release(QStringLiteral("controller"), logicalId, control);
                    stopNavRepeatOnRelease(control);
                }
            });
    connect(m_sonyPad, &DualSenseDevice::rawHidDeviceRemoved, this,
            [this](const QString& identity) {
                const QString logicalId = m_providers.registry().logicalIdFor(
                    ModernInput::ControllerProvider::RawHid, identity);
                const QStringList releases = m_providers.removeRawHid(identity);
                for (const QString& control : releases) {
                    m_runtime->release(QStringLiteral("controller"), logicalId, control);
                    stopNavRepeatOnRelease(control);
                }
            });

    // Surface cloaked pads (present in Windows, hidden from apps by a HID
    // filter driver) in Settings instead of silently detecting nothing.
    connect(m_sonyPad, &DualSenseDevice::hiddenPadsChanged, this,
            [this](const QStringList& pads, bool hidHidePresent) {
                if (pads.isEmpty()) {
                    setControllerWarning({}, false);
                    return;
                }
                const QString names = pads.join(QStringLiteral(", "));
                if (hidHidePresent) {
                    setControllerWarning(
                        //: Hidden-controller warning; %1 lists the hidden controllers.
                        //% "%1 is connected but hidden from applications by the HidHide driver (installed with DSX, DS4Windows, or reWASD)."
                        NativeText::get(QT_TRID_NOOP("gamehq.input.hidden.hidhide"),
                                        "%1 is connected but hidden from applications by the "
                                        "HidHide driver (installed with DSX, DS4Windows, or "
                                        "reWASD).").arg(names),
                        true);
                } else {
                    setControllerWarning(
                        //: Hidden-controller warning; %1 lists the hidden controllers.
                        //% "%1 is connected to Windows but invisible to applications — a HID filter driver is hiding it."
                        NativeText::get(QT_TRID_NOOP("gamehq.input.hidden.filter"),
                                        "%1 is connected to Windows but invisible to "
                                        "applications — a HID filter driver is hiding it.")
                            .arg(names),
                        false);
                }
            });

    // Hold-to-repeat tick for pad navigation (D-pad + L1/R1). Built once and
    // reused for whichever direction is currently held — only one pad button
    // is physically held at a time on a single d-pad/stick face.
    // No separate "initial delay" timer: startNavRepeat starts the tick
    // immediately at 220 ms (which also serves as the tap-release guard),
    // then each tick accelerates toward the 70 ms floor.
    m_repeatTick = new QTimer(this);
    m_repeatTick->setTimerType(Qt::PreciseTimer);
    m_repeatTick->setInterval(220);
    connect(m_repeatTick, &QTimer::timeout, this, [this] {
        if (m_repeatEmitter)
            m_repeatEmitter(m_repeatDirection);
        // Accelerate: shrink the interval toward the floor.
        // 0.77 = 0.88² — twice the shrink-per-tick of the old 0.88, so the
        // ramp reaches its floor in half as many ticks (2x faster ramp).
        m_repeatTick->setInterval(
            qMax(70, int(m_repeatTick->interval() * 0.77)));
    });
}

InputEngine::~InputEngine()
{
    shutdown();
}

void InputEngine::shutdown()
{
    if (m_shuttingDown)
        return;
    m_shuttingDown = true;
    m_started = false;

    // Disconnect before cancelling: editor notifications and synthesized
    // releases must not rearm monitoring or dispatch actions during teardown.
    disconnect(m_config, nullptr, this, nullptr);
    if (m_hotkeys)
        disconnect(m_hotkeys, nullptr, this, nullptr);
    disconnect(m_bindingEditor.get(), nullptr, this, nullptr);
    disconnect(m_runtime.get(), nullptr, this, nullptr);
    disconnect(m_mouse.get(), nullptr, this, nullptr);
    for (const auto& pad : m_pads)
        disconnect(pad.get(), nullptr, this, nullptr);
    if (m_gameInput)
        disconnect(m_gameInput.get(), nullptr, this, nullptr);

    clearPendingCandidate();
    stopNavRepeat();
    m_runtime->cancelAll();
    m_legacyViewFallbackHeld.clear();
    m_bindingEditor->cancelCapture();
    m_bindingEditor->cancelTriggerCapture();

    // Join producers while every consumer member is alive. In particular,
    // MouseHookDevice::stop() synchronously releases held mouse buttons;
    // waiting for its member destructor would outlive the repeat state.
    m_mouse->stop();
    if (m_gameInput)
        m_gameInput->shutdown();
    m_pads.clear();
}

void InputEngine::retranslate()
{
    m_bindingEditor->retranslate();
}

QObject* InputEngine::bindingEditor() const
{
    return m_bindingEditor.get();
}

QObject* InputEngine::mappingPresets() const
{
    return m_mappingPresets.get();
}

MappingPresetModel* InputEngine::mappingPresetsModel() const
{
    return m_mappingPresets.get();
}

void InputEngine::start()
{
    if (m_shuttingDown)
        return;
    if (m_db)
        m_db->seedDefaultBindings();
    m_started = true;   // reloadBindings() below syncs the mouse hook state
    reloadBindings();
    for (const auto& pad : m_pads)
        pad->start();
    m_gameInput->start();
    // Diagnostics only: reports main-thread stalls into the log so user
    // reports can correlate input hitches with slow backend calls (PerfTrace).
    new EventLoopStallMonitor(this);
}

void InputEngine::reloadBindings()
{
    m_runtime->reload();
    // cpo-p05: an installed preset table survives a reload of binding_overrides
    // (its content does not come from there), but the inherited tables every
    // other chain serves just changed - and so did the p03 bridge state. A
    // refresh is a no-op unless a chain actually resolves differently now.
    refreshResolvedPreset();
    emit mappingTablesChanged();
    syncMouseMonitoring();
    ModernInput::SelectiveRawHidFallback::instance().setBoundControls(
        ModernInput::persistedRawHidControls(*m_runtime, m_db));
    if (!m_hotkeys)
        return;

    // Apply replacements before clearing removed actions, preserving the live
    // shortcut if Windows rejects a newly requested chord.
    QSet<QString> desiredBindings;
    for (const auto& binding : m_runtime->effectiveBindings(QStringLiteral("keyboard"))) {
        const auto* action = ActionCatalog::find(binding.actionId);
        if (!action || action->scope != ActionCatalog::Scope::Global
            || binding.activation != QLatin1String("press"))
            continue;
        desiredBindings.insert(binding.actionId + QLatin1Char('#')
                               + QString::number(binding.slot));
        const auto chord = HotkeyManager::parseChord(binding.triggerCode);
        if (!chord.valid) {
            qWarning().noquote() << QStringLiteral("Hotkey: ignored invalid saved binding %1: %2")
                                      .arg(binding.triggerCode, chord.rejectionReason);
            continue;
        }
        m_hotkeys->applyBindingSlot(binding.actionId, binding.slot,
                                    chord.modifiers, chord.vk);
    }
    for (const auto& action : ActionCatalog::all()) {
        if (action.scope != ActionCatalog::Scope::Global)
            continue;
        for (int slot = 1; slot <= 2; ++slot) {
            const QString key = action.id + QLatin1Char('#') + QString::number(slot);
            if (!desiredBindings.contains(key))
                m_hotkeys->clearBindingSlot(action.id, slot);
        }
    }
}

bool InputEngine::needsMouseMonitoring() const
{
    return MouseMonitorPolicy::needed(
        !m_runtime->effectiveBindings(QStringLiteral("mouse")).isEmpty(),
        m_bindingEditor->deviceGroup(),
        m_bindingEditor->captureActive(),
        m_bindingEditor->editorCaptureStep());
}

void InputEngine::syncMouseMonitoring()
{
    if (!m_started)
        return;
    if (needsMouseMonitoring())
        m_mouse->start();
    else
        m_mouse->stop();   // releases anything still held through buttonReleased
}

bool InputEngine::handleKeyPressed(int key, int modifiers, bool autoRepeat)
{
    const QString trigger = keyboardTrigger(key, modifiers);
    if (trigger.isEmpty())
        return false;
    if (!autoRepeat) {
        beginInput(trigger);
        ModernInput::SelectiveRawHidFallback::instance().observeKeyboard(trigger);
    }
    if (!autoRepeat && m_bindingEditor->captureInput(
            QStringLiteral("keyboard"), trigger, trigger))
        return true;
    if (autoRepeat) {
        // BindingRuntime owns a consistent accelerating repeat curve.
        const auto bindings = m_runtime->effectiveBindings(QStringLiteral("keyboard"));
        for (const auto& binding : bindings) {
            if (binding.triggerCode == trigger)
                return true;
        }
        return false;
    }
    m_heldKeyTriggers.insert(key, { trigger, modifiers });
    return m_runtime->press(QStringLiteral("keyboard"), {}, trigger,
                            primaryScope(), fallbackScope());
}

bool InputEngine::handleKeyReleased(int key, int modifiers)
{
    // Release exactly what this key pressed. Rebuilt from the release-time
    // modifiers, Ctrl let go before the arrow turned "Ctrl+Left" into "Left",
    // which matched nothing: the shortcut stayed held and kept repeating.
    const HeldKeyTrigger held = m_heldKeyTriggers.take(key);
    const QString trigger = held.trigger.isEmpty() ? keyboardTrigger(key, modifiers) : held.trigger;
    bool handled = false;
    if (!trigger.isEmpty()) {
        handled = m_runtime->release(QStringLiteral("keyboard"), {}, trigger);
        stopNavRepeatOnRelease(trigger);
    }
    // A modifier released first ends every shortcut that needed it; the key
    // still down does not start its unmodified binding (it was never pressed).
    int modifier = 0;
    switch (key) {
    case Qt::Key_Control: modifier = Qt::ControlModifier; break;
    case Qt::Key_Shift:   modifier = Qt::ShiftModifier; break;
    case Qt::Key_Alt:     modifier = Qt::AltModifier; break;
    case Qt::Key_Meta:    modifier = Qt::MetaModifier; break;
    default: break;
    }
    if (modifier) {
        for (auto it = m_heldKeyTriggers.begin(); it != m_heldKeyTriggers.end();) {
            if (!(it->modifiers & modifier)) {
                ++it;
                continue;
            }
            handled = m_runtime->release(QStringLiteral("keyboard"), {}, it->trigger) || handled;
            stopNavRepeatOnRelease(it->trigger);
            it = m_heldKeyTriggers.erase(it);
        }
    }
    return handled;
}

void InputEngine::attachGamepad(std::unique_ptr<Gamepad> pad, const QString& displayName)
{
    Gamepad* raw = pad.get();
    // cpo-o06e: a backend exists = a device the handoff may have to wait for.
    m_held.noteDevicePresent(legacyDeviceKey(raw));
    connect(raw, &Gamepad::controlPressed, this, &InputEngine::onControlPressed);
    connect(raw, &Gamepad::controlReleased, this, &InputEngine::onControlReleased);
    connect(raw, &Gamepad::connected, this, [this, raw, displayName](bool c) {
        if (raw == m_sonyPad)
            m_sonyConnected = c;
        else if (raw == m_xinputPad)
            m_xinputConnected = c;
        else if (raw == m_winmmPad)
            m_winmmConnected = c;

        if (raw != m_xinputPad) {
            if (c)
                observeLegacyBackend(raw, raw->profile());
            else
                removeLegacyBackend(raw);
        }
        if (!c) {
            m_backendLastControlMs.remove(raw);
            m_backendLastReleaseMs.remove(raw);
            m_backendCandidateFirstMs.remove(raw);
            if (m_pending.source == raw)
                clearPendingCandidate();
        }
        if (raw == m_xinputPad)
            updateXInputIdentity();
        updateActiveBackend();
        if (!c && !anyBackendConnected())
            setLastInput(displayName + QStringLiteral(" disconnected"));
    });
    m_pads.push_back(std::move(pad));
}

ModernInput::ControllerProvider InputEngine::providerFor(const Gamepad* pad) const
{
    if (pad == m_sonyPad)
        return ModernInput::ControllerProvider::SonyRaw;
    if (pad == m_xinputPad)
        return ModernInput::ControllerProvider::XInput;
    return ModernInput::ControllerProvider::WinMM;
}

// Report (or refresh) a connected legacy backend's attachment in the shared
// registry. The fingerprint doubles as the provider device id; when it
// changes (XInput slot → correlated hardware identity) the old attachment is
// replaced so the registry never holds a stale one.
void InputEngine::observeLegacyBackend(Gamepad* pad,
                                       const ControlId::DeviceProfile& profile)
{
    if (profile.fingerprint.isEmpty())
        return;
    const QString previousLogical = m_providers.registry().logicalIdFor(
        providerFor(pad), profile.fingerprint);

    ModernInput::ControllerCapabilities capabilities =
        ModernInput::ControllerCapability::StandardControls;
    if (pad == m_sonyPad) {
        // DualSense/DS4 report a true Create/Share and the PS button.
        capabilities |= ModernInput::ControllerCapability::SystemShare;
        capabilities |= ModernInput::ControllerCapability::Guide;
    } else if (pad == m_xinputPad) {
        // Standard XInput has no Share; Guide arrives via ordinal 100.
        capabilities |= ModernInput::ControllerCapability::Guide;
    } else if (profile.family == ControlId::ControllerFamily::PlayStation) {
        // WinMM with the Sony button order carries Share and PS.
        capabilities |= ModernInput::ControllerCapability::SystemShare;
        capabilities |= ModernInput::ControllerCapability::Guide;
    }
    QStringList rekeyReleases;
    const QString logicalId = m_providers.observeLegacy(
        providerFor(pad), profile.fingerprint, profile.modelFingerprint,
        profile.displayName, capabilities, &rekeyReleases,
        profile.endpointId, profile.containerId, profile.deviceRoot);
    for (const QString& control : rekeyReleases) {
        m_runtime->release(QStringLiteral("controller"), previousLogical, control);
    }
    m_legacyObservedIds[pad].insert(profile.fingerprint);
    configureLogicalProfile(logicalId,
                            previousLogical == logicalId ? QStringList{}
                                                         : QStringList{previousLogical});
    if (previousLogical != logicalId)
        traceProviderLifecycle(QStringLiteral("legacy-observed"));
}

void InputEngine::removeLegacyBackend(Gamepad* pad, const QString& providerDeviceId)
{
    // cpo-o06e: the backend is gone, so anything it was holding is gone with it:
    // a handoff must never wait for a device that can no longer send a release.
    m_held.noteDeviceGone(legacyDeviceKey(pad));
    QSet<QString> observed = m_legacyObservedIds.value(pad);
    if (!providerDeviceId.isEmpty())
        observed.intersect(QSet<QString>{providerDeviceId});
    for (const QString& id : observed) {
        const QString logicalId = canonicalProfile(pad, id);
        // A disconnect cancels, rather than completes, a native tap or hold.
        // Its physical state is gone even when another backend remains active.
        const QString group = QStringLiteral("controller");
        const QStringList down = m_runtime->downControls(group, logicalId);
        m_runtime->invalidateGestureStateFor(group, logicalId);
        const QString prefix = logicalId + QChar(0x1f);
        m_systemControlOrigins.remove(prefix + ControlId::Capture);
        m_systemControlOrigins.remove(prefix + ControlId::Guide);
        m_systemControlReleases.remove(prefix + ControlId::Capture);
        m_systemControlReleases.remove(prefix + ControlId::Guide);
        const QStringList releases =
            m_providers.removeLegacy(providerFor(pad), id);
        for (const QString& control : releases)
            m_runtime->release(group, logicalId, control);
        for (const QString& control : down) {
            m_runtime->clearReleaseGate(group, logicalId, control);
            m_runtime->notePhysicalRelease(group, logicalId, control);
        }
        m_legacyObservedIds[pad].remove(id);
    }
    if (m_legacyObservedIds.value(pad).isEmpty())
        m_legacyObservedIds.remove(pad);
    traceProviderLifecycle(QStringLiteral("legacy-removed"));
    pruneStaleControllers();
}

void InputEngine::pruneStaleControllers()
{
    if (m_shuttingDown || m_mappingSwitchRunning)
        return;   // never reshape the tracked chains mid-switch or in teardown
    const QString controller = QStringLiteral("controller");
    const QString pinned = m_bindingEditor->pinnedProfile();
    const QString pendingProfile = m_pending.source
        ? canonicalProfile(m_pending.source, m_pending.fingerprint) : QString();
    const QStringList pruned = m_providers.registry().pruneUnreachable(
        [&](const QString& logicalId) {
            return logicalId == pinned || logicalId == m_lastControllerRoute
                || logicalId == pendingProfile
                || !m_runtime->downControls(controller, logicalId).isEmpty();
        });
    for (const QString& logicalId : pruned) {
        m_profileMigrationAliases.remove(logicalId);
        m_legacyViewFallbackHeld.remove(logicalId);
        // Nothing can press this route again, so a table installed for it
        // would only be re-planned on every refresh forever.
        const auto chain = m_mappingChains.constFind(mappingChainKey(controller, logicalId));
        if (chain != m_mappingChains.cend()) {
            if (chain->owned)
                m_runtime->uninstallPresetTable(controller, logicalId);
            m_mappingChains.erase(chain);
        }
    }
}

QString InputEngine::canonicalProfile(Gamepad* pad, const QString& providerDeviceId) const
{
    const QString logicalId = m_providers.registry().logicalIdFor(
        providerFor(pad), providerDeviceId);
    return logicalId.isEmpty() ? providerDeviceId : logicalId;
}

void InputEngine::configureLogicalProfile(const QString& logicalId,
                                          const QStringList& migrationAliases)
{
    const auto* logical = m_providers.registry().controller(logicalId);
    if (!logical || logicalId.isEmpty())
        return;
    QStringList aliases = m_profileMigrationAliases.value(logicalId);
    for (const QString& alias : migrationAliases) {
        if (!alias.isEmpty() && !aliases.contains(alias))
            aliases.append(alias);
    }
    m_profileMigrationAliases.insert(logicalId, aliases);
    if (!logical->modelFingerprint.isEmpty())
        aliases.append(logical->modelFingerprint);
    for (const auto& attachment : logical->providers) {
        if (!attachment.providerDeviceId.isEmpty()
            && attachment.providerDeviceId != logicalId
            && !aliases.contains(attachment.providerDeviceId))
            aliases.append(attachment.providerDeviceId);
    }
    m_runtime->setProfileAliases(logicalId, aliases);
    // cpo-p03 migration bridge: the ordered chain is installed now; a durable
    // identity may materialize and prove it, a weak one keeps legacy resolution
    // and leaves no persistent state behind. The call is a memoized no-op on
    // the press path once the chain is settled.
    if (m_presetMaterializer) {
        const bool durable = logical->confidence == ModernInput::IdentityConfidence::Strong;
        (void)m_presetMaterializer->considerControllerChain(logicalId, durable);
    }
}

bool InputEngine::routeLegacySystemEdge(Gamepad* source,
                                        const QString& providerDeviceId,
                                        const QString& controlId, bool pressed)
{
    const QString logicalProfile = canonicalProfile(source, providerDeviceId);
    const QString key = logicalProfile + QChar(0x1f) + controlId;
    const auto provider = providerFor(source);
    const auto* logical = m_providers.registry().controller(logicalProfile);
    const auto recent = m_systemControlReleases.constFind(key);
    if (pressed && !m_systemControlOrigins.contains(key) && logical
        && logical->confidence == ModernInput::IdentityConfidence::Strong
        && recent != m_systemControlReleases.cend()
        && !recent->origin.participants.contains(provider)
        && m_controllerClock.elapsed() - recent->timeMs
               <= ControllerArbitration::BackendDuplicateWindowMs) {
        ++m_mirrorWindowDrops;
        return false;
    }
    const auto result = m_providers.routeLegacySystemEdge(
        provider, providerDeviceId, controlId, pressed,
        quint64(m_controllerClock.elapsed()));
    for (const QString& release : result.safeReleases)
        m_runtime->release(QStringLiteral("controller"),
                            canonicalProfile(source, providerDeviceId), release);
    if (result.accepted) {
        if (pressed)
            m_systemControlOrigins.insert(key, {provider, {provider}});
        else
            noteSystemControlReleased(logicalProfile, controlId, provider);
    } else if (pressed && result.duplicate) {
        const auto origin = m_systemControlOrigins.find(key);
        if (origin != m_systemControlOrigins.end())
            origin->participants.insert(provider);
    }
    return result.accepted;
}

void InputEngine::noteSystemControlReleased(const QString& logicalProfile,
                                            const QString& controlId,
                                            ModernInput::ControllerProvider provider)
{
    if (controlId != ControlId::Capture && controlId != ControlId::Guide)
        return;
    const QString key = logicalProfile + QChar(0x1f) + controlId;
    const auto origin = m_systemControlOrigins.find(key);
    const SystemControlOrigin initiatingProvider = origin == m_systemControlOrigins.end()
        ? SystemControlOrigin{provider, {provider}} : *origin;
    if (origin != m_systemControlOrigins.end())
        m_systemControlOrigins.erase(origin);
    m_systemControlReleases.insert(key, {initiatingProvider, m_controllerClock.elapsed()});
}

bool InputEngine::hasExplicitViewBinding(const QString& logicalProfile) const
{
    for (const auto& binding : m_runtime->effectiveBindings(
             QStringLiteral("controller"), logicalProfile)) {
        if (binding.trigger().controls.contains(ControlId::ViewBack))
            return true;
    }
    return false;
}

QString InputEngine::viewBackFallbackKey(const QString& logicalProfile) const
{
    const auto* logical = m_providers.registry().controller(logicalProfile);
    if (!logical)
        return {};
    for (const auto& attachment : logical->providers) {
        if (attachment.provider != ModernInput::ControllerProvider::XInput
            || !m_providers.allowsLegacyViewFallback(attachment.provider,
                                                     attachment.providerDeviceId, false))
            continue;
        // Strong identity is earned by endpoint/container evidence, never VID/PID.
        // Otherwise this is explicitly a source setting, not a physical-pad claim.
        return logical->confidence == ModernInput::IdentityConfidence::Strong
            ? QStringLiteral("controller:") + logicalProfile
            : QStringLiteral("source:") + attachment.providerDeviceId;
    }
    return {};
}

bool InputEngine::viewBackFallbackAvailable() const
{
    return !viewBackFallbackKey(m_bindingEditor->pinnedProfile()).isEmpty();
}

bool InputEngine::viewBackFallbackEnabledFor(const QString& logicalProfile) const
{
    const QString key = viewBackFallbackKey(logicalProfile);
    return !key.isEmpty() && m_viewBackFallbackPolicies.value(key, true).toBool();
}

bool InputEngine::viewBackFallbackEnabled() const
{
    return viewBackFallbackEnabledFor(m_bindingEditor->pinnedProfile());
}

QString InputEngine::viewBackFallbackScope() const
{
    const QString key = viewBackFallbackKey(m_bindingEditor->pinnedProfile());
    return key.startsWith(QLatin1String("source:")) ? key.mid(7) : m_bindingEditor->controllerName();
}

void InputEngine::setViewBackFallbackEnabled(bool enabled)
{
    const QString key = viewBackFallbackKey(m_bindingEditor->pinnedProfile());
    if (key.isEmpty() || viewBackFallbackEnabled() == enabled)
        return;
    QVariantMap policies = m_viewBackFallbackPolicies;
    if (enabled)
        policies.remove(key);
    else
        policies.insert(key, false);
    m_config->setValue(ConfigKeys::InputViewBackCaptureFallback, policies);
    m_config->save();
}

void InputEngine::applyViewBackFallbackPolicies()
{
    const QVariantMap next = m_config->value(ConfigKeys::InputViewBackCaptureFallback).toMap();
    for (const auto& logical : m_providers.registry().controllers()) {
        const QString key = viewBackFallbackKey(logical.logicalId);
        if (key.isEmpty()
            || m_viewBackFallbackPolicies.value(key, true).toBool() == next.value(key, true).toBool())
            continue;
        const QString group = QStringLiteral("controller");
        const QStringList down = m_runtime->downControls(group, logical.logicalId);
        // A policy change is a route boundary: an old tap/hold must never finish
        // after the setting changes. Other controllers and native Share routes survive.
        m_runtime->invalidateGestureStateFor(group, logical.logicalId);
        for (const QString& control : down)
            m_runtime->armReleaseGate(group, logical.logicalId, control);
        m_legacyViewFallbackHeld.remove(logical.logicalId);
        if (m_pending.source
            && canonicalProfile(m_pending.source, m_pending.fingerprint) == logical.logicalId) {
            if (!m_pending.released)
                m_runtime->armReleaseGate(group, logical.logicalId, m_pending.controlId);
            clearPendingCandidate();
        }
        if (m_repeatRouteProfile == logical.logicalId)
            stopNavRepeat();
    }
    m_viewBackFallbackPolicies = next;
    emit viewBackFallbackChanged();
}

void InputEngine::releaseControllerControl(const QString& logicalProfile, const QString& controlId)
{
    const QString group = QStringLiteral("controller");
    m_runtime->release(group, logicalProfile, controlId);
    if (controlId == ControlId::ViewBack) {
        // Clear the synthetic release gate too after a policy/mapping boundary,
        // even when its held marker was cancelled by that boundary.
        if (m_legacyViewFallbackHeld.remove(logicalProfile)
            || m_runtime->releaseGateArmed(group, logicalProfile, ControlId::Capture))
            m_runtime->release(group, logicalProfile, ControlId::Capture);
    }
}

namespace {
// Throttle for routing snapshots pushed from the press path: a mirrored
// remapper can deliver duplicate events at device rate, and the export needs
// the current state, not one write per dropped event.
constexpr qint64 kRoutingPushThrottleMs = 1000;
} // namespace

// ---------------------------------------------------------------- cpo-x01
// The one-click export's routing evidence: which provider serves the logical
// controller, how the current route's identity is treated, whether a candidate
// press is being held, and what the arbitration suppressed. Pushed at routing
// transitions, and from the press path at most once per kRoutingPushThrottleMs.
//
// Provider-local facts only: the engine never states that two providers see
// the same physical device - that question belongs to cpo-c03b and is open.
void InputEngine::publishControllerRouting()
{
    const qint64 now = m_controllerClock.elapsed();
    m_lastRoutingPushMs = now;
    QStringList lines;
    QString routeProfile;
    if (m_activeBackend) {
        auto profile = m_activeBackend->profile();
        routeProfile = canonicalProfile(m_activeBackend, profile.fingerprint);
    }
    if (routeProfile.isEmpty()) {
        lines << QStringLiteral("route identity: no controller route");
    } else {
        const auto* logical = m_providers.registry().controller(routeProfile);
        const bool durable = logical
            && logical->confidence == ModernInput::IdentityConfidence::Strong;
        lines << (durable
                      ? QStringLiteral("route identity: durable controller id (strong confidence)")
                      : QStringLiteral("route identity: legacy slot (weak or unknown confidence)"));
    }
    if (m_pending.source) {
        lines << QStringLiteral("candidate press held: %1 via %2 (%3 ms)")
                     .arg(m_pending.controlId, backendDisplayName(m_pending.source))
                     .arg(now - m_pending.pressedMs);
    }
    QStringList ages;
    const QVector<Gamepad*> legacyBackends{m_sonyPad, m_xinputPad, m_winmmPad};
    for (Gamepad* pad : legacyBackends) {
        if (!pad || !backendConnected(pad))
            continue;
        const auto lastIt = m_backendLastControlMs.constFind(pad);
        ages << (lastIt == m_backendLastControlMs.cend()
                     ? QStringLiteral("%1 never").arg(backendDisplayName(pad))
                     : QStringLiteral("%1 %2 s").arg(backendDisplayName(pad),
                             QString::number(double(now - *lastIt) / 1000.0, 'f', 1)));
    }
    if (!ages.isEmpty())
        lines << QStringLiteral("provider last-control ages: ") + ages.join(QStringLiteral(", "));
    lines << QStringLiteral("events dropped inside the mirror window: %1").arg(m_mirrorWindowDrops);
    lines << QStringLiteral("candidate presses held for confirmation: %1").arg(m_candidateRuns);

    InputDiagnostics::instance().setControllerRouting(
        m_activeBackend ? backendDisplayName(m_activeBackend) : QStringLiteral("none"),
        m_lastBackendSwitchReason, lines);
}

void InputEngine::traceProviderLifecycle(const QString& event)
{
    const QString context = QStringLiteral("overlay=%1 releasing=%2 held=%3 attached=%4 policy=%5 runtime=%6")
        .arg(m_overlayVisible).arg(m_overlayReleaseActive).arg(m_held.heldCount())
        .arg(m_gameInputFocus && m_gameInputFocus->policyAttached())
        .arg(m_gameInputFocus ? m_gameInputFocus->focusModeName() : QStringLiteral("none"),
             m_gameInput ? m_gameInput->runtimeStatus() : QStringLiteral("none"));
    auto& diag = InputDiagnostics::instance();
    diag.noteProviderTransition(event, QStringLiteral("summary"), {}, {}, {}, {}, {}, context);
    for (const auto& controller : m_providers.registry().controllers()) {
        for (const auto& attachment : controller.providers) {
            diag.noteProviderTransition(event, QString::number(int(attachment.provider)),
                attachment.providerDeviceId, controller.logicalId, controller.containerId,
                controller.endpointId, controller.topologyRoot,
                QStringLiteral("model=%1 evidence=%2 %3").arg(controller.modelFingerprint)
                    .arg(int(attachment.evidence)).arg(context));
        }
    }
}

// The mapping half of the one-click export: the current winner per tracked
// route, the running game context, and the assignment rows the resolver had to
// skip. Profiles and target keys are identity strings, so InputDiagnostics
// hashes them - this method never formats raw values itself.
void InputEngine::publishMappingDiagnostics()
{
    QVector<InputDiagnostics::MappingChainSnapshot> chains;
    chains.reserve(m_mappingChains.size());
    for (auto it = m_mappingChains.cbegin(); it != m_mappingChains.cend(); ++it) {
        const MappingChainState& state = it.value();
        chains.append({state.group, state.profile, state.source, state.presetId,
                       state.owned, state.fingerprint});
    }
    QVector<InputDiagnostics::StaleAssignmentSnapshot> stale;
    const auto reports = m_assignmentResolver->staleReports();
    stale.reserve(reports.size());
    for (const MappingAssignmentResolver::StaleAssignment& report : reports) {
        stale.append({report.deviceGroup, report.targetKind, report.targetKey,
                      report.presetId, report.reason});
    }
    InputDiagnostics::instance().setMappingState(m_runningGameKey, chains, stale);
}

void InputEngine::updateXInputIdentity()
{
    if (!m_sonyPad || !m_xinputPad)
        return;
    const int slot = m_xinputPad->firstConnectedSlot();
    if (slot < 0) {
        for (int index = 0; index < 4; ++index)
            m_xinputPad->setKnownDeviceIdentity(index, {});
        return;
    }
    const QString fingerprint = ControllerIdentity::resolveXInputFingerprint(
        m_sonyPad->xinputClassEndpoints(), m_xinputPad->connectedSlotCount(), slot);
    const bool stable = !ControllerIdentity::isLegacySlotFingerprint(fingerprint);
    const QStringList models = m_sonyPad->xinputClassIdentities();
    m_xinputPad->setKnownDeviceIdentity(
        slot, stable ? fingerprint : QString(), models.size() == 1 ? models.front() : QString());
    // The fingerprint (slot → stable hardware identity or back) is also the
    // registry attachment id — refresh it so cross-provider correlation and
    // Share/Guide dedup key on the identity the pad currently reports.
    if (m_xinputConnected)
        observeLegacyBackend(m_xinputPad, m_xinputPad->profile());
    if (m_activeBackend == m_xinputPad) {
        auto profile = m_xinputPad->profile();
        profile.fingerprint = canonicalProfile(m_xinputPad, profile.fingerprint);
        m_bindingEditor->setControllerProfile(profile);
    }
}

void InputEngine::setGameInputFocusController(ModernInput::GameInputFocusController* controller)
{
    m_gameInputFocus = controller;
    // The router already exists (created in the constructor) and its runtime
    // session has not started yet: the app installs the controller before
    // start(), which is what the router's own precondition asks for.
    if (m_gameInput)
        m_gameInput->setFocusController(controller);
}

// cpo-o06e: the release handoff's source. "Held" means a control the engine has
// seen pressed and not yet released, tracked from the raw edges
// (input/HeldControlTracker.h) — never a re-interpretation: the thresholds and
// deadzones that produced those edges stay with the backend that published them.
ModernInput::NeutralHandoffSample InputEngine::sampleNeutralPadState() const
{
    ModernInput::NeutralHandoffSample sample;
    sample.attachedDevices = m_held.presentDevices();
    sample.heldControls = m_held.heldCount();
    sample.heldSummary =
        m_held.heldControls(ModernInput::kNeutralHandoffSummaryLimit).join(QStringLiteral(", "));
    sample.overlayActionsQuiesced = m_overlayReleaseActive;
    return sample;
}

void InputEngine::setOverlayReleaseActive(bool active)
{
    if (m_overlayReleaseActive == active)
        return;
    m_overlayReleaseActive = active;
    if (!active)
        return;
    // The overlay input path goes quiet the moment the close starts — the same
    // three cancellations the context switches use, so no navigation repeat and
    // no pending gesture can still fire an action into the window that is going
    // away. The PHYSICAL state keeps being tracked, because the handoff is
    // waiting for exactly that release, and nothing is ever pushed into the game.
    stopNavRepeat();
    m_runtime->cancelAll();
    m_legacyViewFallbackHeld.clear();
    qInfo() << "Input: overlay input quiesced - a close is waiting for a neutral controller";
}

void InputEngine::setOverlayVisible(bool visible)
{
    if (m_overlayVisible == visible)
        return;
    m_overlayVisible = visible;
    if (!visible)
        m_playbackActive = false;
    else
        m_held.markHeldAsStale();   // pre-open holds must not stall the close
    // A held navigation button shouldn't keep firing into the window we just
    // left — stop any in-flight repeat when the focus context switches.
    stopNavRepeat();
    m_runtime->cancelAll();
    m_legacyViewFallbackHeld.clear();
    qInfo() << "Input: overlay capture"
            << (visible ? "active — routing controller to overlay only"
                        : "inactive — global triggers only");
}

void InputEngine::setDesktopFocused(bool focused)
{
    if (m_desktopFocused == focused)
        return;
    m_desktopFocused = focused;
    // A gesture started while GameHQ owned the pad must not finish after the
    // window changed hands: a Cross held in the gallery could otherwise fire
    // desktop.confirm — or worse, desktop.bulk_toggle — into whatever has
    // focus now. Same contract as setOverlayVisible/setPlaybackActive.
    stopNavRepeat();
    m_runtime->cancelAll();
    m_legacyViewFallbackHeld.clear();
}

void InputEngine::setPlaybackActive(bool active)
{
    if (m_playbackActive == active)
        return;
    m_playbackActive = active;
    stopNavRepeat();
    m_runtime->cancelAll();
    m_legacyViewFallbackHeld.clear();
}

void InputEngine::cancelNavigationRepeat()
{
    stopNavRepeat();
}

void InputEngine::onControlPressed(const QString& controlId, int family,
                                   const QString&, const QString& fingerprint,
                                   const QString&)
{
    auto* source = qobject_cast<Gamepad*>(sender());
    if (!source || !backendConnected(source))
        return;
    // cpo-o06e: the raw edge, before any arbitration — a control the user holds
    // counts for the release handoff whether or not this press fires an action.
    m_held.notePressed(legacyDeviceKey(source), controlId);
    if (m_overlayReleaseActive)
        return;   // a close is waiting for neutral: no further actions fire
    const auto currentProfile = source->profile();
    if (currentProfile.fingerprint == fingerprint)
        observeLegacyBackend(source, currentProfile);

    // Genuine system capabilities have their own identity and deduplication.
    // A virtual Xbox navigation source cannot suppress a physical Share/Guide
    // press or take ownership of its release. View/Back stays on the ordinary
    // path, including its separately controlled compatibility alias.
    if (controlId == ControlId::Capture || controlId == ControlId::Guide) {
        deliverPress(source, controlId, family, fingerprint);
        return;
    }

    const qint64 now = m_controllerClock.elapsed();
    if (source != m_activeBackend) {
        // A polled mirror can arrive after the raw provider has already
        // released this control. Measure this same-control duplicate window
        // from the release as well as the press; otherwise a slow mirror is
        // buffered and replays a second toggle after candidate confirmation.
        const auto releases = m_backendLastReleaseMs.constFind(m_activeBackend);
        if (releases != m_backendLastReleaseMs.cend()) {
            const auto released = releases->constFind(controlId);
            if (released != releases->cend()
                && now - *released <= ControllerArbitration::BackendDuplicateWindowMs) {
                ++m_mirrorWindowDrops;
                return;
            }
        }
        const auto activeIt = m_backendLastControlMs.constFind(m_activeBackend);
        // Same fingerprint on a higher-priority backend = the same physical
        // pad reached us over a better path (Sony Raw Input and WinMM both
        // report VID:PID, so the DSX virtual pad matches across them); it may
        // upgrade without waiting out the silence threshold. XInput's slot
        // fingerprint never matches a VID:PID one, which is correct — a slot
        // proves nothing about physical identity.
        const bool higherPrioritySameDevice = m_activeBackend
            && backendPriority(source) > backendPriority(m_activeBackend)
            && !source->profile().fingerprint.isEmpty()
            && source->profile().fingerprint == m_activeBackend->profile().fingerprint;
        QString reason = QStringLiteral("control activity");
        if (activeIt != m_backendLastControlMs.cend()
            && !ControllerArbitration::backendMayTakeOver(
                true, *activeIt, now, higherPrioritySameDevice)) {
            // Inside the mirror window this event is a trailing duplicate of
            // a press the active backend already delivered. Holding it as a
            // candidate would replay it as a second action ~250 ms later
            // (the 0.7.3 double-navigation bug) — drop it outright. Only an
            // event past the window but before the takeover timeout can open
            // a candidate run.
            if (now - *activeIt <= ControllerArbitration::BackendDuplicateWindowMs) {
                // A trailing duplicate of a press the active backend already
                // delivered. Counted for the export, which is refreshed at most
                // once per kRoutingPushThrottleMs even while a mirrored remapper
                // keeps flooding this path.
                ++m_mirrorWindowDrops;
                if (m_lastRoutingPushMs < 0
                    || now - m_lastRoutingPushMs >= kRoutingPushThrottleMs)
                    publishControllerRouting();
                return;
            }
            if (!confirmCandidate(source, now, *activeIt)) {
                // Not proven yet — hold the press rather than drop it. If the
                // candidate turns out to be real it is delivered on promotion;
                // if the active backend answers, it was a mirror and dies.
                holdCandidatePress(source, controlId, family, fingerprint, now);
                return;
            }
            reason = QStringLiteral("sustained candidate");
        }
        activateBackend(source, reason);
        // A press held from this candidate's first event still counts: deliver
        // it before the one that confirmed the switch, in the order pressed.
        const PendingPress held = m_pending.source == source ? m_pending : PendingPress{};
        clearPendingCandidate();
        if (held.source)
            replayPendingPress(held);
    }
    // The active backend speaking is proof that a pending candidate press was
    // a mirror of it, not a failover in progress.
    if (m_pending.source && m_pending.source != source)
        clearPendingCandidate();
    m_backendLastControlMs.insert(source, now);
    deliverPress(source, controlId, family, fingerprint);
}

void InputEngine::deliverPress(Gamepad* source, const QString& controlId, int family,
                               const QString& fingerprint)
{
    const QString logicalProfile = canonicalProfile(source, fingerprint);
    // cpo-p05: a new logical route is a mapping boundary in itself. Resolve its
    // winner and, when that changes what serves, switch BEFORE this press is
    // delivered - so the press that revealed the route is already a press of
    // the new table, while every control still held on the old one is gated
    // until release.
    // cpo-p05: a new logical route is a mapping boundary in itself. Its winner
    // is planned and published BEFORE this press is dispatched - so the press
    // that revealed the route is already a press of the new table, while every
    // control still held on the old one is gated until release. The same shared
    // seam serves the legacy pad, GameInput and Raw-HID routes.
    ensureMappingRoute(logicalProfile);
    // Capture/Guide may also arrive through GameInput for the same physical
    // controller; the shared capability router guarantees exactly one edge.
    // A duplicate here means GameInput already delivered this press.
    if ((controlId == ControlId::Capture || controlId == ControlId::Guide)
        && !routeLegacySystemEdge(source, fingerprint, controlId, true))
        return;
    InputDiagnostics::instance().noteControl(controlId, backendDisplayName(source));
    // Which controls the pad genuinely delivers is only knowable by observation:
    // the editor uses it to warn about a button another app is intercepting.
    m_bindingEditor->noteObservedControl(controlId);
    auto editorProfile = source->profile();
    if (source == m_xinputPad) {
        for (int slot = 0; slot < 4; ++slot) {
            const auto slotProfile = m_xinputPad->profileForSlot(slot);
            if (slotProfile.fingerprint == fingerprint) {
                editorProfile = slotProfile;
                break;
            }
        }
    }
    editorProfile.fingerprint = logicalProfile;
    m_bindingEditor->setControllerProfile(editorProfile);
    if (controlId == ControlId::Guide)
        InputDiagnostics::instance().setGuideObserved(true);
    beginInput(ControlId::label(controlId, static_cast<ControlId::ControllerFamily>(family))
               + QStringLiteral(" pressed"));
    if (m_bindingEditor->captureInput(
            QStringLiteral("controller"), controlId,
            ControlId::label(controlId, static_cast<ControlId::ControllerFamily>(family))))
        return;
    // Diagnostics only: keep the paste showing the rows of the pad actually in
    // the user's hands, not just the shared controller table.
    m_runtime->setActiveProfile(QStringLiteral("controller"), logicalProfile);
    const bool handled = m_runtime->press(QStringLiteral("controller"), logicalProfile, controlId,
                                          primaryScope(), fallbackScope(),
                                          controlId == ControlId::Capture || controlId == ControlId::Guide);
    // XInput Back/View is now independently bindable. For profiles that have
    // no explicit View/Back pattern yet, preserve the historic built-in
    // Capture gestures as a capability fallback. As soon as the user binds
    // View/Back itself, that stable meaning wins and the alias is not entered.
    if (!handled && controlId == ControlId::ViewBack
        && m_providers.allowsLegacyViewFallback(
            providerFor(source), fingerprint, hasExplicitViewBinding(logicalProfile),
            viewBackFallbackEnabledFor(logicalProfile))) {
        if (m_runtime->press(QStringLiteral("controller"), logicalProfile,
                             ControlId::Capture, primaryScope(), fallbackScope()))
            m_legacyViewFallbackHeld.insert(logicalProfile);
    }
}

void InputEngine::onControlReleased(const QString& controlId, int, const QString&,
                                    const QString& fingerprint, const QString&)
{
    auto* source = qobject_cast<Gamepad*>(sender());
    // cpo-o06e: the raw release edge, before the confirmation/arbitration logic
    // — this is what lets a close's handoff observe the pad become neutral.
    if (source)
        m_held.noteReleased(legacyDeviceKey(source), controlId);
    if (source && backendConnected(source))
        m_backendLastReleaseMs[source].insert(controlId, m_controllerClock.elapsed());
    if (source && (controlId == ControlId::Capture || controlId == ControlId::Guide)) {
        if (backendConnected(source)
            && routeLegacySystemEdge(source, fingerprint, controlId, false)) {
            releaseControllerControl(canonicalProfile(source, fingerprint), controlId);
            stopNavRepeatOnRelease(controlId);
        }
        return;
    }
    // A release for a press still waiting on confirmation is remembered, not
    // forwarded: the press has not been delivered yet, so there is nothing to
    // release. Recording it is what keeps a tap a tap — on promotion the pair
    // is replayed back to back and the runtime sees a short press, never a
    // hold it never was.
    if (m_pending.source && source == m_pending.source
        && controlId == m_pending.controlId) {
        m_pending.released = true;
        return;
    }
    if (source != m_activeBackend) {
        // cpo-p05: a route that is no longer the active backend can still be
        // the one holding a switch gate - a provider failover mid-press must
        // not leave that gate armed forever. Its real release closes it, and
        // nothing else travels from a backend that is not delivering input.
        // The physical bookkeeping is repaired with the same seam the active
        // path uses: otherwise the recognizer would keep believing the control
        // is down and the NEXT switch would arm a phantom gate for it.
        const QString profile = canonicalProfile(source, fingerprint);
        if (m_runtime->clearReleaseGate(QStringLiteral("controller"), profile, controlId)) {
            m_runtime->notePhysicalRelease(QStringLiteral("controller"), profile, controlId);
            if (controlId == ControlId::ViewBack
                && m_runtime->clearReleaseGate(QStringLiteral("controller"), profile,
                                                ControlId::Capture))
                m_runtime->notePhysicalRelease(QStringLiteral("controller"), profile,
                                               ControlId::Capture);
            return;
        }
        return;
    }
    const QString logicalProfile = canonicalProfile(source, fingerprint);
    releaseControllerControl(logicalProfile, controlId);
    stopNavRepeatOnRelease(controlId);
}

void InputEngine::startNavRepeat(const QString& triggerCode, int direction,
                                 std::function<void(int)> emitter)
{
    // First, fire immediately so a quick tap still does one step.
    emitter(direction);
    // Record what we're now repeating — stopNavRepeat() uses m_repeatButton.
    m_repeatTrigger = triggerCode;
    // A chord trigger serializes both controls, but releases arrive one
    // control at a time: letting go of either part must end the repeat.
    const auto parsed = TriggerSpec::parse(triggerCode);
    m_repeatControls = parsed.ok && parsed.trigger.isChord()
        ? parsed.trigger.controls : QStringList{triggerCode};
    // ...and which mapping route it belongs to: a cpo-p05 switch may only end
    // the repeat of the route whose table it actually replaced. The origin is
    // the dispatch in progress (m_dispatchGroup/m_dispatchProfile); a repeat
    // started outside a dispatch belongs to no route and is never scoped out.
    m_repeatRouteGroup = m_dispatchGroup;
    m_repeatRouteProfile = m_dispatchProfile;
    m_repeatDirection = direction;
    m_repeatEmitter = std::move(emitter);
    // NO initial delay — kick off the accelerating tick immediately. The
    // first tick fires 220 ms after press, which doubles as the natural
    // "this was just a tap" guard: anything released before then is a
    // single step. After that, each tick accelerates toward the 70 ms floor.
    // If another direction was already repeating, this atomically replaces it.
    m_repeatTick->stop();
    m_repeatTick->setInterval(220);
    m_repeatTick->start();
}

void InputEngine::stopNavRepeatOnRelease(const QString& control)
{
    if (!control.isEmpty() && m_repeatControls.contains(control))
        stopNavRepeat();
}

void InputEngine::stopNavRepeat()
{
    m_repeatTick->stop();
    m_repeatTrigger.clear();
    m_repeatControls.clear();
    m_repeatRouteGroup.clear();
    m_repeatRouteProfile.clear();
    m_repeatDirection = 0;
    m_repeatEmitter = {};
}

ActionCatalog::Scope InputEngine::primaryScope() const
{
    // Scope selection is decided in one place (input/OverlayInputPolicy.cpp)
    // so the overlay's routing contract is testable without a device.
    return OverlayInput::primaryScope(
        {m_overlayVisible, m_playbackActive, desktopCanReceiveInput()});
}

ActionCatalog::Scope InputEngine::fallbackScope() const
{
    return OverlayInput::fallbackScope(
        {m_overlayVisible, m_playbackActive, desktopCanReceiveInput()});
}

void InputEngine::dispatchAction(const QString& actionId, const QString& triggerCode,
                                 const QString& deviceGroup, const QString& deviceProfile)
{
    if (m_shuttingDown)
        return;
    // Read by the capture handlers below, which the dispatch table calls with
    // the trigger code alone. Set here rather than threaded through every
    // handler signature: dispatch is synchronous and single-threaded, so the
    // value cannot be overwritten between this line and the handler call.
    m_dispatchSource = CaptureRequest::sourceForDeviceGroup(deviceGroup);
    // The route travels with the action so a lasting effect can record where it
    // came from (the navigation repeat does), under exactly the same contract.
    m_dispatchGroup = deviceGroup;
    m_dispatchProfile = deviceProfile;
    m_bindingEditor->setLastFiredAction(actionId);
    if (const auto* action = ActionCatalog::find(actionId))
        setLastInput(action->label);

    // Dispatch table: every bindable action maps to a handler. When a new action
    // is added to ActionCatalog, a matching entry must appear below — a missing
    // entry compiles and runs but is a silent no-op (caught by the qWarning at
    // the end).  The table is local-static: built once, never rebuilt.
    using Handler = void (InputEngine::*)(const QString& triggerCode);
    struct Entry { const char* actionId; Handler handler; };
    static const Entry table[] = {
        // Global
        { "global.screenshot",     &Self::handleScreenshot },
        { "global.save_replay",    &Self::handleSaveReplay },
        { "global.toggle_overlay", &Self::handleToggleOverlay },
        { "global.toggle_desktop", &Self::handleToggleDesktop },
        // Overlay
        { "overlay.navigate_left",  &Self::handleOverlayNavigateLeft },
        { "overlay.navigate_right", &Self::handleOverlayNavigateRight },
        { "overlay.navigate_up",    &Self::handleOverlayNavigateUp },
        { "overlay.navigate_down",  &Self::handleOverlayNavigateDown },
        { "overlay.confirm",        &Self::handleOverlayConfirm },
        { "overlay.back",           &Self::handleOverlayBack },
        { "overlay.favorite",       &Self::handleOverlayFavorite },
        { "overlay.menu",           &Self::handleOverlayMenu },
        { "overlay.sidebar_toggle", &Self::handleOverlaySidebarToggle },
        { "overlay.game_prev",      &Self::handleOverlayGamePrev },
        { "overlay.game_next",      &Self::handleOverlayGameNext },
        // Desktop
        { "desktop.navigate_left",  &Self::handleDesktopNavigateLeft },
        { "desktop.navigate_right", &Self::handleDesktopNavigateRight },
        { "desktop.navigate_up",    &Self::handleDesktopNavigateUp },
        { "desktop.navigate_down",  &Self::handleDesktopNavigateDown },
        { "desktop.confirm",        &Self::handleDesktopConfirm },
        { "desktop.back",           &Self::handleDesktopBack },
        { "desktop.favorite",       &Self::handleDesktopFavorite },
        { "desktop.menu",           &Self::handleDesktopMenu },
        { "desktop.tab_prev",       &Self::handleDesktopTabPrev },
        { "desktop.tab_next",       &Self::handleDesktopTabNext },
        { "desktop.settings",       &Self::handleDesktopSettings },
        { "desktop.zoom_out",       &Self::handleDesktopZoomOut },
        { "desktop.zoom_in",        &Self::handleDesktopZoomIn },
        { "desktop.scroll_up",      &Self::handleDesktopScrollUp },
        { "desktop.scroll_down",    &Self::handleDesktopScrollDown },
        { "desktop.bulk_toggle",    &Self::handleDesktopBulkToggle },
        // Playback
        { "playback.play_pause",    &Self::handlePlaybackPlayPause },
        { "playback.seek_back",     &Self::handlePlaybackSeekBack },
        { "playback.seek_forward",  &Self::handlePlaybackSeekForward },
        { "playback.frame_grab",     &Self::handleFrameGrab },
    };

    for (const auto& entry : table) {
        if (actionId == QLatin1String(entry.actionId)) {
            (this->*entry.handler)(triggerCode);
            return;
        }
    }

    qWarning().noquote()
        << QStringLiteral("Input: unknown action %1 — missing dispatch table entry?")
               .arg(actionId);
}

void InputEngine::setLastInput(const QString& text)
{
    if (m_lastInput == text)
        return;
    m_lastInput = text;
    qInfo() << "Input:" << text;
    emit lastInputChanged();
}

void InputEngine::beginInput(const QString& text)
{
    // Detection and dispatch are separate events. The old action must not look
    // like this press's result while a tap/hold is pending or unbound.
    m_bindingEditor->setLastFiredAction({});
    setLastInput(text);
}

void InputEngine::setControllerStatus(const QString& text)
{
    if (m_controllerStatus == text)
        return;
    m_controllerStatus = text;
    emit controllerStatusChanged();
}

void InputEngine::setControllerWarning(const QString& text, bool fixAvailable)
{
    if (m_controllerWarning == text && m_controllerFixAvailable == fixAvailable)
        return;
    m_controllerWarning = text;
    m_controllerFixAvailable = fixAvailable;
    emit controllerWarningChanged();
}

void InputEngine::startButtonProbe()
{
    InputDiagnostics::instance().startProbe();
    ModernInput::SelectiveRawHidFallback::instance().beginProbe();
    if (m_sonyPad)
        m_sonyPad->beginButtonProbe();
    m_probeRunning = true;
    m_probeStatus = QStringLiteral("Press the button now — recording for 3 seconds…");
    emit probeStatusChanged();
    // The summary is read slightly after the window closes so the last events
    // are in; the backends notice expiry themselves on their next event.
    QTimer::singleShot(InputDiagnostics::kProbeDurationMs + 200, this, [this] {
        m_probeRunning = false;
        auto& rawFallback = ModernInput::SelectiveRawHidFallback::instance();
        rawFallback.endProbe();
        const QString rawSummary = rawFallback.probeSummary();
        const QString backendSummary = InputDiagnostics::instance().probeSummary();
        m_probeStatus = rawSummary.startsWith(QLatin1String("No event was received"))
            ? backendSummary + QStringLiteral(" ") + rawSummary
            : backendSummary + QStringLiteral("; ") + rawSummary;
        emit probeStatusChanged();
    });
}

QString InputEngine::modernControllerStatus() const
{
    return m_gameInput ? m_gameInput->runtimeStatus() : QStringLiteral("Unavailable");
}

QString InputEngine::modernControllerSummary() const
{
    return m_gameInput ? m_gameInput->controllerSummary()
                       : QStringLiteral("No modern controller reported");
}

bool InputEngine::modernLayoutWarning() const
{
    return m_gameInput && m_gameInput->layoutWarning();
}

QVariantList InputEngine::modernLayoutWarnings() const
{
    QVariantList result;
    if (!m_gameInput)
        return result;
    for (const auto& warning : m_gameInput->layoutWarnings()) {
        QVariantMap item;
        item.insert(QStringLiteral("logicalId"), warning.logicalId);
        item.insert(QStringLiteral("displayName"), warning.displayName);
        item.insert(QStringLiteral("assignments"), warning.staleAssignments);
        item.insert(QStringLiteral("description"), warning.staleAssignments.isEmpty()
            ? QStringLiteral("No saved assignments use the previous extra-button layout. "
                             "Confirmation enables the current layout without remapping buttons.")
            : QStringLiteral("Reassign after confirmation: %1. Confirmation does not remap "
                             "these old button IDs.")
                  .arg(warning.staleAssignments.join(QStringLiteral(", "))));
        result.append(item);
    }
    return result;
}

void InputEngine::copyControllerCompatibilityReport() const
{
    if (m_gameInput && QGuiApplication::clipboard())
        QGuiApplication::clipboard()->setText(m_gameInput->compatibilityReport());
}

void InputEngine::confirmModernControllerLayout(const QString& logicalId)
{
    if (!m_gameInput)
        return;
    if (m_gameInput->confirmLayout(logicalId)) {
        qInfo() << "Input: extra-button layout confirmed for" << logicalId;
        emit modernControllerChanged();
    }
}

bool InputEngine::desktopCanReceiveInput() const
{
    if (!m_desktopFocused)
        return false;

    // Raw Input is registered with RIDEV_INPUTSINK, so button events arrive
    // even while a game owns focus. Treat QML's cached window.active only as
    // a hint and verify the real Win32 foreground window before emitting any
    // desktop-gallery action such as Cross -> lightbox.openAt().
    HWND foreground = GetForegroundWindow();
    if (!foreground)
        return false;

    DWORD processId = 0;
    GetWindowThreadProcessId(foreground, &processId);
    return processId == GetCurrentProcessId();
}

bool InputEngine::anyBackendConnected() const
{
    return m_sonyConnected || m_xinputConnected || m_winmmConnected;
}

// The pre-0.7.3 hold threshold lived under `input.share_hold_ms`. Carry a real
// override across once and never look again; a user who never changed it has
// nothing to carry, and inventing an override for them would freeze today's
// default into their config.
void InputEngine::migrateLegacyHoldSetting()
{
    const auto migrated = GestureTiming::migratedHoldMs(
        !m_config->isDefault(ConfigKeys::InputDefaultHoldMs),
        !m_config->isDefault(ConfigKeys::InputShareHoldMs),
        m_config->value(ConfigKeys::InputShareHoldMs, 2000).toInt());
    if (!migrated)
        return;
    m_config->setValue(ConfigKeys::InputDefaultHoldMs, *migrated);
    m_config->resetValue(ConfigKeys::InputShareHoldMs);
    qInfo() << "Input: migrated hold threshold" << *migrated
            << "ms from input.share_hold_ms";
}

void InputEngine::applyGestureTiming()
{
    const auto timing = GestureTiming::fromValues(
        m_config->value(ConfigKeys::InputDefaultHoldMs, 2000).toInt(),
        m_config->value(ConfigKeys::InputMultiTapIntervalMs, 300).toInt(),
        m_config->value(ConfigKeys::InputChordWindowMs, 300).toInt());
    m_runtime->setDefaultHoldMs(timing.defaultHoldMs);
    m_runtime->setTiming(timing);
    const QString description = GestureTiming::describe(timing);
    if (description == m_lastTimingDescription)
        return;
    m_lastTimingDescription = description;
    qInfo() << "Input: gesture timing —" << description;
    InputDiagnostics::instance().setGestureTiming(description);
}
