#include "input/InputEngine.h"

#include "config/ConfigKeys.h"
#include "config/ConfigManager.h"
#include "input/BindingRuntime.h"
#include "input/ControllerArbitration.h"
#include "input/ControllerIdentity.h"
#include "input/BindingEditorModel.h"
#include "input/DualSenseDevice.h"
#include "input/GestureTiming.h"
#include "input/Gamepad.h"
#include "input/HidCloakMonitor.h"
#include "input/HotkeyManager.h"
#include "input/EventLoopStallMonitor.h"
#include "input/InputDiagnostics.h"
#include "input/MouseHookDevice.h"
#include "input/MouseMonitorPolicy.h"
#include "input/WinMMDevice.h"
#include "input/XInputDevice.h"
#include "input/SelectiveRawHidFallback.h"
#include "input/RawHidBindingCatalog.h"
#include "gameinput/GameInputRouter.h"
#include "gameinput/ProductionGameInputApi.h"
#include "storage/CaptureDatabase.h"

#include <QDebug>
#include <QClipboard>
#include <QGuiApplication>
#include <QKeySequence>
#include <QSet>
#include <QTimer>
#include <QVariantMap>

#include <windows.h>

#include <limits>

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
    migrateLegacyHoldSetting();
    applyGestureTiming();
    connect(m_config, &ConfigManager::valueChanged, this,
            [this](const QString& key, const QVariant&) {
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
        applyGestureTiming();
        reloadBindings();
    });

    connect(m_runtime.get(), &BindingRuntime::actionTriggered,
            this, &InputEngine::dispatchAction);
    if (m_hotkeys) {
        connect(m_hotkeys, &HotkeyManager::hotkeyTriggered, this,
                [this](const QString& actionId) { dispatchAction(actionId); });
    }
    connect(m_mouse.get(), &MouseHookDevice::buttonPressed, this,
            [this](const QString& code, int generation) {
                // A press queued from the hook thread can arrive after the
                // hook was stopped — or stopped AND restarted, which an
                // isRunning() check would wrongly accept. Anything from a
                // previous hook lifetime is dropped: its release was never
                // captured, so acting on it would arm a gesture no release
                // will ever end.
                if (generation != m_mouse->generation())
                    return;
                QString label = code;
                if (code == MouseHookDevice::ButtonBack) label = QStringLiteral("Mouse Back");
                else if (code == MouseHookDevice::ButtonForward) label = QStringLiteral("Mouse Forward");
                else if (code == MouseHookDevice::ButtonMiddle) label = QStringLiteral("Middle Mouse");
                if (m_bindingEditor->captureInput(QStringLiteral("mouse"), code, label))
                    return;
                m_runtime->press(QStringLiteral("mouse"), {}, code,
                                 primaryScope(), fallbackScope());
            });
    connect(m_mouse.get(), &MouseHookDevice::buttonReleased, this,
            [this](const QString& code, int) {
                // Releases process regardless of generation: releasing an
                // unpressed control is a safe no-op, dropping a real one
                // risks a stuck control.
                m_runtime->release(QStringLiteral("mouse"), {}, code);
                if (code == m_repeatTrigger)
                    stopNavRepeat();
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
                InputDiagnostics::instance().noteControl(control, QStringLiteral("GameInput"));
                m_bindingEditor->noteObservedControl(control);
                const auto family = ControlId::ControllerFamily::Generic;
                const auto* logical = m_providers.registry().controller(logicalId);
                m_bindingEditor->setControllerProfile({
                    QStringLiteral("GameInput"), logicalId, family,
                    displayName, logical ? logical->modelFingerprint : QString()});
                if (m_bindingEditor->captureInput(QStringLiteral("controller"), control,
                                                  ControlId::label(control, family)))
                    return;
                m_runtime->press(QStringLiteral("controller"), gameInputProfile(logicalId),
                                 control, primaryScope(), fallbackScope());
                setLastInput((displayName.isEmpty() ? QStringLiteral("Controller") : displayName)
                             + QStringLiteral(": ") + ControlId::label(control, family));
            });
    connect(m_gameInput.get(), &ModernInput::GameInputRouter::systemControlReleased,
            this, [this, gameInputProfile](const QString& control, const QString& logicalId,
                         const QString&) {
                m_runtime->release(QStringLiteral("controller"), gameInputProfile(logicalId),
                                   control);
            });
    connect(m_gameInput.get(), &ModernInput::GameInputRouter::sessionFallback,
            this, [this](const QString&) {
                stopNavRepeat();
                m_runtime->cancelAll();
                m_legacyViewFallbackHeld.clear();
            });
    connect(m_gameInput.get(), &ModernInput::GameInputRouter::lifecycleReset,
            this, [this](const QString&, const QString&) {
                stopNavRepeat();
                m_runtime->cancelAll();
                m_legacyViewFallbackHeld.clear();
            });
    connect(m_gameInput.get(), &ModernInput::GameInputRouter::logicalControllerRekeyed,
            this, [this](const QString& previous, const QString& logicalId) {
                configureLogicalProfile(logicalId, {previous});
            });
    connect(m_gameInput.get(), &ModernInput::GameInputRouter::statusChanged,
            this, &InputEngine::modernControllerChanged);
    connect(m_gameInput.get(), &ModernInput::GameInputRouter::deviceConnected,
            this, [this](const QString&, bool) { emit modernControllerChanged(); });
    connect(m_gameInput.get(), &ModernInput::GameInputRouter::deviceDisconnected,
            this, [this](const QString&) { emit modernControllerChanged(); });

    // The Raw Input backend sees every HID arrival/removal (debounced),
    // including XInput and DirectInput devices. Use it as the hot-plug
    // trigger for the polling backends so they detect new pads immediately
    // without continuously probing empty slots.
    connect(m_sonyPad, &DualSenseDevice::deviceTopologyChanged, this, [this] {
        qInfo() << "Input: device topology changed — rescanning fallback backends";
        m_xinputPad->rescan();
        m_winmmPad->rescan();
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
                    if (m_bindingEditor->captureInput(QStringLiteral("controller"),
                                                      control,
                                                      ControlId::label(control, family)))
                        return;
                    m_runtime->press(QStringLiteral("controller"), logicalId, control,
                                     primaryScope(), fallbackScope());
                } else {
                    m_runtime->release(QStringLiteral("controller"), logicalId, control);
                    if (control == m_repeatTrigger)
                        stopNavRepeat();
                }
            });
    connect(m_sonyPad, &DualSenseDevice::rawHidDeviceRemoved, this,
            [this](const QString& identity) {
                const QString logicalId = m_providers.registry().logicalIdFor(
                    ModernInput::ControllerProvider::RawHid, identity);
                const QStringList releases = m_providers.removeRawHid(identity);
                for (const QString& control : releases) {
                    m_runtime->release(QStringLiteral("controller"), logicalId, control);
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
                        tr("%1 is connected but hidden from applications by the "
                           "HidHide driver (installed with DSX, DS4Windows, or "
                           "reWASD).").arg(names),
                        true);
                } else {
                    setControllerWarning(
                        tr("%1 is connected to Windows but invisible to "
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
    // Run the final controller reset while BindingRuntime and the shared
    // ProviderIntegration are still alive. GameInputRouter's destructor is a
    // no-op after this ordered shutdown.
    if (m_gameInput)
        m_gameInput->shutdown();
}

void InputEngine::retranslate()
{
    m_bindingEditor->retranslate();
}

QObject* InputEngine::bindingEditor() const
{
    return m_bindingEditor.get();
}

void InputEngine::start()
{
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
    if (!autoRepeat)
        ModernInput::SelectiveRawHidFallback::instance().observeKeyboard(trigger);
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
    return m_runtime->press(QStringLiteral("keyboard"), {}, trigger,
                            primaryScope(), fallbackScope());
}

bool InputEngine::handleKeyReleased(int key, int modifiers)
{
    const QString trigger = keyboardTrigger(key, modifiers);
    const bool handled = m_runtime->release(QStringLiteral("keyboard"), {}, trigger);
    if (trigger == m_repeatTrigger)
        stopNavRepeat();
    return handled;
}

void InputEngine::attachGamepad(std::unique_ptr<Gamepad> pad, const QString& displayName)
{
    Gamepad* raw = pad.get();
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
}

void InputEngine::removeLegacyBackend(Gamepad* pad, const QString& providerDeviceId)
{
    QSet<QString> observed = m_legacyObservedIds.value(pad);
    if (!providerDeviceId.isEmpty())
        observed.intersect(QSet<QString>{providerDeviceId});
    for (const QString& id : observed) {
        const QString logicalId = canonicalProfile(pad, id);
        const QStringList releases =
            m_providers.removeLegacy(providerFor(pad), id);
        for (const QString& control : releases)
            m_runtime->release(QStringLiteral("controller"), logicalId, control);
        m_legacyObservedIds[pad].remove(id);
    }
    if (m_legacyObservedIds.value(pad).isEmpty())
        m_legacyObservedIds.remove(pad);
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
}

bool InputEngine::routeLegacySystemEdge(Gamepad* source,
                                        const QString& providerDeviceId,
                                        const QString& controlId, bool pressed)
{
    const auto result = m_providers.routeLegacySystemEdge(
        providerFor(source), providerDeviceId, controlId, pressed,
        quint64(m_controllerClock.elapsed()));
    for (const QString& release : result.safeReleases)
        m_runtime->release(QStringLiteral("controller"),
                           canonicalProfile(source, providerDeviceId), release);
    return result.accepted;
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

// Keep the current backend while it remains connected. Sony > XInput > WinMM
// is only the initial/fallback choice; real control activity may move the
// active role later. This avoids a stale Raw Input or virtual-device path
// suppressing the backend a game is actually using.
void InputEngine::updateActiveBackend()
{
    if (backendConnected(m_activeBackend))
        return;

    Gamepad* pick = nullptr;
    if (m_sonyConnected)
        pick = m_sonyPad;
    else if (m_xinputConnected)
        pick = m_xinputPad;
    else if (m_winmmConnected)
        pick = m_winmmPad;

    activateBackend(pick, QStringLiteral("connection fallback"));
}

// A backend the takeover gate refused is tracked as a pending candidate: the
// role still cannot move on one event, but it no longer has to wait out a full
// second of silence either. The run restarts the moment the active backend
// reports again, which is what makes this safe against mirrored input — a
// remapper feeding two APIs keeps both of them talking.
bool InputEngine::confirmCandidate(Gamepad* source, qint64 now,
                                   qint64 activeLastControlMs)
{
    const auto firstIt = m_backendCandidateFirstMs.constFind(source);
    if (firstIt == m_backendCandidateFirstMs.cend() || *firstIt <= activeLastControlMs) {
        m_backendCandidateFirstMs.insert(source, now);
        return false;
    }
    if (!ControllerArbitration::candidateMayConfirm(*firstIt, now, activeLastControlMs))
        return false;
    m_backendCandidateFirstMs.remove(source);
    return true;
}

// Buffer the first press of a candidate run. XInput and WinMM report state
// *changes*, so a user who presses once and lets go produces exactly one event
// — waiting for a second one would lose that press forever. The press is held
// for BackendCandidateConfirmMs and then either delivered (active backend
// stayed silent: this was a real switch) or dropped (active backend spoke: it
// was a mirror). Only one press is ever held; a further control from the same
// candidate resolves the run immediately through confirmCandidate().
void InputEngine::holdCandidatePress(Gamepad* source, const QString& controlId,
                                     int family, const QString& fingerprint,
                                     qint64 pressedMs)
{
    if (m_pending.source == source && !m_pending.controlId.isEmpty())
        return;   // already holding this candidate's first press
    m_pending.source = source;
    m_pending.controlId = controlId;
    m_pending.family = family;
    m_pending.fingerprint = fingerprint;
    m_pending.pressedMs = pressedMs;
    m_pending.released = false;
    const int generation = ++m_pendingGeneration;
    QTimer::singleShot(ControllerArbitration::BackendCandidateConfirmMs, this,
                       [this, generation] { resolvePendingCandidate(generation); });
}

void InputEngine::clearPendingCandidate()
{
    ++m_pendingGeneration;   // orphan the timer still counting down
    m_pending = {};
}

// The confirmation window closed with the candidate still unanswered: the
// active backend never spoke, so the held press was real input on a backend
// that has genuinely taken over.
void InputEngine::resolvePendingCandidate(int generation)
{
    if (generation != m_pendingGeneration || !m_pending.source)
        return;
    Gamepad* source = m_pending.source;
    if (!backendConnected(source)) {
        clearPendingCandidate();
        return;
    }
    const auto activeIt = m_backendLastControlMs.constFind(m_activeBackend);
    const qint64 activeLastMs = activeIt != m_backendLastControlMs.cend()
        ? *activeIt : std::numeric_limits<qint64>::min();
    if (!ControllerArbitration::heldPressSurvives(m_pending.pressedMs, activeLastMs,
                                                  m_controllerClock.elapsed())) {
        clearPendingCandidate();   // the active backend answered after all
        return;
    }

    const PendingPress press = m_pending;
    clearPendingCandidate();
    m_backendCandidateFirstMs.clear();
    activateBackend(source, QStringLiteral("sustained candidate"));
    m_backendLastControlMs.insert(source, m_controllerClock.elapsed());
    replayPendingPress(press);
}

void InputEngine::replayPendingPress(const PendingPress& press)
{
    deliverPress(press.source, press.controlId, press.family, press.fingerprint);
    // Replayed back to back so a tap stays a tap: the runtime measures hold
    // time from the press it just saw, and this release arrives immediately.
    if (press.released) {
        m_runtime->release(QStringLiteral("controller"),
                           canonicalProfile(press.source, press.fingerprint),
                           press.controlId);
        if (press.controlId == m_repeatTrigger)
            stopNavRepeat();
    }
}

void InputEngine::activateBackend(Gamepad* pick, const QString& reason)
{
    if (pick == m_activeBackend)
        return;
    stopNavRepeat();
    m_runtime->cancelAll();
    m_legacyViewFallbackHeld.clear();
    m_activeBackend = pick;

    if (pick) {
        auto profile = pick->profile();
        profile.fingerprint = canonicalProfile(pick, profile.fingerprint);
        m_bindingEditor->setControllerProfile(profile);
        const QString name = backendDisplayName(pick);
        InputDiagnostics::instance().noteBackendSwitch(name, reason);
        qInfo() << "Input: active controller backend ->" << name
                << "(" << reason << ")";
        setControllerStatus(name + QStringLiteral(" connected"));
        setLastInput(name + QStringLiteral(" connected"));
    } else {
        m_bindingEditor->setControllerProfile({});
        qInfo() << "Input: no controller backend connected";
        setControllerStatus(QStringLiteral("No controller detected"));
    }
}

bool InputEngine::backendConnected(const Gamepad* pad) const
{
    if (pad == m_sonyPad)
        return m_sonyConnected;
    if (pad == m_xinputPad)
        return m_xinputConnected;
    if (pad == m_winmmPad)
        return m_winmmConnected;
    return false;
}

QString InputEngine::backendDisplayName(const Gamepad* pad) const
{
    if (pad == m_sonyPad)
        return QStringLiteral("Sony controller");
    if (pad == m_xinputPad)
        return QStringLiteral("XInput controller");
    if (pad == m_winmmPad)
        return QStringLiteral("WinMM joystick");
    return QStringLiteral("Controller");
}

int InputEngine::backendPriority(const Gamepad* pad) const
{
    if (pad == m_sonyPad)
        return 3;
    if (pad == m_xinputPad)
        return 2;
    if (pad == m_winmmPad)
        return 1;
    return 0;
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

void InputEngine::setOverlayVisible(bool visible)
{
    if (m_overlayVisible == visible)
        return;
    m_overlayVisible = visible;
    if (!visible)
        m_playbackActive = false;
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

void InputEngine::onControlPressed(const QString& controlId, int family,
                                   const QString&, const QString& fingerprint,
                                   const QString&)
{
    auto* source = qobject_cast<Gamepad*>(sender());
    if (!source || !backendConnected(source))
        return;
    const auto currentProfile = source->profile();
    if (currentProfile.fingerprint == fingerprint)
        observeLegacyBackend(source, currentProfile);

    const qint64 now = m_controllerClock.elapsed();
    if (source != m_activeBackend) {
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
            if (now - *activeIt <= ControllerArbitration::BackendDuplicateWindowMs)
                return;
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
    setLastInput(ControlId::label(controlId, static_cast<ControlId::ControllerFamily>(family))
                 + QStringLiteral(" pressed"));
    if (m_bindingEditor->captureInput(
            QStringLiteral("controller"), controlId,
            ControlId::label(controlId, static_cast<ControlId::ControllerFamily>(family))))
        return;
    const bool handled = m_runtime->press(QStringLiteral("controller"), logicalProfile, controlId,
                                          primaryScope(), fallbackScope());
    // XInput Back/View is now independently bindable. For profiles that have
    // no explicit View/Back pattern yet, preserve the historic built-in
    // Capture gestures as a capability fallback. As soon as the user binds
    // View/Back itself, that stable meaning wins and the alias is not entered.
    if (!handled && controlId == ControlId::ViewBack
        && m_providers.allowsLegacyViewFallback(
            providerFor(source), fingerprint, hasExplicitViewBinding(logicalProfile))) {
        if (m_runtime->press(QStringLiteral("controller"), logicalProfile,
                             ControlId::Capture, primaryScope(), fallbackScope()))
            m_legacyViewFallbackHeld.insert(logicalProfile);
    }
}

void InputEngine::onControlReleased(const QString& controlId, int, const QString&,
                                    const QString& fingerprint, const QString&)
{
    auto* source = qobject_cast<Gamepad*>(sender());
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
    if (source != m_activeBackend)
        return;
    if ((controlId == ControlId::Capture || controlId == ControlId::Guide)
        && !routeLegacySystemEdge(source, fingerprint, controlId, false))
        return;
    const QString logicalProfile = canonicalProfile(source, fingerprint);
    m_runtime->release(QStringLiteral("controller"), logicalProfile, controlId);
    if (controlId == ControlId::ViewBack
        && m_legacyViewFallbackHeld.remove(logicalProfile))
        m_runtime->release(QStringLiteral("controller"), logicalProfile, ControlId::Capture);
    if (controlId == m_repeatTrigger)
        stopNavRepeat();
}

void InputEngine::startNavRepeat(const QString& triggerCode, int direction,
                                 std::function<void(int)> emitter)
{
    // First, fire immediately so a quick tap still does one step.
    emitter(direction);
    // Record what we're now repeating — stopNavRepeat() uses m_repeatButton.
    m_repeatTrigger = triggerCode;
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

void InputEngine::stopNavRepeat()
{
    m_repeatTick->stop();
    m_repeatTrigger.clear();
    m_repeatDirection = 0;
    m_repeatEmitter = {};
}

ActionCatalog::Scope InputEngine::primaryScope() const
{
    if (m_playbackActive)
        return ActionCatalog::Scope::Playback;
    if (m_overlayVisible)
        return ActionCatalog::Scope::Overlay;
    if (desktopCanReceiveInput())
        return ActionCatalog::Scope::Desktop;
    return ActionCatalog::Scope::Global;
}

ActionCatalog::Scope InputEngine::fallbackScope() const
{
    if (!m_playbackActive)
        return ActionCatalog::Scope::Global;
    return m_overlayVisible ? ActionCatalog::Scope::Overlay
                            : ActionCatalog::Scope::Desktop;
}

void InputEngine::dispatchAction(const QString& actionId, const QString& triggerCode)
{
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

void InputEngine::fixHiddenController()
{
    if (m_fixProcess)
        return;   // helper already in flight

    void* process = HidCloakMonitor::launchElevatedWhitelistHelper();
    if (!process) {
        setControllerWarning(
            tr("Administrator approval was declined — the controller stays "
               "hidden. You can whitelist GameHQ manually in the HidHide "
               "Configuration Client."),
            true);
        return;
    }
    m_fixProcess = process;
    setControllerWarning(tr("Applying the fix (administrator prompt)..."), false);

    if (!m_fixWatch) {
        m_fixWatch = new QTimer(this);
        m_fixWatch->setInterval(500);
        connect(m_fixWatch, &QTimer::timeout, this, [this] {
            HANDLE h = static_cast<HANDLE>(m_fixProcess);
            if (WaitForSingleObject(h, 0) != WAIT_OBJECT_0)
                return;   // still running
            DWORD code = 1;
            GetExitCodeProcess(h, &code);
            CloseHandle(h);
            m_fixProcess = nullptr;
            m_fixWatch->stop();
            if (code == 0) {
                qInfo() << "Input: GameHQ whitelisted in HidHide";
                setControllerWarning(
                    tr("GameHQ is now whitelisted in HidHide. Unplug and replug "
                       "the controller if it does not appear within a few "
                       "seconds."),
                    false);
                m_sonyPad->rescan();
            } else {
                qWarning() << "Input: HidHide whitelist helper failed, code" << code;
                setControllerWarning(
                    tr("Automatic whitelisting failed (code %1). Add GameHQ.exe "
                       "on the Applications tab of the HidHide Configuration "
                       "Client instead.").arg(code),
                    true);
            }
        });
    }
    m_fixWatch->start();
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
