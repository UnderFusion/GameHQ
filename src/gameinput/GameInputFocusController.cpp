#include "gameinput/GameInputFocusController.h"

#include "gameinput/IGameInputApi.h"
#include "input/InputDiagnostics.h"

#include <QDebug>

namespace ModernInput {

GameInputFocusController::GameInputFocusController(QObject* parent)
    : QObject(parent)
{
}

GameInputFocusController::~GameInputFocusController() = default;

void GameInputFocusController::attach(IGameInputApi* api)
{
    if (m_api == api)
        return;
    m_api = api;
    // A fresh runtime session always starts from the background policy, whatever
    // the previous session ended in: the exclusive state is scoped to one
    // interactive overlay, never to the process.
    apply(GameInputFocusMode::Background, QStringLiteral("runtime-attached"));
}

void GameInputFocusController::detach()
{
    if (!m_api)
        return;
    // Restore first, then drop the pointer: the runtime is asked to go back to
    // the background policy while it is still alive to receive the call. This is
    // what makes "the controller/runtime went away" unable to leave GameHQ
    // exclusive.
    apply(GameInputFocusMode::Background, QStringLiteral("runtime-stopped"));
    m_api = nullptr;
}

bool GameInputFocusController::requestExclusiveForeground(const QString& reason)
{
    if (!m_api) {
        ++m_refusedRequests;
        qInfo().noquote() << QStringLiteral(
            "GameInput focus policy: exclusive-foreground refused — no runtime attached (%1)")
            .arg(reason);
        return false;
    }
    if (m_mode == GameInputFocusMode::ExclusiveForeground) {
        // Already in force: re-issuing the same flags would be noise, and a
        // repeated open must not accumulate policy applications.
        ++m_duplicateRequests;
        return true;
    }
    apply(GameInputFocusMode::ExclusiveForeground, reason);
    return true;
}

void GameInputFocusController::restoreBackground(const QString& reason)
{
    if (!m_api)
        return;
    if (m_mode == GameInputFocusMode::Background) {
        ++m_duplicateRequests;
        return;
    }
    apply(GameInputFocusMode::Background, reason);
}

void GameInputFocusController::apply(GameInputFocusMode mode, const QString& reason)
{
    // The single place in GameHQ that changes the runtime's focus policy. The
    // flags themselves are decided in ProductionGameInputApi::applyFocusPolicy;
    // this records the transition (mode + reason) so the export shows a policy
    // timeline rather than only the policy in force at the moment it was written.
    m_api->applyFocusPolicy(mode);
    m_mode = mode;
    ++m_transitionCount;
    m_lastReason = reason;
    InputDiagnostics::instance().noteGameInputFocusTransition(gameInputFocusModeName(mode), reason);
    qInfo().noquote() << QStringLiteral("GameInput focus policy -> %1 (%2)")
                             .arg(gameInputFocusModeName(mode), reason);
    emit focusPolicyTransitioned(gameInputFocusModeName(mode), reason);
}

}  // namespace ModernInput
