#pragma once

#include "gameinput/GameInputEventQueue.h"

#include <QObject>

#include <atomic>
#include <memory>
#include <thread>

namespace ModernInput {

// The worker is the only bridge from the plain callback queue to Qt. GameInput
// callbacks never receive or dereference this QObject.
class GameInputQtDispatcher final : public QObject
{
    Q_OBJECT
public:
    explicit GameInputQtDispatcher(std::shared_ptr<GameInputEventQueue> queue,
                                   QObject* parent = nullptr);
    ~GameInputQtDispatcher() override;

    void start();
    void shutdown();
    // Deliveries posted to the GUI thread and not yet run: at most one.
    int pendingDeliveries() const { return m_pendingDeliveries.load(std::memory_order_acquire); }

signals:
    void eventsReady(const ModernInput::GameInputEventBatch& batch);

private:
    void workerLoop();
    void drain();

    std::shared_ptr<GameInputEventQueue> m_queue;
    std::atomic_bool m_running{false};
    std::atomic_int m_pendingDeliveries{0};
    std::thread m_worker;
};

} // namespace ModernInput

