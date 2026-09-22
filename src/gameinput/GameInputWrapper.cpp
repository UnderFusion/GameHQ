#include "gameinput/GameInputWrapper.h"

#include "gameinput/GameInputFocusController.h"
#include <QDebug>

namespace ModernInput {

void GameInputWrapper::setFocusController(GameInputFocusController* controller)
{
    m_focusController = controller;
}

GameInputWrapper::GameInputWrapper(std::unique_ptr<IGameInputApi> api,
                                   int queueCapacity, int emergencyReserve,
                                   QObject* parent)
    : QObject(parent)
    , m_api(std::move(api))
    , m_queueCapacity(queueCapacity)
    , m_emergencyReserve(emergencyReserve)
    , m_queue(std::make_shared<GameInputEventQueue>(queueCapacity, emergencyReserve))
{
}

GameInputWrapper::~GameInputWrapper()
{
    shutdown();
}

QString GameInputWrapper::runtimeDescription() const
{
    QString description = m_api ? m_api->runtimeDescription() : QStringLiteral("Unavailable");
    if (m_running && !systemButtonsAvailable())
        description += QStringLiteral("; Guide/Share callback unavailable (existing providers only)");
    return description;
}

bool GameInputWrapper::start(QString& error)
{
    if (m_running)
        return true;
    if (!m_api || !m_api->initialize(error))
        return false;

    // Focus policy must be in place before the first callback registration —
    // otherwise the runtime may withhold background Share/Guide edges that
    // arrive while the game (not GameHQ) holds focus.
    //
    // cpo-o06c: when a controller owns the policy, attaching it is what applies
    // that start-up background policy; the ordering contract is unchanged.
    if (m_focusController)
        m_focusController->attach(m_api.get());
    else
        m_api->applyFocusPolicy(GameInputFocusMode::Background);

    // A previous shutdown() permanently stops the old queue so late callbacks
    // stay harmless. A restart (Auto → Off → Auto) therefore needs a fresh
    // queue; stale callbacks still hold only the old, stopped instance.
    if (!m_queue->accepting())
        m_queue = std::make_shared<GameInputEventQueue>(m_queueCapacity,
                                                        m_emergencyReserve);

    m_dispatcher = std::make_unique<GameInputQtDispatcher>(m_queue);
    connect(m_dispatcher.get(), &GameInputQtDispatcher::eventsReady,
            this, &GameInputWrapper::eventsReady);
    m_dispatcher->start();

    const std::weak_ptr<GameInputEventQueue> weakQueue(m_queue);
    const auto sink = [weakQueue](GameInputEvent event) {
        if (auto queue = weakQueue.lock())
            queue->push(std::move(event));
    };

    m_deviceRegistration = {m_api.get(), m_api->registerDeviceCallback(sink)};
    m_readingRegistration = {m_api.get(), m_api->registerReadingCallback(sink)};
    if (!m_deviceRegistration.valid() || !m_readingRegistration.valid()) {
        error = QStringLiteral("GameInput callback registration failed.");
        shutdown();
        return false;
    }
    // Guide/Share is a separate, optional registration. Keep ordinary readings,
    // device identity and the focus-policy owner alive if only this path fails.
    m_systemRegistration = {m_api.get(), m_api->registerSystemButtonCallback(sink)};
    if (!systemButtonsAvailable())
        qWarning() << "GameInput degraded: Guide/Share callback unavailable; ordinary input and focus policy retained; existing system-button providers remain eligible";
    m_running = true;
    return true;
}

void GameInputWrapper::shutdown()
{
    if (!m_api)
        return;

    // cpo-o06c: release the focus policy first, while the runtime is still alive
    // to receive the call. A stopped or restarted session must never inherit the
    // exclusive state an interactive overlay asked for.
    if (m_focusController)
        m_focusController->detach();

    // Required order: stop/unregister every callback, release retained device
    // state, stop the queue bridge, then release/unload the runtime.
    m_systemRegistration.reset();
    m_readingRegistration.reset();
    m_deviceRegistration.reset();
    m_api->releaseDevices();
    if (m_dispatcher) {
        m_dispatcher->shutdown();
        m_dispatcher.reset();
    } else {
        m_queue->stopAccepting();
    }
    m_api->unload();
    m_running = false;
}

} // namespace ModernInput
