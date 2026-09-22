#pragma once

#include "gameinput/GameInputQtDispatcher.h"
#include "gameinput/IGameInputApi.h"

#include <QObject>

#include <memory>

namespace ModernInput {

class GameInputFocusController;

class GameInputWrapper final : public QObject
{
    Q_OBJECT
public:
    explicit GameInputWrapper(std::unique_ptr<IGameInputApi> api,
                              int queueCapacity = 256,
                              int emergencyReserve = 64,
                              QObject* parent = nullptr);
    ~GameInputWrapper() override;

    // cpo-o06c: the process-wide focus policy has one owner
    // (GameInputFocusController). Install one before start(); the wrapper then
    // attaches it to the runtime session it creates — which applies the
    // start-up background policy before any callback registration — and detaches
    // it on shutdown, while the runtime is still alive to receive the restore.
    // Without one, the wrapper applies the start-up background policy itself.
    void setFocusController(GameInputFocusController* controller);

    bool start(QString& error);
    void shutdown();
    bool running() const { return m_running; }
    QString runtimeDescription() const;

signals:
    void eventsReady(const ModernInput::GameInputEventBatch& batch);

private:
    std::unique_ptr<IGameInputApi> m_api;
    GameInputFocusController* m_focusController = nullptr;
    const int m_queueCapacity;
    const int m_emergencyReserve;
    std::shared_ptr<GameInputEventQueue> m_queue;
    std::unique_ptr<GameInputQtDispatcher> m_dispatcher;
    GameInputCallbackRegistration m_deviceRegistration;
    GameInputCallbackRegistration m_readingRegistration;
    GameInputCallbackRegistration m_systemRegistration;
    bool m_running = false;
};

} // namespace ModernInput
