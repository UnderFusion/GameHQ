#include "gameinput/GameInputQtDispatcher.h"

#include <QMetaObject>

namespace ModernInput {

GameInputQtDispatcher::GameInputQtDispatcher(
    std::shared_ptr<GameInputEventQueue> queue, QObject* parent)
    : QObject(parent), m_queue(std::move(queue))
{
    qRegisterMetaType<GameInputEventBatch>();
}

GameInputQtDispatcher::~GameInputQtDispatcher()
{
    shutdown();
}

void GameInputQtDispatcher::start()
{
    bool expected = false;
    if (!m_running.compare_exchange_strong(expected, true))
        return;
    m_worker = std::thread([this] { workerLoop(); });
}

void GameInputQtDispatcher::shutdown()
{
    if (!m_running.exchange(false))
        return;
    m_queue->stopAccepting();
    if (m_worker.joinable())
        m_worker.join();
}

// The worker never takes events itself. It only tells the GUI thread that
// the queue has work, at most once until that drain runs, so a stalled GUI
// leaves the backlog in the bounded queue (where coalescing and overflow
// recovery apply) instead of in an unbounded run of queued callbacks.
void GameInputQtDispatcher::workerLoop()
{
    while (m_running.load(std::memory_order_acquire)) {
        if (!m_queue->waitForDrainRequest())
            break;
        m_pendingDeliveries.fetch_add(1, std::memory_order_acq_rel);
        // Dropped with this object if it is destroyed first.
        QMetaObject::invokeMethod(this, [this] { drain(); }, Qt::QueuedConnection);
    }
}

void GameInputQtDispatcher::drain()
{
    m_pendingDeliveries.fetch_sub(1, std::memory_order_acq_rel);
    if (!m_running.load(std::memory_order_acquire))
        return;
    // One take per event-loop turn: everything pending, which the queue
    // bounds to its capacity plus reserve and one recovery per device.
    const GameInputEventBatch batch = m_queue->takeForDrain();
    if (batch.events.isEmpty() && !batch.forceResync)
        return;
    emit eventsReady(batch);
}

} // namespace ModernInput

