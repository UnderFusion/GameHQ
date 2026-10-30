// InputEngine: controller backend arbitration (which legacy backend owns the
// controller role, and the candidate-press buffer that lets the role move).
// Split out of InputEngine.cpp; the class itself is declared in InputEngine.h.
#include "input/InputEngine.h"

#include "input/BindingEditorModel.h"
#include "input/BindingRuntime.h"
#include "input/ControlId.h"
#include "input/ControllerArbitration.h"
#include "input/DualSenseDevice.h"
#include "input/Gamepad.h"
#include "input/InputDiagnostics.h"
#include "input/WinMMDevice.h"
#include "input/XInputDevice.h"

#include <QDebug>
#include <QTimer>

#include <limits>

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
    ++m_candidateRuns;
    publishControllerRouting();
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
        publishControllerRouting();
        return;
    }
    const auto activeIt = m_backendLastControlMs.constFind(m_activeBackend);
    qint64 activeLastMs = activeIt != m_backendLastControlMs.cend()
        ? *activeIt : std::numeric_limits<qint64>::min();
    const auto releases = m_backendLastReleaseMs.constFind(m_activeBackend);
    if (releases != m_backendLastReleaseMs.cend()) {
        const auto released = releases->constFind(m_pending.controlId);
        if (released != releases->cend())
            activeLastMs = qMax(activeLastMs, *released);
    }
    if (!ControllerArbitration::heldPressSurvives(m_pending.pressedMs, activeLastMs,
                                                  m_controllerClock.elapsed())) {
        clearPendingCandidate();   // the active backend answered after all
        publishControllerRouting();
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
        // The buffered press entered the capability router too. Close that
        // cycle before releasing the recognizer, or the next real PS/Share
        // press is mistaken for an echo of a button still held.
        if ((press.controlId == ControlId::Capture || press.controlId == ControlId::Guide)
            && !routeLegacySystemEdge(press.source, press.fingerprint,
                                      press.controlId, false))
            return;
        releaseControllerControl(canonicalProfile(press.source, press.fingerprint),
                                 press.controlId);
        stopNavRepeatOnRelease(press.controlId);
    }
}

void InputEngine::activateBackend(Gamepad* pick, const QString& reason)
{
    if (pick == m_activeBackend)
        return;
    stopNavRepeat();
    if (pick)
        m_runtime->cancelForBackendSwitch();
    else
        m_runtime->cancelAll(); // No surviving controller source.
    m_legacyViewFallbackHeld.clear();
    m_activeBackend = pick;
    m_lastBackendSwitchReason = reason;

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
    publishControllerRouting();
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
